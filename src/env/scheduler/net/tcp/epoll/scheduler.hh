
#ifndef ENV_SCHEDULER_TCP_EPOLL_SCHEDULER_HH
#define ENV_SCHEDULER_TCP_EPOLL_SCHEDULER_HH

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
#include "../common/layers.hh"
#include "../common/error.hh"
#include "../senders/join.hh"
#include "../senders/stop.hh"
#include "./epoll.hh"
#include "env/scheduler/net/common/session_tags.hh"
#include "env/scheduler/net/common/send_sender.hh"

namespace pump::scheduler::tcp::epoll {

    enum class
    epoll_read_res {
        read_more,
        wait_next_event,
        socket_closed,
        socket_error
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

        detail::poller_epoll poller;

        core::per_core::queue<join_req_type*> join_q{2048};
        core::per_core::queue<common::send_req*> send_q{2048};
        core::per_core::queue<stop_req_type*> stop_q{2048};
        std::list<common::send_req*> send_list;

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

        session_scheduler()
            : poller(){
        }

        explicit session_scheduler(size_t queue_depth)
            : poller(), join_q(queue_depth), send_q(queue_depth), stop_q(queue_depth) {
        }

        void
        shutdown() {
            _shutdown.store(true, std::memory_order_release);
        }

        void
        schedule_join(join_req_type* req) {
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
            handle_join_req();
            handle_stop_req();
            handle_send_request();
            handle_events();
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

        auto
        handle_join_req() {
            join_q.drain([this](join_req_type* req) {
                auto* s = req->session;
                if (!s || s->invoke(common::get_status) != common::session_status::normal) [[unlikely]] {
                    req->cb(false);
                    delete req;
                    return;
                }
                epoll_event event{};
                event.data.ptr = s;
                event.events = EPOLLIN | EPOLLET;

                auto fd = s->invoke(common::get_fd);
                if (poller.add_event(fd, &event) != -1) [[likely]] {
                    req->cb(true);
                }
                else {
                    req->cb(false);
                }

                delete req;
            });
        }

        auto
        handle_stop_req() {
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

        // EPOLLET requires reading until EAGAIN
        auto
        handle_read_event(epoll_event* e) {
            auto* s = static_cast<session_t*>(e->data.ptr);
            while (true) {
                auto [iov, iovcnt] = s->invoke(common::get_read_iov);
                auto fd = s->invoke(common::get_fd);
                auto res = readv(fd, iov, iovcnt);
                if (res > 0) {
                    s->invoke(common::on_recv, static_cast<int>(res));
                    if (s->invoke(common::get_status) != common::session_status::normal) [[unlikely]] {
                        s->invoke(::pump::scheduler::net::do_close);
                        e->data.ptr = nullptr;
                        return;
                    }
                    continue;
                }
                if (res == 0) {
                    handle_session_error(s, std::make_exception_ptr(common::session_closed_error()));
                    e->data.ptr = nullptr;
                    return;
                }
                // res < 0
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return;
                handle_session_error(s, std::make_exception_ptr(common::session_closed_error()));
                e->data.ptr = nullptr;
                return;
            }
        }

        // Send: try writev immediately; if EAGAIN, keep in send_list for retry
        void
        handle_send_request() {
            send_q.drain([this](common::send_req* req) {
                send_list.push_back(req);
            });
            auto it = send_list.begin();
            while (it != send_list.end()) {
                auto* r = *it;
                auto res = writev(r->fd, r->_send_vec, static_cast<int>(r->_send_cnt));
                if (res < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        ++it;
                        continue;
                    }
                    r->cb(false);
                    delete r;
                    it = send_list.erase(it);
                    continue;
                }
                size_t expected = 0;
                for (size_t i = 0; i < r->_send_cnt; ++i)
                    expected += r->_send_vec[i].iov_len;
                r->cb(static_cast<size_t>(res) == expected);
                delete r;
                it = send_list.erase(it);
            }
        }

        auto
        handle_events() {
            const int cnt = poller.wait_event(0);
            for (int i = 0; i < cnt; ++i) {
                if (poller.events[i].data.ptr == nullptr)
                    continue;
                if (poller.events[i].events & (EPOLLERR | EPOLLHUP)) {
                    auto* s = static_cast<session_t*>(poller.events[i].data.ptr);
                    handle_session_error(s, std::make_exception_ptr(common::session_closed_error()));
                }
                else {
                    if (poller.events[i].events & EPOLLIN)
                        handle_read_event(&poller.events[i]);
                }
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


    // accept_scheduler: accepts connections, delivers raw fd to upper layer.
    // Does NOT create sessions — upper layer is responsible for session lifecycle.
    template <template<typename> class conn_op_t>
    struct
    accept_scheduler {
        friend struct conn_op_t<accept_scheduler>;
    private:
        core::per_core::queue<common::conn_req*> conn_request_q{2048};
        core::mpmc::queue<int> conn_fd_q{2048};
        detail::poller_epoll poller;
        int listen_fd = -1;

        std::atomic<bool> _shutdown{false};
    private:
        auto
        schedule(common::conn_req* req) {
            if (auto opt = conn_fd_q.try_dequeue(); opt) {
                req->cb(opt.value());
                delete req;
            }
            else {
                if (!conn_request_q.try_enqueue(req)) {
                    req->cb(-1);
                    delete req;
                }
            }
        }

        bool
        maybe_accept(int fd) {
            if (const auto opt = conn_request_q.try_dequeue(); opt) {
                const int flags = fcntl(fd, F_GETFL, 0);
                fcntl(fd, F_SETFL, flags | O_NONBLOCK);
                opt.value()->cb(fd);
                delete opt.value();
                return true;
            }
            return false;
        }

        auto
        handle_new_connections(int fd) {
            sockaddr in_addr{};
            socklen_t in_len = sizeof(in_addr);
            if (const auto conn_sock = accept(fd, &in_addr, &in_len); conn_sock > 0) [[likely]] {
                if (!maybe_accept(conn_sock)) {
                    const int flags = fcntl(conn_sock, F_GETFL, 0);
                    fcntl(conn_sock, F_SETFL, flags | O_NONBLOCK);
                    if (!conn_fd_q.try_enqueue(conn_sock)) [[unlikely]] {
                        ::close(conn_sock);
                    }
                }
            }
        }

        void
        drain_on_shutdown() {
            while (auto opt = conn_request_q.try_dequeue()) {
                opt.value()->cb(-1);
                delete opt.value();
            }
        }

    public:
        accept_scheduler()
            : poller() {
        }

        explicit accept_scheduler(size_t queue_depth)
            : poller(), conn_request_q(queue_depth), conn_fd_q(queue_depth) {
        }

        int
        init(const common::scheduler_config& cfg) {
            return init(cfg.address, cfg.port);
        }

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

            epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.fd = listen_fd;
            poller.add_event(listen_fd, &ev);

            return 0;
        }

        ~accept_scheduler() {
            if (listen_fd >= 0)
                ::close(listen_fd);
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

#endif //ENV_SCHEDULER_TCP_EPOLL_SCHEDULER_HH
