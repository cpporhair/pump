

#ifndef PUMP_NET_IOURING_SCHEDULER_HH
#define PUMP_NET_IOURING_SCHEDULER_HH

#include <cstdint>
#include <list>
#include <bits/move_only_function.h>
#include <liburing.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <arpa/inet.h>

#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"

#include "../common/struct.hh"
#include "../common/detail.hh"
#include "../common/error.hh"

namespace pump::scheduler::net::io_uring {

    // 6.1: unified session type - recv-only (io_uring manages sends via ring, no per-session send cache)
    using session_t = common::detail::internal_session<common::detail::recv_cache>;

    enum struct
    uring_event_type {
        accept  = 0,
        read    = 1,
        write   = 2
    };

    struct
    io_uring_request {
        uring_event_type event_type;
        void *user_data;
    };

    template <template<typename> class conn_op_t>
    struct
    accept_scheduler {
        friend struct conn_op_t<accept_scheduler>;
    private:
        int server_socket = -1;
        struct ::io_uring ring{};
        core::mpsc::queue<common::conn_req*, 2048> request_q;
        // 6.3: use session_id_t instead of raw uint64_t
        core::mpmc::queue<common::session_id_t, 2048> conn_fd_q;
        size_t _recv_buffer_size = 4096;

        // 6.4: shutdown flag
        std::atomic<bool> _shutdown{false};
    private:
        // 4.2: fix req leak when immediate connection available; 6.3: use session_id_t
        auto
        schedule(common::conn_req* req) {
            if (const auto opt = conn_fd_q.try_dequeue(); opt) {
                req->cb(opt.value());
                delete req;
            }
            else {
                if (!request_q.try_enqueue(req)) {
                    req->cb(common::session_id_t{});
                    delete req;
                }
            }
        }

        // 6.3: encode session pointer as tagged session_id_t
        bool
        maybe_accept(const int fd) {
            if (const auto opt = request_q.try_dequeue(); opt) {
                const int flags = fcntl(fd, F_GETFL, 0);
                fcntl(fd, F_SETFL, flags | O_NONBLOCK);
                auto s = new session_t(fd, new common::detail::recv_cache(_recv_buffer_size));
                auto sid = common::session_id_t::encode(s);
                opt.value()->cb(sid);
                delete opt.value();
                return true;
            }
            return false;
        }

        auto
        add_accept_request(
            int socket,
            sockaddr_in *client_addr,
            socklen_t *client_addr_len
        ) {
            ::io_uring_sqe *sqe = ::io_uring_get_sqe(&ring);
            ::io_uring_prep_accept(sqe, socket, reinterpret_cast<struct sockaddr *>(client_addr),
                                 client_addr_len, 0);
            auto *req = new io_uring_request;
            req->event_type = uring_event_type::accept;
            ::io_uring_sqe_set_data(sqe, req);
            ::io_uring_submit(&ring);

            return 0;
        }

        // 4.3: add break to prevent fallthrough; 4.4: fix new/free mismatch
        // 6.3: encode accepted fd as session_id_t when no pending request
        auto
        handle_io_uring() {
            sockaddr_in client_addr{};
            socklen_t client_addr_len = sizeof(client_addr);
            ::io_uring_cqe *cqe;
            add_accept_request(server_socket, &client_addr, &client_addr_len);
            while (::io_uring_peek_cqe(&ring, &cqe) == 0) {
                if (cqe->res < 0)[[unlikely]]
                    break;
                auto *uring_req = reinterpret_cast<io_uring_request *>(cqe->user_data);

                switch (uring_req->event_type) {
                    case uring_event_type::accept:
                        if (!maybe_accept(cqe->res)) {
                            const int flags = fcntl(cqe->res, F_GETFL, 0);
                            fcntl(cqe->res, F_SETFL, flags | O_NONBLOCK);
                            auto s = new session_t(cqe->res, new common::detail::recv_cache(_recv_buffer_size));
                            conn_fd_q.try_enqueue(common::session_id_t::encode(s));
                        }
                        delete uring_req;
                        break;
                    default:
                        delete uring_req;
                        break;
                }

                ::io_uring_cqe_seen(&ring, cqe);
            }
        }

        // 6.4: drain pending requests on shutdown
        void
        drain_on_shutdown() {
            while (auto opt = request_q.try_dequeue()) {
                opt.value()->cb(common::session_id_t{});
                delete opt.value();
            }
        }

    public:
        accept_scheduler() = default;

        // 6.2: unified init with scheduler_config
        int
        init(const common::scheduler_config& cfg) {
            _recv_buffer_size = cfg.recv_buffer_size;
            return init(cfg.address, cfg.port, cfg.queue_depth);
        }

        // 4.1: server socket + ring initialization
        int
        init(const char* address, uint16_t port, unsigned queue_depth = 256) {
            server_socket = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (server_socket < 0)
                return -1;

            int opt = 1;
            setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            inet_pton(AF_INET, address, &addr.sin_addr);

            if (bind(server_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
                ::close(server_socket);
                server_socket = -1;
                return -1;
            }

            if (listen(server_socket, SOMAXCONN) < 0) {
                ::close(server_socket);
                server_socket = -1;
                return -1;
            }

            if (::io_uring_queue_init(queue_depth, &ring, 0) < 0) {
                ::close(server_socket);
                server_socket = -1;
                return -1;
            }

            return 0;
        }

        ~accept_scheduler() {
            if (server_socket >= 0) {
                ::close(server_socket);
                ::io_uring_queue_exit(&ring);
            }
        }

        // 6.4: graceful shutdown
        void
        shutdown() {
            _shutdown.store(true, std::memory_order_release);
        }

        auto
        advance() {
            if (_shutdown.load(std::memory_order_acquire)) [[unlikely]] {
                drain_on_shutdown();
                return false;
            }
            handle_io_uring();
            return true;
        }

        template<typename runtime_t>
        auto
        advance(const runtime_t&) {
            return advance();
        }
    };

    template<
        template<typename> class join_op_t,
        template<typename> class recv_op_t,
        template<typename> class send_op_t,
        template<typename> class stop_op_t
    >
    struct
    session_scheduler {
        friend struct join_op_t<session_scheduler>;
        friend struct recv_op_t<session_scheduler>;
        friend struct send_op_t<session_scheduler>;
        friend struct stop_op_t<session_scheduler>;
    private:
        core::mpsc::queue<common::join_req*, 2048> join_q;
        core::mpsc::queue<common::send_req*, 2048> send_q;
        core::mpsc::queue<common::stop_req*, 2048> stop_q;
        std::list<common::send_req*> send_list;
        struct ::io_uring ring{};

        // 6.4: shutdown flag
        std::atomic<bool> _shutdown{false};
    private:
        // 6.1: named accessor
        static
        auto
        recv_cache(session_t* s) noexcept{
            return s->get_recv_cache();
        }

        auto
        schedule(common::join_req* req) noexcept {
            if (!join_q.try_enqueue(req)) {
                req->cb(false);
                delete req;
            }
        }

        auto
        schedule(common::send_req* req) noexcept {
            if (!send_q.try_enqueue(req)) {
                req->cb(false);
                delete req;
            }
        }

        // 4.8: change stop from synchronous to async via queue
        auto
        schedule(common::stop_req* req) noexcept {
            if (!stop_q.try_enqueue(req)) {
                req->cb(false);
                delete req;
            }
        }

        // 4.7: add return after exception path; 6.3: use session_id_t::decode
        auto
        schedule(common::recv_req* req) {
            auto* s = req->session_id.decode<session_t>();
            if (!s || s->status.load() != common::detail::session_status::normal) [[unlikely]] {
                req->cb(std::make_exception_ptr(common::session_closed_error()));
                delete req;
                return;
            }

            // check ready_q first (frames already copied out but not yet consumed)
            if (auto opt = recv_cache(s)->ready_q.try_dequeue()) {
                req->cb(std::move(*opt.value()));
                delete opt.value();
                delete req;
            }
            else if (auto frame = common::detail::copy_out_frame(&recv_cache(s)->buf); frame.size() > 0) {
                req->cb(std::move(frame));
                delete req;
            }
            else {
                if (!recv_cache(s)->recv_q.try_enqueue(req)) [[unlikely]] {
                    req->cb(std::make_exception_ptr(common::session_closed_error()));
                    delete req;
                }
            }
        }

        void
        on_read_event(const io_uring_request *iur, int res) {
            auto s = static_cast<session_t*>(iur->user_data);
            // update tail after read
            recv_cache(s)->buf.forward_tail(res);
            for (auto frame = common::detail::copy_out_frame(&recv_cache(s)->buf);
                 frame.size() > 0;
                 frame = common::detail::copy_out_frame(&recv_cache(s)->buf)) {
                if (auto opt = recv_cache(s)->recv_q.try_dequeue()) {
                    opt.value()->cb(std::move(frame));
                    delete opt.value();
                } else {
                    // no waiting recv, stash frame for later consumption
                    recv_cache(s)->ready_q.try_enqueue(new common::recv_frame(std::move(frame)));
                }
            }
        }

        void
        on_write_event(const io_uring_request *iur) {
            auto r = static_cast<common::send_req*>(iur->user_data);
            r->cb(true);
            delete r;
        }

        void
        close_session(session_t* s) {
            s->close();
        }

        void
        close_session(io_uring_request* iur) {
            close_session(static_cast<session_t *>(iur->user_data));
        }

        // 4.10: add break + delete for write case; 6.3: use session_id_t::decode
        void
        process_err(::io_uring_cqe *cqe) {
            switch (auto *uring_req = reinterpret_cast<io_uring_request *>(cqe->user_data); uring_req->event_type) {
                case uring_event_type::read: {
                    auto s = static_cast<session_t*>(uring_req->user_data);
                    // notify all pending recv callbacks before closing session
                    while (auto opt = recv_cache(s)->recv_q.try_dequeue()) {
                        opt.value()->cb(std::make_exception_ptr(common::session_closed_error()));
                        delete opt.value();
                    }
                    close_session(s);
                    delete uring_req;
                    break;
                }
                case uring_event_type::write: {
                    auto r = static_cast<common::send_req*>(uring_req->user_data);
                    auto s = r->session_id.decode<session_t>();
                    if (s) close_session(s);
                    r->cb(false);
                    delete r;
                    delete uring_req;
                    break;
                }
                default:
                    delete uring_req;
                    break;
            }
        }

        void
        process_cqe(::io_uring_cqe *cqe) {
            switch (auto *uring_req = reinterpret_cast<io_uring_request *>(cqe->user_data); uring_req->event_type) {
                case uring_event_type::read: {
                    auto s = static_cast<session_t*>(uring_req->user_data);
                    if (cqe->res == 0) [[unlikely]] {
                        // EOF: client closed connection gracefully
                        while (auto opt = recv_cache(s)->recv_q.try_dequeue()) {
                            opt.value()->cb(std::make_exception_ptr(common::session_closed_error()));
                            delete opt.value();
                        }
                        close_session(s);
                        delete uring_req;
                        break;
                    }
                    on_read_event(uring_req, cqe->res);
                    if (s->status.load() == common::detail::session_status::normal)[[likely]] {
                        submit_read(uring_req, s);
                    }
                    else {
                        close_session(uring_req);
                        delete uring_req;
                    }
                    break;
                }
                case uring_event_type::write: {
                    on_write_event(uring_req);
                    delete uring_req;
                    break;
                }
                default:
                    delete uring_req;
                    break;
            }
        }

        void
        handle_io() {
            while (true) {
                switch (::io_uring_cqe *cqe = nullptr; ::io_uring_peek_cqe(&ring, &cqe)) {
                    case 0: [[likely]]
                        if (cqe->res >= 0) [[likely]]
                            process_cqe(cqe);
                        else
                            process_err(cqe);
                        ::io_uring_cqe_seen(&ring, cqe);
                        break;
                    case -EAGAIN:
                        return;
                    default:
                        ::io_uring_queue_exit(&ring);
                        return;
                }
            }
        }

        // 4.11: use member iovec (no leak since make_iovec uses _iov member)
        void
        submit_read(io_uring_request *io_req, session_t* s) {
            ::io_uring_sqe *sqe = ::io_uring_get_sqe(&ring);
            io_req->event_type = uring_event_type::read;
            io_req->user_data = s;
            int iovcnt = recv_cache(s)->buf.make_iovec();
            ::io_uring_prep_readv(sqe, s->fd, recv_cache(s)->buf.iov(), iovcnt, 0);
            ::io_uring_sqe_set_data(sqe, io_req);
            ::io_uring_submit(&ring);
        }

        void
        submit_read(session_t* s) {
            return submit_read(new io_uring_request, s);
        }

        // 4.9: call join callback before delete; 6.3: use session_id_t::decode
        void
        handle_join_request() {
            while (auto opt = join_q.try_dequeue()) {
                auto* s = opt.value()->session_id.decode<session_t>();
                if (!s) [[unlikely]] {
                    opt.value()->cb(false);
                    delete opt.value();
                    continue;
                }
                submit_read(s);
                opt.value()->cb(true);
                delete opt.value();
            }
        }

        // 4.8: handle stop requests asynchronously in advance; 6.3: use session_id_t::decode
        void
        handle_stop_request() {
            while (auto opt = stop_q.try_dequeue()) {
                auto* s = opt.value()->session_id.decode<session_t>();
                if (!s) [[unlikely]] {
                    opt.value()->cb(false);
                    delete opt.value();
                    continue;
                }
                s->status.store(common::detail::session_status::closed);
                ::close(s->fd);
                opt.value()->cb(true);
                delete opt.value();
            }
        }

        // 6.3: use session_id_t::decode
        auto
        submit_write(::io_uring_sqe *sqe, common::send_req* req) {
            const auto s = req->session_id.decode<session_t>();
            req->prepare_frame();
            auto *io_req = new io_uring_request;
            io_req->event_type = uring_event_type::write;
            io_req->user_data = req;
            ::io_uring_sqe_set_data(sqe, io_req);
            if (s) {
                ::io_uring_prep_writev(sqe, s->fd, req->_send_vec, req->_send_cnt, 0);
            } else {
                // session decode 失败，提交 NOP 避免未定义行为
                ::io_uring_prep_nop(sqe);
            }
        }

        // 4.12: add io_uring_submit after processing send requests
        void
        handle_send_request() {
            bool submitted = false;
            while (!send_list.empty()) {
                if (::io_uring_sqe *sqe = ::io_uring_get_sqe(&ring)) {
                    common::send_req* req = send_list.front();
                    send_list.pop_front();
                    submit_write(sqe, req);
                    submitted = true;
                }
                else {
                    break;
                }
            }
            while (auto opt = send_q.try_dequeue()) {
                if (::io_uring_sqe *sqe = ::io_uring_get_sqe(&ring)) {
                    submit_write(sqe, opt.value());
                    submitted = true;
                }
                else {
                    send_list.push_back(opt.value());
                    break;
                }
            }
            if (submitted) {
                ::io_uring_submit(&ring);
            }
        }

        // 6.4: drain all pending queues with error on shutdown
        void
        drain_on_shutdown() {
            while (auto opt = join_q.try_dequeue()) {
                opt.value()->cb(false);
                delete opt.value();
            }
            while (auto opt = stop_q.try_dequeue()) {
                opt.value()->cb(false);
                delete opt.value();
            }
            while (auto opt = send_q.try_dequeue()) {
                opt.value()->cb(false);
                delete opt.value();
            }
            for (auto* req : send_list) {
                req->cb(false);
                delete req;
            }
            send_list.clear();
        }

    public:
        session_scheduler() = default;

        // 6.2: unified init with scheduler_config
        int
        init(const common::scheduler_config& cfg) {
            return init(cfg.queue_depth);
        }

        // 4.6: ring initialization
        int
        init(unsigned queue_depth = 256) {
            return ::io_uring_queue_init(queue_depth, &ring, 0);
        }

        ~session_scheduler() {
            ::io_uring_queue_exit(&ring);
        }

        // 6.4: graceful shutdown
        void
        shutdown() {
            _shutdown.store(true, std::memory_order_release);
        }

        auto
        advance() {
            if (_shutdown.load(std::memory_order_acquire)) [[unlikely]] {
                drain_on_shutdown();
                return false;
            }
            handle_join_request();
            handle_stop_request();
            handle_send_request();
            handle_io();
            return true;
        }

        template<typename runtime_t>
        auto
        advance(const runtime_t&) {
            return advance();
        }
    };
}

#endif //PUMP_NET_IOURING_SCHEDULER_HH
