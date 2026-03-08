
#ifndef ENV_SCHEDULER_TCP_COMMON_LAYERS_HH
#define ENV_SCHEDULER_TCP_COMMON_LAYERS_HH

#include <unistd.h>

#include "env/scheduler/net/common/session_tags.hh"
#include "env/scheduler/net/common/frame.hh"
#include "env/scheduler/net/common/frame_receiver.hh"
#include "./struct.hh"
#include "./detail.hh"

namespace pump::scheduler::tcp::common {

    // TCP-specific tags
    struct on_recv{};       constexpr inline on_recv on_recv{};
    struct get_fd{};        constexpr inline get_fd get_fd{};
    struct get_read_iov{};  constexpr inline get_read_iov get_read_iov{};
    struct prepare_send{};  constexpr inline prepare_send prepare_send{};
    struct get_status{};    constexpr inline get_status get_status{};

    // tcp_bind<scheduler_t>: holds fd, scheduler pointer, and session status
    template<typename scheduler_t>
    struct
    tcp_bind {
        int fd;
        scheduler_t* scheduler;
        session_status status{session_status::normal};

        explicit tcp_bind(int _fd, scheduler_t* sche) : fd(_fd), scheduler(sche) {}

        template<typename owner_t>
        int
        invoke(const struct get_fd&, owner_t&) {
            return fd;
        }

        template<typename owner_t>
        session_status
        invoke(const struct get_status&, owner_t&) {
            return status;
        }

        template<typename owner_t>
        void
        invoke(const struct ::pump::scheduler::net::do_close&, owner_t&) {
            if (fd >= 0) {
                ::close(fd);
                fd = -1;
            }
            status = session_status::closed;
        }

        template<typename owner_t>
        void
        invoke(const struct ::pump::scheduler::net::on_error&, owner_t&, std::exception_ptr) {
            status = session_status::errors;
        }

        template<typename owner_t>
        void
        invoke(const struct ::pump::scheduler::net::do_send&, owner_t& owner, ::pump::scheduler::net::frame_send_req* req) {
            auto* send = new send_req{
                fd,
                net_frame(static_cast<char*>(req->data), req->len),
                std::move(req->cb)
            };
            owner.invoke(prepare_send, send);
            scheduler->schedule_send(send);
            delete req;
        }
    };

    // tcp_ring_buffer<unpacker_t>: packet buffer + unpack loop + prepare_send
    template<typename unpacker_t = detail::length_prefix_unpacker>
    struct
    tcp_ring_buffer {
        packet_buffer buf;

        explicit tcp_ring_buffer(size_t buf_size = 4096)
            : buf(buf_size) {}

        template<typename owner_t>
        auto
        invoke(const struct get_read_iov&, owner_t&) -> std::pair<iovec*, int> {
            int cnt = buf.make_iovec();
            return {buf.iov(), cnt};
        }

        template<typename owner_t>
        void
        invoke(const struct on_recv&, owner_t& owner, int bytes) {
            buf.forward_tail(bytes);
            for (auto frame = unpacker_t::unpack(&buf);
                 !unpacker_t::empty(frame);
                 frame = unpacker_t::unpack(&buf)) {
                owner.invoke(::pump::scheduler::net::on_frame, std::move(frame));
            }
        }

        template<typename owner_t>
        void
        invoke(const struct prepare_send&, owner_t&, send_req* req) {
            unpacker_t::prepare_send(req);
        }
    };

}

#endif //ENV_SCHEDULER_TCP_COMMON_LAYERS_HH
