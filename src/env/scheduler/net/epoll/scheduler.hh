#ifndef ENV_SCHEDULER_NET_EPOLL_SCHEDULER_HH
#define ENV_SCHEDULER_NET_EPOLL_SCHEDULER_HH

#include <list>
#include <bits/move_only_function.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <arpa/inet.h>

#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"

#include "../common/struct.hh"
#include "../common/detail.hh"
#include "../common/error.hh"
#include "./epoll.hh"

namespace pump::scheduler::net::epoll {

    struct
    epoll_send_cache {
        core::mpsc::queue<common::send_req*,1024> send_q;

        auto
        release() {
            while (auto opt = send_q.try_dequeue()) {
                opt.value()->cb(false);
                delete opt.value();
            }
        }
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
            if (!join_q.try_enqueue(req)) {
                req->cb(false);
                delete req;
            }
        }

        auto
        schedule(common::stop_req* req) {
            if (!stop_q.try_enqueue(req)) {
                req->cb(false);
                delete req;
            }
        }

        // 3.8: add return after exception path
        auto
        schedule(common::recv_req* req) {
            auto* s = reinterpret_cast<session_t*>(req->session_id);
            if (s->status.load() != common::detail::session_status::normal) [[unlikely]] {
                req->cb(std::make_exception_ptr(common::session_closed_error()));
                delete req;
                return;
            }

            if (common::detail::has_full_pkt(&recv_cache(s)->buf)) {
                req->cb(&(recv_cache(s)->buf));
                delete req;
            }
            else {
                auto tmp = static_cast<common::recv_req*>(nullptr);
                if (!recv_cache(s)->req.compare_exchange_strong(tmp, req)) [[unlikely]] {
                    req->cb(std::make_exception_ptr(common::duplicate_recv_error()));
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
            if (!send_cache(s)->send_q.try_enqueue(req)) {
                req->cb(false);
                delete req;
            }
        }

        // 3.10: clean up close_session logic
        void
        close_session(session_t* s) {
            s->close();
        }

        // 3.2: use session_t instead of session<reader>
        auto
        handle_join_req() {
            while (auto opt = join_q.try_dequeue()) {
                auto* s = reinterpret_cast<session_t*>(opt.value()->session_id);
                auto event = new epoll_event;
                event->data.ptr = s;
                event->events = EPOLLIN | EPOLLOUT | EPOLLET;

                if (poller.add_event(s->fd, event) != -1)[[likely]] {
                    opt.value()->cb(true);
                }
                else {
                    delete event;
                    opt.value()->cb(false);
                }

                delete opt.value();
            }
        }

        // 3.2 + 3.7: use session_t, fix close_session flow
        auto
        handle_stop_req() {
            while (auto opt = stop_q.try_dequeue()) {
                auto* s = reinterpret_cast<session_t*>(opt.value()->session_id);
                close_session(s);
                delete s;
                opt.value()->cb(true);
                delete opt.value();
            }
        }

        // 3.3 + 3.4: fix readv call and add forward_tail
        auto
        handle_read_event(epoll_event* e) {
            auto* s = static_cast<session_t*>(e->data.ptr);
            auto& buf = recv_cache(s)->buf;
            int iovcnt = buf.make_iovec();
            if (auto res = readv(s->fd, buf.iov(), iovcnt); res > 0) {
                // 3.4: update tail after read
                buf.forward_tail(res);
                if (auto req = recv_cache(s)->req.exchange(nullptr); req != nullptr) {
                    req->cb(&buf);
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

        // 3.5: batch processing + writev return value check
        auto
        handle_write_event(epoll_event* e) {
            auto *s = static_cast<session_t*>(e->data.ptr);
            while (auto opt = send_cache(s)->send_q.try_dequeue()) {
                auto *r = opt.value();
                auto res = writev(s->fd, r->vec, r->cnt);
                if (res < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        r->cb(false);
                        delete r;
                        return;
                    }
                    r->cb(false);
                    delete r;
                    return;
                }
                r->cb(true);
                delete r;
            }
        }

        auto
        handle_events() {
            const int cnt = poller.wait_event(0);
            for (int i = 0; i < cnt; ++i) {
                if (poller.events[i].data.ptr == nullptr)
                    continue;
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

        template<typename runtime_t>
        auto
        advance(const runtime_t&) {
            return advance();
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
        int listen_fd = -1;
    private:
        auto
        schedule(common::conn_req* req) {
            if (auto opt = session_q.try_dequeue();opt) {
                req->cb(opt.value());
                delete req;
            }
            else {
                if (!conn_request_q.try_enqueue(req)) {
                    req->cb(0);
                    delete req;
                }
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

        // 3.6: server socket initialization
        int
        init(const char* address, uint16_t port) {
            listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (listen_fd < 0)
                return -1;

            int opt = 1;
            setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            inet_pton(AF_INET, address, &addr.sin_addr);

            if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
                ::close(listen_fd);
                listen_fd = -1;
                return -1;
            }

            if (listen(listen_fd, SOMAXCONN) < 0) {
                ::close(listen_fd);
                listen_fd = -1;
                return -1;
            }

            auto* ev = new epoll_event;
            ev->events = EPOLLIN;
            ev->data.fd = listen_fd;
            poller.add_event(listen_fd, ev);

            return 0;
        }

        ~accept_scheduler() {
            if (listen_fd >= 0)
                ::close(listen_fd);
        }

        auto
        advance() {
            const int cnt = poller.wait_event(0);
            for (int i = 0; i < cnt; ++i) {
                handle_new_connections(poller.events[i].data.fd);
            }
            return true;
        }

        template<typename runtime_t>
        auto
        advance(const runtime_t&) {
            return advance();
        }
    };
}

#endif //ENV_SCHEDULER_NET_EPOLL_SCHEDULER_HH
