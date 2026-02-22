
#ifndef ENV_SCHEDULER_NET_COMMON_DETAIL_HH
#define ENV_SCHEDULER_NET_COMMON_DETAIL_HH
#include <atomic>
#include <string.h>
#include <unistd.h>

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
        // 1.8: fix cross-buffer read - use &need[l1] instead of &need[1]
        uint16_t
        operator()(const char* d1 ,const size_t l1,const char* d2,const size_t l2) const noexcept {
            uint08_t need[2] = {0, 0};
            memcpy(&need[0],d1,l1) ;
            memcpy(&need[l1],d2,l2);
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

    // 1.7: fix condition - 0xffff means data insufficient, should return false
    [[nodiscard]]
    inline bool
    has_full_pkt(const common::packet_buffer* buf) {
        const auto len = buf->handle_data(sizeof(uint16_t), read_pkt_len);
        if (len == 0xffff)
            return false;
        return buf->handle_data(len, full_pkt_checker);
    }

    // 1.7: fix condition - 0xffff means data insufficient
    inline auto
    get_recv_pkt(const common::packet_buffer* buf) {
        const auto len = buf->handle_data(sizeof(uint16_t), read_pkt_len);
        if (len == 0xffff)
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
            : buf(size)
            , req(nullptr) {
        }

        // 1.9: implement resource release
        auto
        release() {
            if (auto* r = req.exchange(nullptr); r != nullptr) {
                delete r;
            }
        }
    };

    enum class
    session_status {
        normal,
        closed,
        errors
    };

    // 6.3: global generation counter for session tagged handles
    inline std::atomic<uint16_t> _session_generation_counter{0};

    // 6.1: unified session template with explicit recv_cache and optional send_cache
    // Primary template: session with both recv and send cache
    template <typename recv_cache_t, typename send_cache_t = void>
    struct
    internal_session {
        int fd;
        uint16_t generation;
        std::atomic<session_status> status;
        recv_cache_t* _recv_cache;
        send_cache_t* _send_cache;

        explicit
        internal_session(const int _fd, recv_cache_t* rc, send_cache_t* sc)
            : fd(_fd)
            , generation(_session_generation_counter.fetch_add(1, std::memory_order_relaxed))
            , status(session_status::normal)
            , _recv_cache(rc)
            , _send_cache(sc) {
        }

        [[nodiscard]] auto* get_recv_cache() noexcept { return _recv_cache; }
        [[nodiscard]] auto* get_send_cache() noexcept { return _send_cache; }

        auto
        release() {
            _recv_cache->release();
            _send_cache->release();
        }

        void
        close() {
            ::close(fd);
            release();
            status.store(session_status::closed);
        }
    };

    // 6.1: specialization for recv-only session (io_uring - no per-session send cache)
    template <typename recv_cache_t>
    struct
    internal_session<recv_cache_t, void> {
        int fd;
        uint16_t generation;
        std::atomic<session_status> status;
        recv_cache_t* _recv_cache;

        explicit
        internal_session(const int _fd, recv_cache_t* rc)
            : fd(_fd)
            , generation(_session_generation_counter.fetch_add(1, std::memory_order_relaxed))
            , status(session_status::normal)
            , _recv_cache(rc) {
        }

        [[nodiscard]] auto* get_recv_cache() noexcept { return _recv_cache; }

        auto
        release() {
            _recv_cache->release();
        }

        void
        close() {
            ::close(fd);
            release();
            status.store(session_status::closed);
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
