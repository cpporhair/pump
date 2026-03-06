
#ifndef ENV_SCHEDULER_UDP_IOURING_SCHEDULER_HH
#define ENV_SCHEDULER_UDP_IOURING_SCHEDULER_HH

#include <cstdint>
#include <vector>
#include <cstring>
#include <liburing.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"

#include "../common/struct.hh"

namespace pump::scheduler::udp::io_uring {

    enum struct
    uring_event_type {
        recvmsg = 0,
        sendmsg = 1
    };

    struct
    io_uring_request {
        uring_event_type event_type;
        void* user_data;
    };

    struct
    recv_slot {
        char* buf;
        uint32_t buf_size;
        sockaddr_in src_addr;
        msghdr msg;
        iovec iov;

        void
        prepare(uint32_t max_size) {
            iov = {buf, max_size};
            msg = {};
            msg.msg_name = &src_addr;
            msg.msg_namelen = sizeof(src_addr);
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
        }
    };

    template <
        template<typename> class recv_op_t,
        template<typename> class send_op_t
    >
    struct
    scheduler {
        friend struct recv_op_t<scheduler>;
        friend struct send_op_t<scheduler>;
    private:
        int _fd = -1;
        struct ::io_uring _ring{};
        uint32_t _max_datagram_size = 65536;

        core::per_core::queue<common::recv_req*, 2048> recv_q;
        core::per_core::queue<common::send_req*, 2048> send_q;

        core::local::queue<common::recv_req*> pending_recv;
        core::local::queue<recv_slot*> pending_slots;
        std::vector<recv_slot> recv_slots;

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
        submit_recvmsg(recv_slot* slot) {
            slot->prepare(_max_datagram_size);
            auto* sqe = ::io_uring_get_sqe(&_ring);
            if (!sqe) [[unlikely]] return;
            auto* uring_req = new io_uring_request{uring_event_type::recvmsg, slot};
            ::io_uring_prep_recvmsg(sqe, _fd, &slot->msg, 0);
            ::io_uring_sqe_set_data(sqe, uring_req);
            ::io_uring_submit(&_ring);
        }

        void
        deliver_to_req(common::recv_req* req, recv_slot* slot, int len) {
            auto* copy = new char[len];
            memcpy(copy, slot->buf, len);
            req->cb(std::make_pair(
                common::datagram{copy, static_cast<uint32_t>(len)},
                common::endpoint{slot->src_addr}
            ));
            delete req;
        }

        void
        drain_recv_q() {
            recv_q.drain([this](common::recv_req* req) {
                if (auto opt = pending_slots.try_dequeue()) {
                    auto* slot = opt.value();
                    auto len = slot->msg.msg_namelen;
                    // msg_namelen was repurposed; use iov_len for actual data length
                    // Actually, recvmsg returns the data length via CQE res, which we
                    // stored in a side channel. For pending_slots, we store the length
                    // in buf_size temporarily after CQE processing.
                    deliver_to_req(req, slot, slot->buf_size);
                    submit_recvmsg(slot);
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
                auto* sqe = ::io_uring_get_sqe(&_ring);
                if (!sqe) [[unlikely]] {
                    req->cb(false);
                    delete req;
                    return;
                }
                auto* uring_req = new io_uring_request{uring_event_type::sendmsg, req};
                ::io_uring_prep_sendmsg(sqe, _fd, &req->_msg, 0);
                ::io_uring_sqe_set_data(sqe, uring_req);
                submitted = true;
            });
            if (submitted) {
                ::io_uring_submit(&_ring);
            }
        }

        void
        on_recvmsg_complete(io_uring_request* uring_req, int res) {
            auto* slot = static_cast<recv_slot*>(uring_req->user_data);
            delete uring_req;

            if (res <= 0) [[unlikely]] {
                // Error or empty datagram — just re-submit
                submit_recvmsg(slot);
                return;
            }

            if (auto opt = pending_recv.try_dequeue()) {
                deliver_to_req(opt.value(), slot, res);
                submit_recvmsg(slot);
            } else {
                // No pending recv_req — stash the slot with data length in buf_size
                slot->buf_size = static_cast<uint32_t>(res);
                pending_slots.try_enqueue(slot);
                // Don't re-submit recvmsg — slot is occupied
            }
        }

        void
        on_sendmsg_complete(io_uring_request* uring_req, int res) {
            auto* req = static_cast<common::send_req*>(uring_req->user_data);
            delete uring_req;
            req->cb(res >= 0);
            delete req;
        }

        void
        handle_io() {
            while (true) {
                ::io_uring_cqe* cqe = nullptr;
                if (::io_uring_peek_cqe(&_ring, &cqe) != 0)
                    return;

                auto* uring_req = reinterpret_cast<io_uring_request*>(cqe->user_data);
                int res = cqe->res;
                ::io_uring_cqe_seen(&_ring, cqe);

                switch (uring_req->event_type) {
                    case uring_event_type::recvmsg:
                        on_recvmsg_complete(uring_req, res);
                        break;
                    case uring_event_type::sendmsg:
                        on_sendmsg_complete(uring_req, res);
                        break;
                }
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
            _fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (_fd < 0)
                return -1;

            int opt = 1;
            setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            inet_pton(AF_INET, address, &addr.sin_addr);

            if (bind(_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
                ::close(_fd);
                _fd = -1;
                return -1;
            }

            if (::io_uring_queue_init(queue_depth, &_ring, 0) < 0) {
                ::close(_fd);
                _fd = -1;
                return -1;
            }

            _max_datagram_size = max_datagram_size;
            recv_slots.resize(recv_depth);
            for (auto& slot : recv_slots) {
                slot.buf = new char[max_datagram_size];
                slot.buf_size = max_datagram_size;
                submit_recvmsg(&slot);
            }

            return 0;
        }

        ~scheduler() {
            for (auto& slot : recv_slots) {
                delete[] slot.buf;
            }
            if (_fd >= 0) {
                ::close(_fd);
                ::io_uring_queue_exit(&_ring);
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
            drain_recv_q();
            drain_send_q();
            handle_io();
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
