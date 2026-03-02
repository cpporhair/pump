#ifndef ENV_SCHEDULER_NET_EPOLL_CONNECT_SCHEDULER_HH
#define ENV_SCHEDULER_NET_EPOLL_CONNECT_SCHEDULER_HH

#include <list>
#include <bits/move_only_function.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <cerrno>

#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"

#include "../common/struct.hh"
#include "../common/detail.hh"
#include "../common/error.hh"
#include "./epoll.hh"
#include "./scheduler.hh"
#include "../senders/connect.hh"

namespace pump::scheduler::net::epoll {

    template <template<typename> class conn_op_t>
    struct
    connect_scheduler {
        friend struct conn_op_t<connect_scheduler>;
        friend struct senders::connect::op<connect_scheduler>;
    private:
        struct pending_connect_info {
            int fd;
            common::connect_req* req;
        };

        core::mpsc::queue<common::connect_req*, 2048> conn_request_q;
        core::mpmc::queue<common::session_id_t, 2048> session_q;
        detail::poller_epoll poller;
        size_t _recv_buffer_size = 4096;
        std::atomic<bool> _shutdown{false};
        std::list<pending_connect_info> pending_connects;

    private:
        auto
        schedule(common::connect_req* req) {
            if (auto opt = session_q.try_dequeue(); opt) {
                req->cb(opt.value());
                delete req;
            } else {
                initiate_connect(req);
            }
        }

        void
        initiate_connect(common::connect_req* req) {
            int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (fd < 0) {
                req->cb(common::session_id_t{});
                delete req;
                return;
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(req->port);
            inet_pton(AF_INET, req->address.c_str(), &addr.sin_addr);

            int ret = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            if (ret == 0) {
                create_internal_session(fd, req);
                return;
            }

            if (errno == EINPROGRESS) {
                auto* ev = new epoll_event;
                ev->events = EPOLLOUT;
                ev->data.fd = fd;
                poller.add_event(fd, ev);
                pending_connects.push_back({fd, req});
            } else {
                req->cb(common::session_id_t{});
                ::close(fd);
                delete req;
            }
        }

        void
        handle_connect_completion(epoll_event* e) {
            int fd = e->data.fd;
            auto it = pending_connects.begin();
            for (; it != pending_connects.end(); ++it) {
                if (it->fd == fd)
                    break;
            }
            if (it == pending_connects.end())
                return;

            auto* req = it->req;
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);

            epoll_event ev{};
            poller.del_event(fd, &ev);

            if (so_error == 0) {
                create_internal_session(fd, req);
            } else {
                req->cb(common::session_id_t{});
                ::close(fd);
                delete req;
            }

            pending_connects.erase(it);
        }

        void
        create_internal_session(int fd, common::connect_req* req) {
            const int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            auto* s = new session_t(fd, new common::detail::recv_cache(_recv_buffer_size), new epoll_send_cache());
            auto sid = common::session_id_t::encode(s);
            req->cb(sid);
            delete req;
        }

        void
        drain_on_shutdown() {
            while (auto opt = conn_request_q.try_dequeue()) {
                opt.value()->cb(common::session_id_t{});
                delete opt.value();
            }
            for (auto& pc : pending_connects) {
                pc.req->cb(common::session_id_t{});
                ::close(pc.fd);
                delete pc.req;
            }
            pending_connects.clear();
        }

    public:
        connect_scheduler()
            : poller() {
        }

        int
        init(const common::scheduler_config& cfg) {
            _recv_buffer_size = cfg.recv_buffer_size;
            return 0;
        }

        int
        init() {
            return 0;
        }

        ~connect_scheduler() {
            for (auto& pc : pending_connects) {
                ::close(pc.fd);
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
            const int cnt = poller.wait_event(0);
            for (int i = 0; i < cnt; ++i) {
                handle_connect_completion(&poller.events[i]);
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

#endif //ENV_SCHEDULER_NET_EPOLL_CONNECT_SCHEDULER_HH
