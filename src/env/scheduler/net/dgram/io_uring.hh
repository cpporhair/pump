
#ifndef ENV_SCHEDULER_DGRAM_IO_URING_HH
#define ENV_SCHEDULER_DGRAM_IO_URING_HH

#include <cstdint>
#include <vector>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "./common.hh"

namespace pump::scheduler::dgram::io_uring {

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

    struct
    transport {
    private:
        enum struct event_type : uint8_t { recvmsg = 0, sendmsg = 1 };

        struct uring_req {
            event_type type;
            void* user_data;
        };

        int _fd = -1;
        struct ::io_uring _ring{};
        uint32_t _max_dgram_size = 65536;
        std::vector<recv_slot> _recv_slots;

        void
        submit_recvmsg(recv_slot* slot) {
            slot->prepare(_max_dgram_size);
            auto* sqe = ::io_uring_get_sqe(&_ring);
            if (!sqe) [[unlikely]] return;
            auto* req = new uring_req{event_type::recvmsg, slot};
            ::io_uring_prep_recvmsg(sqe, _fd, &slot->msg, 0);
            ::io_uring_sqe_set_data(sqe, req);
            ::io_uring_submit(&_ring);
        }

    public:
        transport() = default;

        transport(const transport&) = delete;
        transport& operator=(const transport&) = delete;

        int
        init(const char* address, uint16_t port, const transport_config& cfg = {}) {
            _fd = create_bound_udp_socket(address, port);
            if (_fd < 0)
                return -1;

            if (::io_uring_queue_init(cfg.queue_depth, &_ring, 0) < 0) {
                ::close(_fd);
                _fd = -1;
                return -1;
            }

            _max_dgram_size = cfg.max_dgram_size;
            _recv_slots.resize(cfg.recv_depth);
            for (auto& slot : _recv_slots) {
                slot.buf = new char[cfg.max_dgram_size];
                slot.buf_size = cfg.max_dgram_size;
                submit_recvmsg(&slot);
            }

            return 0;
        }

        ~transport() {
            for (auto& slot : _recv_slots)
                delete[] slot.buf;
            if (_fd >= 0) {
                ::close(_fd);
                ::io_uring_queue_exit(&_ring);
            }
        }

        [[nodiscard]] int fd() const { return _fd; }

        // Sync sendto — fire-and-forget, used by KCP's ikcp output callback
        void
        sendto(const char* data, uint32_t len, const sockaddr_in& dest) {
            ::sendto(_fd, data, len, MSG_DONTWAIT,
                     reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
        }

        // Enqueue async sendmsg via io_uring SQE.
        // user_data is passed back to send_handler in advance().
        // Returns false if no SQE available.
        bool
        enqueue_sendmsg(msghdr* msg, void* user_data) {
            auto* sqe = ::io_uring_get_sqe(&_ring);
            if (!sqe) [[unlikely]] return false;
            auto* req = new uring_req{event_type::sendmsg, user_data};
            ::io_uring_prep_sendmsg(sqe, _fd, msg, 0);
            ::io_uring_sqe_set_data(sqe, req);
            return true;
        }

        // Submit pending SQEs (call after one or more enqueue_sendmsg).
        void
        flush() {
            ::io_uring_submit(&_ring);
        }

        // Process all completed io_uring events.
        // recv_handler: void(const char* buf, uint32_t len, const sockaddr_in& src)
        //   buf is valid only during the callback; transport re-submits the slot after.
        // send_handler: void(void* user_data, int res)
        template<typename RecvHandler, typename SendHandler>
        void
        advance(RecvHandler&& recv_handler, SendHandler&& send_handler) {
            while (true) {
                ::io_uring_cqe* cqe = nullptr;
                if (::io_uring_peek_cqe(&_ring, &cqe) != 0)
                    return;

                auto* req = reinterpret_cast<uring_req*>(cqe->user_data);
                int res = cqe->res;
                ::io_uring_cqe_seen(&_ring, cqe);

                if (req->type == event_type::recvmsg) {
                    auto* slot = static_cast<recv_slot*>(req->user_data);
                    delete req;
                    if (res > 0) {
                        recv_handler(slot->buf, static_cast<uint32_t>(res), slot->src_addr);
                    }
                    submit_recvmsg(slot);
                } else {
                    auto* ud = req->user_data;
                    delete req;
                    send_handler(ud, res);
                }
            }
        }
    };

}  // namespace pump::scheduler::dgram::io_uring

#endif //ENV_SCHEDULER_DGRAM_IO_URING_HH
