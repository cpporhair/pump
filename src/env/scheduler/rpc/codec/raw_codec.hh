#ifndef ENV_SCHEDULER_RPC_CODEC_RAW_CODEC_HH
#define ENV_SCHEDULER_RPC_CODEC_RAW_CODEC_HH

#include <cstring>
#include <type_traits>

#include "../common/codec_concept.hh"

namespace pump::rpc {

    // Default Codec for trivially-copyable types
    struct raw_codec {
        template<typename T>
        T decode_payload(const payload_view& pv) {
            static_assert(std::is_trivially_copyable_v<T>);
            T result;
            uint8_t tmp[sizeof(T)];
            const uint8_t* data = linearize_payload(pv, tmp, sizeof(T));
            std::memcpy(&result, data, sizeof(T));
            return result;
        }

        template<typename T>
        encode_result encode_payload(const T& obj) {
            static_assert(std::is_trivially_copyable_v<T>);
            static_assert(sizeof(T) <= 64, "T too large for inline_buf");
            encode_result r{};
            std::memcpy(r.inline_buf, &obj, sizeof(T));
            r.vec[0] = {r.inline_buf, sizeof(T)};
            r.cnt = 1;
            r.total_len = sizeof(T);
            return r;
        }
    };

}

#endif //ENV_SCHEDULER_RPC_CODEC_RAW_CODEC_HH
