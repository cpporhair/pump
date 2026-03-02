
#ifndef ENV_SCHEDULER_NET_COMMON_DETAIL_HH
#define ENV_SCHEDULER_NET_COMMON_DETAIL_HH
#include <atomic>
#include <cstring>
#include <unistd.h>

#include "pump/core/meta.hh"
#include "pump/core/lock_free_queue.hh"
#include "./struct.hh"

namespace pump::scheduler::net::common::detail {
    struct
    _read_pkt_len {
        uint32_t
        operator()() const noexcept {
            return 0xffffffff;
        }
        uint32_t
        operator()(const char* data ,const size_t len) const noexcept {
            return *reinterpret_cast<const uint32_t*>(data);
        }
        uint32_t
        operator()(const char* d1 ,const size_t l1,const char* d2,const size_t l2) const noexcept {
            uint08_t need[4] = {0, 0, 0, 0};
            memcpy(&need[0],d1,l1) ;
            memcpy(&need[l1],d2,l2);
            return *reinterpret_cast<const uint32_t*>(need);
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

    inline constexpr _read_pkt_len read_pkt_len{};
    inline constexpr _full_pkt_checker full_pkt_checker{};

    [[nodiscard]]
    inline bool
    has_full_pkt(const common::packet_buffer* buf) {
        const auto len = buf->handle_data(sizeof(uint32_t), read_pkt_len);
        if (len == 0xffffffff)
            return false;
        return buf->handle_data(len, full_pkt_checker);
    }

    // D7: parameter types match handle_data's size_t to avoid narrowing
    struct
    _frame_copier {
        net_frame
        operator()() const noexcept {
            return {};
        }

        net_frame
        operator()(const char* data, const size_t len) const noexcept {
            auto* copy = new char[len];
            memcpy(copy, data, len);
            return {copy, static_cast<uint32_t>(len)};
        }

        net_frame
        operator()(const char* d1, const size_t l1, const char* d2, const size_t l2) const noexcept {
            size_t total = l1 + l2;
            auto* copy = new char[total];
            memcpy(copy, d1, l1);
            memcpy(copy + l1, d2, l2);
            return {copy, static_cast<uint32_t>(total)};
        }
    };

    inline constexpr _frame_copier frame_copier{};

    inline net_frame
    copy_out_frame(common::packet_buffer* buf) {
        const auto len = buf->handle_data(sizeof(uint32_t), read_pkt_len);
        if (len == 0xffffffff)
            return {};
        if (buf->used() < len)
            return {};
        // skip the uint32_t length prefix — net layer handles framing,
        // net_frame contains only the application payload
        buf->forward_head(sizeof(uint32_t));
        auto payload_len = static_cast<size_t>(len) - sizeof(uint32_t);
        auto frame = buf->handle_data(payload_len, frame_copier);
        if (frame.size() > 0)
            buf->forward_head(payload_len);
        return frame;
    }

    struct
    recv_cache {
        packet_buffer buf;
        core::spsc::queue<common::recv_req*> recv_q;
        core::spsc::queue<common::net_frame*> ready_q;

        explicit
        recv_cache(size_t size)
            : buf(size) {
        }

        // 1.9: implement resource release
        auto
        release() {
            while (auto opt = recv_q.try_dequeue()) {
                delete opt.value();
            }
            while (auto opt = ready_q.try_dequeue()) {
                delete opt.value();  // ~net_frame() handles _data
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
            if (fd >= 0) {
                ::close(fd);
                fd = -1;
            }
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
            if (fd >= 0) {
                ::close(fd);
                fd = -1;
            }
            release();
            status.store(session_status::closed);
        }
    };
}

#endif //ENV_SCHEDULER_NET_COMMON_DETAIL_HH
