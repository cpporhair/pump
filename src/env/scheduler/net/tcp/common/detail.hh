
#ifndef ENV_SCHEDULER_TCP_COMMON_DETAIL_HH
#define ENV_SCHEDULER_TCP_COMMON_DETAIL_HH
#include <cstring>
#include <unistd.h>

#include "pump/core/meta.hh"
#include "pump/core/lock_free_queue.hh"
#include "./struct.hh"
#include "./error.hh"

namespace pump::scheduler::tcp::common::detail {
    // --- Unpacker helpers (internal) ---
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
        buf->forward_head(sizeof(uint32_t));
        auto payload_len = static_cast<size_t>(len) - sizeof(uint32_t);
        auto frame = buf->handle_data(payload_len, frame_copier);
        if (frame.size() > 0)
            buf->forward_head(payload_len);
        return frame;
    }

    // --- Pluggable unpacker strategies ---

    // Default: 4-byte length-prefix framing
    struct length_prefix_unpacker {
        using frame_type = net_frame;

        static frame_type
        unpack(common::packet_buffer* buf) {
            return copy_out_frame(buf);
        }

        static bool
        empty(const frame_type& f) { return f.size() == 0; }

        static void
        prepare_send(common::send_req* req) {
            auto len = static_cast<uint32_t>(req->frame._len + sizeof(uint32_t));
            memcpy(req->_hdr, &len, sizeof(uint32_t));
            req->_send_vec[0] = {req->_hdr, sizeof(uint32_t)};
            req->_send_vec[1] = {req->frame._data, req->frame._len};
            req->_send_cnt = 2;
        }
    };

    // Raw stream: no framing, delivers all available bytes as-is.
    struct stream_unpacker {
        using frame_type = net_frame;

        static frame_type
        unpack(common::packet_buffer* buf) {
            auto used = buf->used();
            if (used == 0)
                return {};
            auto frame = buf->handle_data(used, frame_copier);
            if (frame.size() > 0)
                buf->forward_head(used);
            return frame;
        }

        static bool
        empty(const frame_type& f) { return f.size() == 0; }

        static void
        prepare_send(common::send_req* req) {
            req->_send_vec[0] = {req->frame._data, req->frame._len};
            req->_send_cnt = 1;
        }
    };

    // Backward-compatible aliases
    using length_prefix_framing = length_prefix_unpacker;
    using stream_framing = stream_unpacker;
}

#endif //ENV_SCHEDULER_TCP_COMMON_DETAIL_HH
