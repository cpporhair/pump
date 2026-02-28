#ifndef ENV_SCHEDULER_RPC_COMMON_CODEC_CONCEPT_HH
#define ENV_SCHEDULER_RPC_COMMON_CODEC_CONCEPT_HH

#include <cstdint>
#include <cstring>
#include <cassert>
#include <bits/types/struct_iovec.h>

namespace pump::rpc {

    // payload view: points into Net layer ring buffer data
    // may span ring buffer boundary, hence iovec representation
    struct payload_view {
        const iovec* vec;
        uint8_t      cnt;   // 1 or 2
        size_t       len;
    };

    // encode result: holds serialized payload data lifetime
    struct encode_result {
        iovec    vec[4];
        size_t   cnt       = 0;
        size_t   total_len = 0;

        uint8_t  inline_buf[64]{};
        uint8_t* heap_buf = nullptr;

        encode_result() = default;
        ~encode_result() { delete[] heap_buf; }

        encode_result(encode_result&& o) noexcept
            : cnt(o.cnt)
            , total_len(o.total_len)
            , heap_buf(o.heap_buf) {
            std::memcpy(inline_buf, o.inline_buf, sizeof(inline_buf));
            std::memcpy(vec, o.vec, sizeof(vec));
            o.heap_buf = nullptr;
            // fix up iovec pointers that reference inline_buf
            for (size_t i = 0; i < cnt; ++i) {
                auto* base = static_cast<uint8_t*>(vec[i].iov_base);
                if (base >= o.inline_buf && base < o.inline_buf + sizeof(o.inline_buf)) {
                    vec[i].iov_base = inline_buf + (base - o.inline_buf);
                }
            }
        }

        encode_result& operator=(encode_result&& o) noexcept {
            if (this != &o) {
                delete[] heap_buf;
                cnt = o.cnt;
                total_len = o.total_len;
                heap_buf = o.heap_buf;
                std::memcpy(inline_buf, o.inline_buf, sizeof(inline_buf));
                std::memcpy(vec, o.vec, sizeof(vec));
                o.heap_buf = nullptr;
                for (size_t i = 0; i < cnt; ++i) {
                    auto* base = static_cast<uint8_t*>(vec[i].iov_base);
                    if (base >= o.inline_buf && base < o.inline_buf + sizeof(o.inline_buf)) {
                        vec[i].iov_base = inline_buf + (base - o.inline_buf);
                    }
                }
            }
            return *this;
        }

        encode_result(const encode_result&) = delete;
        encode_result& operator=(const encode_result&) = delete;
    };

    // linearize payload: if contiguous return pointer directly (zero-copy),
    // if cross-boundary copy to tmp_buf
    inline const uint8_t*
    linearize_payload(const payload_view& pv, uint8_t* tmp_buf, size_t tmp_buf_size) {
        if (pv.cnt == 1) {
            return static_cast<const uint8_t*>(pv.vec[0].iov_base);
        }
        assert(pv.len <= tmp_buf_size);
        size_t offset = 0;
        for (uint8_t i = 0; i < pv.cnt; ++i) {
            std::memcpy(tmp_buf + offset, pv.vec[i].iov_base, pv.vec[i].iov_len);
            offset += pv.vec[i].iov_len;
        }
        return tmp_buf;
    }

}

#endif //ENV_SCHEDULER_RPC_COMMON_CODEC_CONCEPT_HH
