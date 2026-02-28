#ifndef ENV_SCHEDULER_RPC_COMMON_MESSAGE_TYPE_HH
#define ENV_SCHEDULER_RPC_COMMON_MESSAGE_TYPE_HH

#include <cstdint>

namespace pump::rpc {

    enum class message_type : uint8_t {
        request      = 0x01,
        response     = 0x02,
        notification = 0x03,
        error        = 0x04,
    };

}

#endif //ENV_SCHEDULER_RPC_COMMON_MESSAGE_TYPE_HH
