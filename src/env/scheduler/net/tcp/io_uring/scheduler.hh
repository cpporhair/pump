

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
#include "../common/layers.hh"
#include "../common/error.hh"
#include "../senders/join.hh"
#include "../senders/stop.hh"
#include "env/scheduler/net/common/session_tags.hh"
#include "env/scheduler/net/common/send_sender.hh"

namespace pump::scheduler::tcp::io_uring {

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

    // accept_scheduler: accepts connections, delivers raw fd to upper layer.
    // Does NOT create sessions — upper layer is responsible for session lifecycle.
    template <template<typename> class conn_op_t>
    struct
    accept_scheduler {
        friend struct conn_op_t<accept_scheduler>;
    private:
        int server_socket = -1;
        struct ::io_uring ring{};
        core::per_core::queue<common::conn_req*> request_q{2048};
        core::mpmc::queue<int> conn_fd_q{2048};
        sockaddr_in _accept_addr{};
        socklen_t _accept_addr_len = sizeof(sockaddr_in);

        std::atomic<bool> _shutdown{false};
    private:
        auto
        schedule(common::conn_req* req) {
            if (const auto opt = conn_fd_q.try_dequeue(); opt) {
                req->cb(opt.value());
                delete req;
            }
            else {
                if (!request_q.try_enqueue(req)) {
                    req->cb(-1);
                    delete req;
                }
            }
        }

        bool
        maybe_accept(const int fd) {
            if (const auto opt = request_q.try_dequeue(); opt) {
                const int flags = fcntl(fd, F_GETFL, 0);
                fcntl(fd, F_SETFL, flags | O_NONBLOCK);
                opt.value()->cb(fd);
                delete opt.value();
                return true;
            }
            return false;
        }

        void
        submit_accept() {
            ::io_uring_sqe *sqe = ::io_uring_get_sqe(&ring);
            if (!sqe) [[unlikely]] return;
            _accept_addr_len = sizeof(sockaddr_in);
            ::io_uring_prep_accept(sqe, server_socket,
                reinterpret_cast<sockaddr *>(&_accept_addr), &_accept_addr_len, 0);
            auto *req = new io_uring_request;
            req->event_type = uring_event_type::accept;
            ::io_uring_sqe_set_data(sqe, req);
            ::io_uring_submit(&ring);
        }

        void
        handle_io_uring() {
            ::io_uring_cqe *cqe;
            while (::io_uring_peek_cqe(&ring, &cqe) == 0) {
                auto *uring_req = reinterpret_cast<io_uring_request *>(cqe->user_data);
                if (cqe->res < 0) [[unlikely]] {
                    delete uring_req;
                    ::io_uring_cqe_seen(&ring, cqe);
                    submit_accept();
                    continue;
                }

                if (!maybe_accept(cqe->res)) {
                    const int flags = fcntl(cqe->res, F_GETFL, 0);
                    fcntl(cqe->res, F_SETFL, flags | O_NONBLOCK);
                    if (!conn_fd_q.try_enqueue(cqe->res)) [[unlikely]] {
                        ::close(cqe->res);
                    }
                }

                delete uring_req;
                ::io_uring_cqe_seen(&ring, cqe);
                submit_accept();
            }
        }

        void
        drain_on_shutdown() {
            while (auto opt = request_q.try_dequeue()) {
                opt.value()->cb(-1);
                delete opt.value();
            }
        }

    public:
        accept_scheduler() = default;

        explicit accept_scheduler(size_t queue_depth)
            : request_q(queue_depth), conn_fd_q(queue_depth) {
        }

        int
        init(const common::scheduler_config& cfg) {
            return init(cfg.address, cfg.port, cfg.queue_depth);
        }

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

            submit_accept();
            return 0;
        }

        ~accept_scheduler() {
            if (server_socket >= 0) {
                ::close(server_socket);
                ::io_uring_queue_exit(&ring);
            }
        }

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

