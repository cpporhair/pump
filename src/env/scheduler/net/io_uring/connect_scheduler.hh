#ifndef ENV_SCHEDULER_NET_IOURING_CONNECT_SCHEDULER_HH
#define ENV_SCHEDULER_NET_IOURING_CONNECT_SCHEDULER_HH

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
#include "./scheduler.hh"
#include "../senders/connect.hh"

namespace pump::scheduler::net::io_uring {

    template <template<typename> class conn_op_t>
    struct
    connect_scheduler {
        friend struct conn_op_t<connect_scheduler>;
        friend struct senders::connect::op<connect_scheduler>;
    private:
        struct pending_connect_info {
            int fd;
            sockaddr_in addr;
            common::connect_req* req;
        };

        core::mpsc::queue<common::connect_req*, 2048> conn_request_q;
        core::mpmc::queue<common::session_id_t, 2048> session_q;
        struct ::io_uring ring{};
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

            pending_connects.push_back({fd, {}, req});
            auto& pc = pending_connects.back();
            pc.addr.sin_family = AF_INET;
            pc.addr.sin_port = htons(req->port);
            inet_pton(AF_INET, req->address, &pc.addr.sin_addr);

            ::io_uring_sqe* sqe = ::io_uring_get_sqe(&ring);
            if (!sqe) {
                pending_connects.pop_back();
                req->cb(common::session_id_t{});
                ::close(fd);
                delete req;
                return;
            }

            ::io_uring_prep_connect(sqe, fd,
                reinterpret_cast<sockaddr*>(&pc.addr), sizeof(pc.addr));
            ::io_uring_sqe_set_data(sqe, &pc);
            ::io_uring_submit(&ring);
        }

        void
        handle_io_uring() {
            ::io_uring_cqe* cqe;
            while (::io_uring_peek_cqe(&ring, &cqe) == 0) {
                auto* pc = reinterpret_cast<pending_connect_info*>(cqe->user_data);
                int res = cqe->res;
                ::io_uring_cqe_seen(&ring, cqe);

                if (res == 0) {
                    create_internal_session(pc->fd, pc->req);
                } else {
                    pc->req->cb(common::session_id_t{});
                    ::close(pc->fd);
                    delete pc->req;
                }

                pending_connects.remove_if([pc](const pending_connect_info& info) {
                    return &info == pc;
                });
            }
        }

        void
        create_internal_session(int fd, common::connect_req* req) {
            const int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            auto* s = new session_t(fd, new common::detail::recv_cache(_recv_buffer_size));
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
        connect_scheduler() = default;

        int
        init(const common::scheduler_config& cfg) {
            _recv_buffer_size = cfg.recv_buffer_size;
            return init(cfg.queue_depth);
        }

        int
        init(unsigned queue_depth = 256) {
            if (::io_uring_queue_init(queue_depth, &ring, 0) < 0)
                return -1;
            return 0;
        }

        ~connect_scheduler() {
            for (auto& pc : pending_connects) {
                ::close(pc.fd);
            }
            ::io_uring_queue_exit(&ring);
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
}

#endif //ENV_SCHEDULER_NET_IOURING_CONNECT_SCHEDULER_HH
