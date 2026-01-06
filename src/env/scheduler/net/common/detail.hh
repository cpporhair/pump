
#ifndef ENV_SCHEDULER_NET_COMMON_DETAIL_HH
#define ENV_SCHEDULER_NET_COMMON_DETAIL_HH
#include <atomic>
#include <string.h>

#include "pump/core/meta.hh"
#include "pump/core/lock_free_queue.hh"
#include "./struct.hh"

namespace pump::scheduler::net::common::detail {
    struct
    _read_pkt_len {
        uint16_t
        operator()() const noexcept {
            return 0xffff;
        }
        uint16_t
        operator()(const char* data ,const size_t len) const noexcept {
            return *reinterpret_cast<const uint16_t*>(data);
        }
        uint16_t
        operator()(const char* d1 ,const size_t l1,const char* d2,const size_t l2) const noexcept {
            uint08_t need[2] = {0, 0};
            memcpy(&need[0],d1,l1) ;
            memcpy(&need[1],d2,l2);
            return *reinterpret_cast<const uint16_t*>(need);
        }
    };

    struct
    _full_pkt_checker {
        bool
        operator()() const noexcept {
            return false;
        }

        bool
        operator()(const char* data ,const size_t len) const noexcept {
            return true;
        }

        bool
        operator()(const char* d1 ,const size_t l1,const char* d2,const size_t l2) const noexcept {
            return true;
        }
    };

    struct
    _recv_pkt_getter {
        auto
        operator()() const noexcept {
            return pkt_iovec{0};
        }

        auto
        operator()(const char* data ,const size_t len) const noexcept {
            return pkt_iovec{1, new iovec{(void *)data, len}};
        }

        auto
        operator()(const char *d1, const size_t l1, const char *d2, const size_t l2) const noexcept {
            return pkt_iovec{2, new iovec[2]{{(void *)d1, l1}, {(void *)d2, l2}}};
        }
    };

    inline constexpr _read_pkt_len read_pkt_len{};
    inline constexpr _full_pkt_checker full_pkt_checker{};
    inline constexpr _recv_pkt_getter recv_pkt_getter{};

    [[nodiscard]]
    inline bool
    has_full_pkt(const common::packet_buffer* buf) {
        const auto len = buf->handle_data(sizeof(uint16_t), read_pkt_len);
        if (len != 0xffff)
            return false;
        return buf->handle_data(len, full_pkt_checker);
    }

    inline auto
    get_recv_pkt(const common::packet_buffer* buf) {
        const auto len = buf->handle_data(sizeof(uint16_t), read_pkt_len);
        if (len != 0xffff)
            return pkt_iovec{0};
        return buf->handle_data(len, recv_pkt_getter);
    }

    struct
    pkt_vec {
        const char* data = nullptr;
        const size_t len = 0;
    };

    struct
    recv_cache {
        packet_buffer buf;
        std::atomic<common::recv_req *> req;

        explicit
        recv_cache(size_t size)
            : buf(size) {
        }

        auto
        release() {
        }
    };

    enum class
    session_status {
        normal,
        closed,
        errors
    };

    template <typename ...impl_t>
    struct
    internal_session {
        int fd;
        std::atomic<session_status> status;
        std::tuple<impl_t*...> impls;

        explicit
        internal_session(const int _fd, impl_t* ...args)
            : impls(args...)
            , fd(_fd)
            , status(session_status::normal) {
        }

        auto
        release() {
            std::apply([](impl_t *... impl) { (impl->release(), ...); }, impls);
        }

        void
        close() {
            ::close(fd);
            release();
            status.store(session_status::closed);
        }
    };

    template <typename reader_t>
    struct
    session {
        int fd;
        std::atomic<session_status> status;
        std::unique_ptr<reader_t> rdr;
        void* user_data;

        explicit
        session(const int _fd, std::unique_ptr<reader_t>&& r)
            : fd(_fd)
            , status(session_status::normal)
            , rdr(__fwd__(r))
            , user_data(nullptr) {
        }

        void
        clear() {
            rdr.clear();
            rdr.release();
        }
    };

    enum class
    packet_recv_status {
        wait_len,
        wait_data
    };

    struct
    reader {
        packet_recv_status recv_status;
        uint16_t cur_read;
        common::packet* buf;
        core::spsc::queue<common::recv_req*> recv_request_q;
        core::spsc::queue<common::packet*> pkt_q;

        reader()
            : recv_status(packet_recv_status::wait_len)
            , cur_read(0)
            , buf(nullptr) {
        }

        void
        clear() {
            while (auto opt = recv_request_q.try_dequeue()) {
                delete opt.value();
            }

            while (auto opt = pkt_q.try_dequeue()) {
                opt.value()->clear();
                delete opt.value();
            }

            delete buf;
            buf = nullptr;
            cur_read = 0;
            recv_status = packet_recv_status::wait_len;
        }
    };
}

#endif //ENV_SCHEDULER_NET_COMMON_DETAIL_HH