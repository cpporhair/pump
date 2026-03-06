
#ifndef ENV_SCHEDULER_UDP_EPOLL_SCHEDULER_HH
#define ENV_SCHEDULER_UDP_EPOLL_SCHEDULER_HH

#include <cstdint>
#include <cstring>
#include <list>

#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"

#include "../common/struct.hh"

#include "env/scheduler/dgram/epoll.hh"

namespace pump::scheduler::udp::epoll {

    template <
        template<typename> class recv_op_t,
        template<typename> class send_op_t
    >
    struct
    scheduler {
        friend struct recv_op_t<scheduler>;
        friend struct send_op_t<scheduler>;
    private:
        dgram::epoll::transport _transport;

        core::per_core::queue<common::recv_req*, 2048> recv_q;
        core::per_core::queue<common::send_req*, 2048> send_q;

        core::local::queue<common::recv_req*> pending_recv;

        // Completed datagrams waiting for recv_req
        struct ready_entry {
            common::datagram dg;
            common::endpoint ep;
        };
        core::local::queue<ready_entry*> ready_q;

        // Sends that got EAGAIN, retry on EPOLLOUT
        std::list<common::send_req*> pending_sends;

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

        bool
        try_send(common::send_req* req) {
            req->prepare();
            auto res = _transport.try_sendmsg(&req->_msg);
            if (res >= 0) {
                req->cb(true);
                delete req;
                return true;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return false;
            }
            req->cb(false);
            delete req;
            return true;
        }

        void
        drain_send_q() {
            send_q.drain([this](common::send_req* req) {
                if (!try_send(req)) {
                    pending_sends.push_back(req);
                }
            });
        }

        void
        flush_pending_sends() {
            auto it = pending_sends.begin();
            while (it != pending_sends.end()) {
                if (try_send(*it)) {
                    it = pending_sends.erase(it);
                } else {
                    break;
                }
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
            for (auto* req : pending_sends) {
                req->cb(false);
                delete req;
            }
            pending_sends.clear();
            while (auto opt = ready_q.try_dequeue())
                delete opt.value();
        }

    public:
        scheduler() = default;

        int
        init(const common::scheduler_config& cfg) {
            return init(cfg.address, cfg.port, cfg.max_datagram_size);
        }

        int
        init(const char* address, uint16_t port,
             uint32_t max_datagram_size = 65536) {
            dgram::transport_config cfg{};
            cfg.max_dgram_size = max_datagram_size;
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
            bool writable = _transport.advance(
                [this](const char* buf, uint32_t len, const sockaddr_in& src) {
                    on_recv(buf, len, src);
                }
            );
            if (writable)
                flush_pending_sends();
            return true;
        }

        template<typename runtime_t>
        auto
        advance(const runtime_t&) {
            return advance();
        }
    };
}

#endif //ENV_SCHEDULER_UDP_EPOLL_SCHEDULER_HH
