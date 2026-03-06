
#ifndef ENV_SCHEDULER_UDP_IOURING_SCHEDULER_HH
#define ENV_SCHEDULER_UDP_IOURING_SCHEDULER_HH

#include <cstdint>
#include <cstring>

#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"

#include "../common/struct.hh"

#include "env/scheduler/dgram/io_uring.hh"

namespace pump::scheduler::udp::io_uring {

    template <
        template<typename> class recv_op_t,
        template<typename> class send_op_t
    >
    struct
    scheduler {
        friend struct recv_op_t<scheduler>;
        friend struct send_op_t<scheduler>;
    private:
        dgram::io_uring::transport _transport;

        core::per_core::queue<common::recv_req*, 2048> recv_q;
        core::per_core::queue<common::send_req*, 2048> send_q;

        core::local::queue<common::recv_req*> pending_recv;

        // Completed datagrams waiting for recv_req
        struct ready_entry {
            common::datagram dg;
            common::endpoint ep;
        };
        core::local::queue<ready_entry*> ready_q;

        std::atomic<bool> _shutdown{false};

    private:
        void
        schedule(common::recv_req* req) {
            if (!recv_q.try_enqueue(req)) {
                req->cb(std::make_exception_ptr(std::runtime_error("udp recv queue full")));
                delete req;
            }
        }

        void
        schedule(common::send_req* req) {
            if (!send_q.try_enqueue(req)) {
                req->cb(false);
                delete req;
            }
        }

        void
        drain_recv_q() {
            recv_q.drain([this](common::recv_req* req) {
                if (auto opt = ready_q.try_dequeue()) {
                    auto* entry = opt.value();
                    req->cb(std::make_pair(
                        std::move(entry->dg),
                        std::move(entry->ep)
                    ));
                    delete req;
                    delete entry;
                } else {
                    pending_recv.try_enqueue(req);
                }
            });
        }

        void
        drain_send_q() {
            bool submitted = false;
            send_q.drain([this, &submitted](common::send_req* req) {
                req->prepare();
                if (!_transport.enqueue_sendmsg(&req->_msg, req)) [[unlikely]] {
                    req->cb(false);
                    delete req;
                    return;
                }
                submitted = true;
            });
            if (submitted) {
                _transport.flush();
            }
        }

        void
        on_recv(const char* buf, uint32_t len, const sockaddr_in& src) {
            if (auto opt = pending_recv.try_dequeue()) {
                auto* copy = new char[len];
                memcpy(copy, buf, len);
                opt.value()->cb(std::make_pair(
                    common::datagram{copy, len},
                    common::endpoint{src}
                ));
                delete opt.value();
            } else {
                auto* copy = new char[len];
                memcpy(copy, buf, len);
                ready_q.try_enqueue(new ready_entry{
                    common::datagram{copy, len},
                    common::endpoint{src}
                });
            }
        }

        void
        on_send_complete(void* user_data, int res) {
            auto* req = static_cast<common::send_req*>(user_data);
            req->cb(res >= 0);
            delete req;
        }

        void
        drain_on_shutdown() {
            auto err = std::make_exception_ptr(std::runtime_error("udp scheduler shutdown"));
            while (auto opt = pending_recv.try_dequeue()) {
                opt.value()->cb(err);
                delete opt.value();
            }
            recv_q.drain([&err](common::recv_req* req) {
                req->cb(err);
                delete req;
            });
            send_q.drain([](common::send_req* req) {
                req->cb(false);
                delete req;
            });
            while (auto opt = ready_q.try_dequeue())
                delete opt.value();
        }

    public:
        scheduler() = default;

        int
        init(const common::scheduler_config& cfg) {
            return init(cfg.address, cfg.port, cfg.queue_depth,
                        cfg.max_datagram_size, cfg.recv_depth);
        }

        int
        init(const char* address, uint16_t port,
             unsigned queue_depth = 256,
             uint32_t max_datagram_size = 65536,
             uint32_t recv_depth = 32) {
            dgram::transport_config cfg{};
            cfg.queue_depth = queue_depth;
            cfg.max_dgram_size = max_datagram_size;
            cfg.recv_depth = recv_depth;
            return _transport.init(address, port, cfg);
        }

        ~scheduler() {
            while (auto opt = ready_q.try_dequeue())
                delete opt.value();
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
            drain_recv_q();
            drain_send_q();
            _transport.advance(
                [this](const char* buf, uint32_t len, const sockaddr_in& src) {
                    on_recv(buf, len, src);
                },
                [this](void* user_data, int res) {
                    on_send_complete(user_data, res);
                }
            );
            return true;
        }

        template<typename runtime_t>
        auto
        advance(const runtime_t&) {
            return advance();
        }
    };
}

#endif //ENV_SCHEDULER_UDP_IOURING_SCHEDULER_HH
