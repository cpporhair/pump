
#ifndef PUMP_NET_IOURING_SCHEDULER_HH
#define PUMP_NET_IOURING_SCHEDULER_HH

#include <cstdint>
#include <list>
#include <bits/move_only_function.h>
#include <liburing.h>
#include <netinet/in.h>

#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"

#include "../common/struct.hh"
#include "../common/detail.hh"

namespace pump::scheduler::net::io_uring {

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
        int server_socket = 0;
        struct io_uring ring{};
        core::mpsc::queue<common::conn_req*, 2048> request_q;
        core::mpmc::queue<uint64_t,2048> conn_fd_q;
    private:
        auto
        schedule(common::conn_req* req) {
            if (const auto opt = conn_fd_q.try_dequeue(); opt) {
                req->cb(opt.value());
            }
            else {
                request_q.try_enqueue(req);
            }
        }

        bool
        maybe_accept(const int fd) {
            if (const auto opt = request_q.try_dequeue(); opt) {
                auto s = new session_t(fd, new common::detail::recv_cache(4096));
                opt.value()->cb(reinterpret_cast<uint64_t>(s));
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
            io_uring_sqe *sqe = io_uring_get_sqe(&ring);
            io_uring_prep_accept(sqe, socket, reinterpret_cast<struct sockaddr *>(client_addr),
                                 client_addr_len, 0);
            auto *req = new io_uring_request;
            req->event_type = uring_event_type::accept;
            io_uring_sqe_set_data(sqe, req);
            io_uring_submit(&ring);

            return 0;
        }

        auto
        handle_io_uring() {
            sockaddr_in client_addr{};
            socklen_t client_addr_len = sizeof(client_addr);
            io_uring_cqe *cqe;
            add_accept_request(server_socket, &client_addr, &client_addr_len);
            while (io_uring_peek_cqe(&ring, &cqe) == 0) {
                if (cqe->res < 0)[[unlikely]]
                    break;
                auto *uring_req = reinterpret_cast<io_uring_request *>(cqe->user_data);

                switch (uring_req->event_type) {
                    case uring_event_type::accept:
                        if (maybe_accept(cqe->res))
                            delete uring_req;
                        else
                            conn_fd_q.try_enqueue(cqe->res);
                    default:
                        break;
                }

                free(uring_req);

                io_uring_cqe_seen(&ring, cqe);
            }
        }

    public:
        template<typename runtime_t>
        auto
        advance(const runtime_t& rt) {
            handle_io_uring();
            return true;
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
        std::list<common::send_req*> send_list;
        struct io_uring ring{};
    private:
        static
        auto
        recv_cache(session_t* s) noexcept{
            return std::get<0>(s->impls);
        }

        auto
        schedule(common::join_req* req) noexcept {
            return join_q.try_enqueue(req);
        }

        auto
        schedule(common::send_req* req) noexcept {
            send_q.try_enqueue(req);
        }

        auto
        schedule(common::stop_req* req) noexcept {
            auto* s = reinterpret_cast<session_t*>(req->session_id);
            s->status.store(common::detail::session_status::closed);
            close(s->fd);
            req->cb(true);
        }

        auto
        schedule(common::recv_req* req) {
            auto* s = reinterpret_cast<session_t*>(req->session_id);
            if (s->status.load() != common::detail::session_status::normal) [[unlikely]] {
                req->cb(std::make_exception_ptr(std::logic_error("")));
            }

            if (common::detail::has_full_pkt(&recv_cache(s)->buf)) {
                req->cb(&(recv_cache(s)->buf));
                delete req;
            }
            else {
                auto tmp = static_cast<common::recv_req*>(nullptr);
                if (!recv_cache(s)->req.compare_exchange_strong(tmp, req)) [[unlikely]] {
                    req->cb(std::make_exception_ptr(std::logic_error("")));
                    delete req;
                }
            }
        }

        void
        on_read_event(const io_uring_request *iur) {
            auto s = static_cast<session_t*>(iur->user_data);
            if (auto req = recv_cache(s)->req.exchange(nullptr); req != nullptr) {
                req->cb(&(recv_cache(s)->buf));
                delete req;
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

        void
        process_err(io_uring_cqe *cqe) {
            switch (auto *uring_req = reinterpret_cast<io_uring_request *>(cqe->user_data); uring_req->event_type) {
                case uring_event_type::read: {
                    auto s = static_cast<session_t*>(uring_req->user_data);
                    close_session(s);
                    delete uring_req;
                    break;
                }
                case uring_event_type::write: {
                    auto r = static_cast<common::send_req*>(uring_req->user_data);
                    auto s = reinterpret_cast<session_t*>(r->session_id);
                    close_session(s);
                    r->cb(false);
                    delete r;
                }
                default:
                    break;
            }
        }

        void
        process_cqe(io_uring_cqe *cqe) {
            switch (auto *uring_req = reinterpret_cast<io_uring_request *>(cqe->user_data); uring_req->event_type) {
                case uring_event_type::read: {
                    on_read_event(uring_req);
                    auto s = static_cast<session_t*>(uring_req->user_data);
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
                    break;
            }
        }

        void
        handle_io() {
            while (true) {
                switch (io_uring_cqe *cqe = nullptr; io_uring_peek_cqe(&ring, &cqe)) {
                    case 0: [[likely]]
                        if (cqe->res >= 0) [[likely]]
                            process_cqe(cqe);
                        else
                            process_err(cqe);
                        io_uring_cqe_seen(&ring, cqe);
                        break;
                    case -EAGAIN:
                        return;
                    default:
                        io_uring_queue_exit(&ring);
                        return;
                }
            }
        }

        void
        submit_read(io_uring_request *io_req, session_t* s) {
            io_uring_sqe *sqe = io_uring_get_sqe(&ring);
            io_req->event_type = uring_event_type::read;
            io_req->user_data = s;
            io_uring_prep_readv(sqe, s->fd, recv_cache(s)->buf.make_iovec(), 2, 0);
            io_uring_sqe_set_data(sqe, io_req);
            io_uring_submit(&ring);
        }

        void
        submit_read(session_t* s) {
            return submit_read(new io_uring_request, s);
        }

        void
        handle_join_request() {
            while (auto opt = join_q.try_dequeue()) {
                submit_read(reinterpret_cast<session_t*>(opt.value()->session_id));
                delete opt.value();
            }
        }

        auto
        submit_write(io_uring_sqe *sqe, common::send_req* req) {
            const auto s = reinterpret_cast<session_t*>(req->session_id);
            auto *io_req = new io_uring_request;
            io_req->event_type = uring_event_type::write;
            io_req->user_data = req;
            io_uring_sqe_set_data(sqe, io_req);
            io_uring_prep_writev(sqe, s->fd, req->vec, req->cnt, 0);
        }

        void
        handle_send_request() {
            while (!send_list.empty()) {
                if (io_uring_sqe *sqe = io_uring_get_sqe(&ring)) {
                    common::send_req* req = send_list.front();
                    send_list.pop_front();
                    submit_write(sqe, req);
                }
                else {
                    return;
                }
            }
            while (auto opt = send_q.try_dequeue()) {
                if (io_uring_sqe *sqe = io_uring_get_sqe(&ring)) {
                    submit_write(sqe, opt.value());
                }
                else {
                    send_list.push_back(opt.value());
                    return;;
                }
            }
        }

    public:
        template<typename runtime_t>
        auto
        advance(const runtime_t& rt) {
            handle_join_request();
            handle_send_request();
            handle_io();
            return true;
        }
    };
}

#endif //PUMP_NET_IOURING_SCHEDULER_HH