    // session_scheduler: manages IO for composed sessions.
    // Session interacts via invoke-based tag dispatch.
    template<typename factory_t>
    struct
    session_scheduler {
        using factory_type = factory_t;
        using session_t = typename factory_t::template session_type<session_scheduler>;
        using join_req_type = common::join_req<session_t>;
        using stop_req_type = common::stop_req<session_t>;

        core::per_core::queue<join_req_type*> join_q{2048};
        core::per_core::queue<common::send_req*> send_q{2048};
        core::per_core::queue<stop_req_type*> stop_q{2048};
        std::list<common::send_req*> send_list;
        struct ::io_uring ring{};

        std::atomic<bool> _shutdown{false};

    public:
        using address_type = session_t*;

        static uint64_t
        address_raw(address_type addr) {
            return reinterpret_cast<uint64_t>(addr);
        }

        auto
        join(address_type session) {
            return senders::join::sender<session_scheduler, session_t>(this, session);
        }

        auto
        stop(address_type session) {
            return senders::stop::sender<session_scheduler, session_t>(this, session);
        }

        session_scheduler() = default;

        explicit session_scheduler(size_t queue_depth)
            : join_q(queue_depth), send_q(queue_depth), stop_q(queue_depth) {
        }

        int
        init(const common::scheduler_config& cfg) {
            return init(cfg.queue_depth);
        }

        int
        init(unsigned queue_depth = 256) {
            return ::io_uring_queue_init(queue_depth, &ring, 0);
        }

        ~session_scheduler() {
            ::io_uring_queue_exit(&ring);
        }

        void
        shutdown() {
            _shutdown.store(true, std::memory_order_release);
        }

        void
        schedule_join(join_req_type* req) noexcept {
            if (!join_q.try_enqueue(req)) {
                req->cb(false);
                delete req;
            }
        }

        void
        schedule_send(common::send_req* req) noexcept {
            if (!send_q.try_enqueue(req)) {
                req->cb(false);
                delete req;
            }
        }

        void
        schedule_stop(stop_req_type* req) noexcept {
            if (!stop_q.try_enqueue(req)) {
                req->cb(false);
                delete req;
            }
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

    private:
        void
        handle_session_error(session_t* s, std::exception_ptr ex) {
            s->broadcast(::pump::scheduler::net::on_error, ex);
            s->invoke(::pump::scheduler::net::do_close);
        }

        void
        submit_read(io_uring_request *io_req, session_t* s) {
            ::io_uring_sqe *sqe = ::io_uring_get_sqe(&ring);
            io_req->event_type = uring_event_type::read;
            io_req->user_data = s;
            auto [iov, iovcnt] = s->invoke(common::get_read_iov);
            auto fd = s->invoke(common::get_fd);
            ::io_uring_prep_readv(sqe, fd, iov, iovcnt, 0);
            ::io_uring_sqe_set_data(sqe, io_req);
            ::io_uring_submit(&ring);
        }

        void
        submit_read(session_t* s) {
            return submit_read(new io_uring_request, s);
        }

        void
        on_read_event(const io_uring_request *iur, int res) {
            auto s = static_cast<session_t*>(iur->user_data);
            s->invoke(common::on_recv, res);
        }

        void
        on_write_event(const io_uring_request *iur, int res) {
            auto r = static_cast<common::send_req*>(iur->user_data);
            size_t expected = 0;
            for (size_t i = 0; i < r->_send_cnt; ++i)
                expected += r->_send_vec[i].iov_len;
            r->cb(static_cast<size_t>(res) == expected);
            delete r;
        }

        void
        process_err(::io_uring_cqe *cqe) {
            switch (auto *uring_req = reinterpret_cast<io_uring_request *>(cqe->user_data); uring_req->event_type) {
                case uring_event_type::read: {
                    auto s = static_cast<session_t*>(uring_req->user_data);
                    handle_session_error(s, std::make_exception_ptr(common::session_closed_error()));
                    delete uring_req;
                    break;
                }
                case uring_event_type::write: {
                    auto r = static_cast<common::send_req*>(uring_req->user_data);
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
                        handle_session_error(s, std::make_exception_ptr(common::session_closed_error()));
                        delete uring_req;
                        break;
                    }
                    on_read_event(uring_req, cqe->res);
                    if (s->invoke(common::get_status) == common::session_status::normal) [[likely]] {
                        submit_read(uring_req, s);
                    }
                    else {
                        delete uring_req;
                    }
                    break;
                }
                case uring_event_type::write: {
                    on_write_event(uring_req, cqe->res);
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

        void
        handle_join_request() {
            join_q.drain([this](join_req_type* req) {
                auto* s = req->session;
                if (!s || s->invoke(common::get_status) != common::session_status::normal) [[unlikely]] {
                    req->cb(false);
                    delete req;
                    return;
                }
                submit_read(s);
                req->cb(true);
                delete req;
            });
        }

        void
        handle_stop_request() {
            stop_q.drain([this](stop_req_type* req) {
                auto* s = req->session;
                if (!s || s->invoke(common::get_status) != common::session_status::normal) [[unlikely]] {
                    req->cb(false);
                    delete req;
                    return;
                }
                handle_session_error(s, std::make_exception_ptr(common::session_closed_error()));
                req->cb(true);
                delete req;
            });
        }

        auto
        submit_write(::io_uring_sqe *sqe, common::send_req* req) {
            auto *io_req = new io_uring_request;
            io_req->event_type = uring_event_type::write;
            io_req->user_data = req;
            ::io_uring_sqe_set_data(sqe, io_req);
            if (req->fd >= 0) {
                ::io_uring_prep_writev(sqe, req->fd, req->_send_vec, req->_send_cnt, 0);
            } else {
                ::io_uring_prep_nop(sqe);
            }
        }

        void
        handle_send_request() {
            send_q.drain([this](common::send_req* req) {
                send_list.push_back(req);
            });
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
            if (submitted) {
                ::io_uring_submit(&ring);
            }
        }

        void
        drain_on_shutdown() {
            auto fail_and_delete = [](auto* req) { req->cb(false); delete req; };
            join_q.drain(fail_and_delete);
            stop_q.drain(fail_and_delete);
            send_q.drain(fail_and_delete);
            for (auto* req : send_list) {
                req->cb(false);
                delete req;
            }
            send_list.clear();
        }
    };
}

#endif //PUMP_NET_IOURING_SCHEDULER_HH
