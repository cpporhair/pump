
#ifndef ENV_SCHEDULER_KCP_COMMON_STRUCT_HH
#define ENV_SCHEDULER_KCP_COMMON_STRUCT_HH

#include <cstdint>
#include <functional>
#include <variant>
#include <netinet/in.h>

#include "pump/core/meta.hh"
#include "env/scheduler/net/common/frame.hh"

namespace pump::scheduler::kcp::common {

    struct conv_id_t {
        uint32_t value = 0;

        constexpr conv_id_t() = default;
        constexpr explicit conv_id_t(uint32_t v) : value(v) {}

        bool operator==(const conv_id_t& o) const { return value == o.value; }
        bool operator!=(const conv_id_t& o) const { return value != o.value; }
    };

    struct send_req {
        conv_id_t conv;
        char* data;
        uint32_t len;
        std::move_only_function<void(bool)> cb;
    };

    struct accept_req {
        std::move_only_function<void(conv_id_t)> cb;
    };

    struct connect_req {
        sockaddr_in target;
        std::move_only_function<void(std::variant<conv_id_t, std::exception_ptr>)> cb;
    };

    // Handshake protocol:
    // SYN  = 4 bytes: conv (proposed by client, network order)
    // ACK  = 4 bytes: conv (echoed by server, network order)
    static constexpr uint8_t HANDSHAKE_SYN = 0xF0;
    static constexpr uint8_t HANDSHAKE_ACK = 0xF1;

    struct __attribute__((packed)) handshake_pkt {
        uint8_t  type;   // SYN or ACK
        uint32_t conv;   // proposed/confirmed conv_id
    };

    static constexpr uint32_t HANDSHAKE_PKT_SIZE = sizeof(handshake_pkt);

}  // namespace pump::scheduler::kcp::common

#endif //ENV_SCHEDULER_KCP_COMMON_STRUCT_HH
