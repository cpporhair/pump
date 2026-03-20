#ifndef ENV_SCHEDULER_TCP_EPOLL_CONNECT_SCHEDULER_HH
#define ENV_SCHEDULER_TCP_EPOLL_CONNECT_SCHEDULER_HH

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
#include "../common/error.hh"
#include "./epoll.hh"
#include "../senders/connect.hh"

namespace pump::scheduler::tcp::epoll {

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

        core::per_core::queue<common::connect_req*> conn_request_q{2048};
        core::mpmc::queue<int> conn_fd_q{2048};
        detail::poller_epoll poller;
        std::atomic<bool> _shutdown{false};
        std::list<pending_connect_info> pending_connects;

    private:
        auto
        schedule(common::connect_req* req) {
            if (auto opt = conn_fd_q.try_dequeue(); opt) {
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
                req->cb(-1);
                delete req;
                return;
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(req->port);
            inet_pton(AF_INET, req->address.c_str(), &addr.sin_addr);

            int ret = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            if (ret == 0) {
                const int flags = fcntl(fd, F_GETFL, 0);
                fcntl(fd, F_SETFL, flags | O_NONBLOCK);
                req->cb(fd);
                delete req;
                return;
            }

            if (errno == EINPROGRESS) {
                auto* ev = new epoll_event;
                ev->events = EPOLLOUT;
                ev->data.fd = fd;
                poller.add_event(fd, ev);
                pending_connects.push_back({fd, req});
            } else {
                req->cb(-1);
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
                const int flags = fcntl(fd, F_GETFL, 0);
                fcntl(fd, F_SETFL, flags | O_NONBLOCK);
                req->cb(fd);
            } else {
                req->cb(-1);
                ::close(fd);
            }
            delete req;

            pending_connects.erase(it);
        }

        void
        drain_on_shutdown() {
            while (auto opt = conn_request_q.try_dequeue()) {
                opt.value()->cb(-1);
                delete opt.value();
            }
            for (auto& pc : pending_connects) {
                pc.req->cb(-1);
                ::close(pc.fd);
                delete pc.req;
            }
            pending_connects.clear();
        }

    public:
        connect_scheduler()
            : poller() {
        }

        explicit connect_scheduler(size_t queue_depth)
            : poller(), conn_request_q(queue_depth), conn_fd_q(queue_depth) {
        }

        int
        init(const common::scheduler_config&) {
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

#endif //ENV_SCHEDULER_TCP_EPOLL_CONNECT_SCHEDULER_HH
