#ifndef ENV_SCHEDULER_NET_EPOLL_SCHEDULER_HH
#define ENV_SCHEDULER_NET_EPOLL_SCHEDULER_HH

#include <list>
#include <bits/move_only_function.h>
#include <liburing.h>
#include <netinet/in.h>

#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"

#include "../common/struct.hh"
#include "../common/detail.hh"
#include "./epoll.hh"

namespace pump::scheduler::net::epoll {

    struct
    epoll_send_cache {
        core::mpsc::queue<common::send_req*,1024> send_q;
    };

    using session_t = common::detail::internal_session<common::detail::recv_cache, epoll_send_cache>;

    enum class
    epoll_read_res {
        read_more,
        wait_next_event,
        socket_closed,
        socket_error
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
        detail::poller_epoll poller;

        core::mpsc::queue<common::join_req*, 2048> join_q;
        core::mpsc::queue<common::stop_req*, 2048> stop_q;
    private:
        static
        auto
        recv_cache(session_t* s) noexcept{
            return std::get<0>(s->impls);
        }

        static
        auto
        send_cache(session_t* s) noexcept{
            return std::get<1>(s->impls);
        }

        auto
        schedule(common::join_req* req) {
            return join_q.try_enqueue(req);
        }

        auto
        schedule(common::stop_req* req) {
            return stop_q.try_enqueue(req);
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

        auto
        schedule(common::send_req* req) {
            auto* s = reinterpret_cast<session_t*>(req->session_id);
            if (s->status.load() != common::detail::session_status::normal) [[unlikely]] {
                req->cb(false);
                delete req;
                return;
            }
            send_cache(s)->send_q.try_enqueue(req);
        }

        void
        close_session(epoll_event* e) {
            auto s = static_cast<session_t*>(e->data.ptr);
            poller.del_event(s->fd, e);
            s->close();
            e->data.ptr = nullptr;
            delete e;
        }

        auto
        handle_join_req() {
            while (auto opt = join_q.try_dequeue()) {
                auto* s = reinterpret_cast<common::detail::session<common::detail::reader>*>(opt.value()->session_id);
                auto event = new epoll_event;
                event->data.ptr = s;
                s->user_data = event;
                event->events = EPOLLIN | EPOLLET;

                if (poller.add_event(s->fd, event) != -1)[[likely]] {
                    opt.value()->cb(true);
                }
                else {
                    opt.value()->cb(false);
                }

                delete opt.value();
            }
        }

        auto
        handle_stop_req() {
            while (auto opt = stop_q.try_dequeue()) {
                auto* s = reinterpret_cast<common::detail::session<common::detail::reader>*>(opt.value()->session_id);
                auto* e = static_cast<epoll_event*>(s->user_data);
                poller.del_event(s->fd, e);
                delete e;
                close_session(s);
                delete s;
                opt.value()->cb(true);
                delete opt.value();
            }
        }

        auto
        handle_read_event(epoll_event* e) {
            auto* s = static_cast<session_t*>(e->data.ptr);
            if (auto res = readv(s->fd, recv_cache(s)->data.make_iovec(), 2, 0); res > 0) {
                if (auto req = recv_cache(s)->req->exchange(nullptr); req != nullptr) {
                    req->cb(&(recv_cache(s)->buf));
                    delete req;
                }
            }
            else if (res == 0) {
                close_session(s);
            }
            else {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return;
                close_session(s);
            }
        }

        auto
        handle_write_event(epoll_event* e) {
            auto *s = static_cast<session_t*>(e->data.ptr);
            if (auto opt = send_cache(s)->send_q.try_dequeue()) {
                auto *r = opt.value();
                writev(s->fd,r->vec,r->cnt);
                r->cb(true);
                delete r;
            }
        }

        auto
        handle_events() {
            const int cnt = poller.wait_event(0);
            for (int i = 0; i < cnt; ++i) {
                if (poller.events[i].events & (EPOLLERR | EPOLLHUP)) {
                    close_session(static_cast<session_t*>(poller.events[i].data.ptr));
                }
                else {
                    if (poller.events[i].events & EPOLLIN)
                        handle_read_event(&poller.events[i]);
                    if (poller.events[i].events & EPOLLOUT)
                        handle_write_event(&poller.events[i]);
                }
            }
        }


    public:
        session_scheduler()
            : poller(){
        }

        auto
        advance() {
            handle_join_req();
            handle_events();
            handle_stop_req();
            return true;
        }
    };


    template <template<typename> class conn_op_t>
    struct
    accept_scheduler {
        friend struct conn_op_t<accept_scheduler>;
    private:
        core::mpsc::queue<common::conn_req*, 2048> conn_request_q;
        core::mpmc::queue<uint64_t, 2048> session_q;
        detail::poller_epoll poller;
    private:
        auto
        schedule(common::conn_req* req) {
            if (auto opt = session_q.try_dequeue();opt) {
                req->cb(opt.value());
                delete req;
            }
            else {
                conn_request_q.try_enqueue(req);
            }
        }

        auto
        create_internal_session(int fd) {
            const int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            auto* s = new session_t(fd, new common::detail::recv_cache(4096), new epoll_send_cache());
            if (const auto opt = conn_request_q.try_dequeue(); opt) {
                opt.value()->cb(reinterpret_cast<uint64_t>(s));
                delete opt.value();
            }
            else {
                session_q.try_enqueue(reinterpret_cast<uint64_t>(s));
            }
        }

        auto
        handle_new_connections(int fd) {
            sockaddr in_addr{};
            socklen_t in_len = sizeof(in_addr);
            if (const auto conn_sock = accept(fd, &in_addr, &in_len); conn_sock > 0) [[likely]]
                create_internal_session(conn_sock);
        }



    public:
        accept_scheduler()
            : poller() {
        }

        auto
        advance() {
            const int cnt = poller.wait_event(0);
            for (int i = 0; i < cnt; ++i) {
                handle_new_connections(poller.events[i].data.fd);
            }
            return true;
        }
    };
}

#endif //ENV_SCHEDULER_NET_EPOLL_SCHEDULER_HH