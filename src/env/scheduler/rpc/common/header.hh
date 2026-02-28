#ifndef ENV_SCHEDULER_RPC_COMMON_HEADER_HH
#define ENV_SCHEDULER_RPC_COMMON_HEADER_HH

#include <cstdint>
#include <cstring>

#include "./message_type.hh"

namespace pump::rpc {

    struct rpc_header {
        message_type type;
        uint32_t     request_id;
        uint16_t     method_id;

        static constexpr size_t size = 7;
    };

    inline rpc_header
    parse_header(const uint8_t* data) {
        rpc_header h;
        h.type = static_cast<message_type>(data[0]);
        std::memcpy(&h.request_id, data + 1, 4);
        std::memcpy(&h.method_id,  data + 5, 2);
        return h;
    }

    inline void
    write_header(uint8_t* buf, const rpc_header& h) {
        buf[0] = static_cast<uint8_t>(h.type);
        std::memcpy(buf + 1, &h.request_id, 4);
        std::memcpy(buf + 5, &h.method_id,  2);
    }

}

#endif //ENV_SCHEDULER_RPC_COMMON_HEADER_HH
