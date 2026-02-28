#ifndef ENV_SCHEDULER_RPC_COMMON_ERROR_HH
#define ENV_SCHEDULER_RPC_COMMON_ERROR_HH

#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace pump::rpc {

    enum class error_code : uint16_t {
        success          = 0,
        method_not_found = 1,
        remote_error     = 2,
        codec_error      = 3,
        timeout          = 4,
        internal_error   = 5,
    };

    struct rpc_timeout_error : std::runtime_error {
        uint32_t request_id;
        explicit rpc_timeout_error(uint32_t rid)
            : std::runtime_error("rpc timeout"), request_id(rid) {}
    };

    struct method_not_found_error : std::runtime_error {
        uint16_t method_id;
        explicit method_not_found_error(uint16_t mid)
            : std::runtime_error("method not found"), method_id(mid) {}
    };

    struct remote_rpc_error : std::runtime_error {
        error_code code;
        remote_rpc_error(error_code c, const char* msg)
            : std::runtime_error(msg), code(c) {}
    };

    struct connection_lost_error : std::runtime_error {
        connection_lost_error()
            : std::runtime_error("connection lost") {}
    };

    struct channel_closed_error : std::runtime_error {
        channel_closed_error()
            : std::runtime_error("channel closed") {}
    };

    struct pending_overflow_error : std::runtime_error {
        pending_overflow_error()
            : std::runtime_error("pending requests overflow") {}
    };

    // Error payload format: [error_code:2B][error_message:...]
    struct error_payload {
        error_code code;
        char message[256];
    };

    inline error_payload
    parse_error_payload(const uint8_t* data, size_t len) {
        error_payload ep{};
        if (len >= 2) {
            std::memcpy(&ep.code, data, 2);
            size_t msg_len = len - 2;
            if (msg_len > sizeof(ep.message) - 1)
                msg_len = sizeof(ep.message) - 1;
            if (msg_len > 0)
                std::memcpy(ep.message, data + 2, msg_len);
            ep.message[msg_len] = '\0';
        }
        return ep;
    }

    inline size_t
    write_error_payload(uint8_t* buf, error_code code, const char* msg) {
        std::memcpy(buf, &code, 2);
        size_t msg_len = msg ? std::strlen(msg) : 0;
        if (msg_len > 0)
            std::memcpy(buf + 2, msg, msg_len);
        return 2 + msg_len;
    }

}

#endif //ENV_SCHEDULER_RPC_COMMON_ERROR_HH
