
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

    // 6.1: unified session type with explicit recv and send cache
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

        // 6.4: shutdown flag
        std::atomic<bool> _shutdown{false};
    private:
        // 6.1: named accessor replaces std::get<0>
        static
        auto
        recv_cache(session_t* s) noexcept{
            return s->get_recv_cache();
        }

        // 6.1: named accessor replaces std::get<1>
        static
        auto
        send_cache(session_t* s) noexcept{
            return s->get_send_cache();
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
        // 6.3: use session_id_t::decode with generation validation
        auto
        schedule(common::recv_req* req) {
            auto* s = req->session_id.decode<session_t>();
            if (!s || s->status.load() != common::detail::session_status::normal) [[unlikely]] {
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

        // 6.3: use session_id_t::decode
        auto
        schedule(common::send_req* req) {
            auto* s = req->session_id.decode<session_t>();
            if (!s || s->status.load() != common::detail::session_status::normal) [[unlikely]] {
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

        // 3.2: use session_t; 6.3: use session_id_t::decode
        auto
        handle_join_req() {
            while (auto opt = join_q.try_dequeue()) {
                auto* s = opt.value()->session_id.decode<session_t>();
                if (!s) [[unlikely]] {
                    opt.value()->cb(false);
                    delete opt.value();
                    continue;
                }
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

        // 3.2 + 3.7: use session_t; 6.3: use session_id_t::decode
        auto
        handle_stop_req() {
            while (auto opt = stop_q.try_dequeue()) {
                auto* s = opt.value()->session_id.decode<session_t>();
                if (!s) [[unlikely]] {
                    opt.value()->cb(false);
                    delete opt.value();
                    continue;
                }
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
        }

    public:
        session_scheduler()
            : poller(){
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
        core::mpmc::queue<common::session_id_t, 2048> session_q;
        detail::poller_epoll poller;
        int listen_fd = -1;
        size_t _recv_buffer_size = 4096;

        // 6.4: shutdown flag
        std::atomic<bool> _shutdown{false};
    private:
        // 6.3: use session_id_t
        auto
        schedule(common::conn_req* req) {
            if (auto opt = session_q.try_dequeue();opt) {
                req->cb(opt.value());
                delete req;
            }
            else {
                if (!conn_request_q.try_enqueue(req)) {
                    req->cb(common::session_id_t{});
                    delete req;
                }
            }
        }

        // 6.3: encode session pointer as tagged session_id_t
        auto
        create_internal_session(int fd) {
            const int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            auto* s = new session_t(fd, new common::detail::recv_cache(_recv_buffer_size), new epoll_send_cache());
            auto sid = common::session_id_t::encode(s);
            if (const auto opt = conn_request_q.try_dequeue(); opt) {
                opt.value()->cb(sid);
                delete opt.value();
            }
            else {
                session_q.try_enqueue(sid);
            }
        }

        auto
        handle_new_connections(int fd) {
            sockaddr in_addr{};
            socklen_t in_len = sizeof(in_addr);
            if (const auto conn_sock = accept(fd, &in_addr, &in_len); conn_sock > 0) [[likely]]
                create_internal_session(conn_sock);
        }

        // 6.4: drain pending requests on shutdown
        void
        drain_on_shutdown() {
            while (auto opt = conn_request_q.try_dequeue()) {
                opt.value()->cb(common::session_id_t{});
                delete opt.value();
            }
        }

    public:
        accept_scheduler()
            : poller() {
        }

        // 6.2: unified init with scheduler_config
        int
        init(const common::scheduler_config& cfg) {
            _recv_buffer_size = cfg.recv_buffer_size;
            return init(cfg.address, cfg.port);
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
