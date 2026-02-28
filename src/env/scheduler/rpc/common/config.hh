#ifndef ENV_SCHEDULER_RPC_COMMON_CONFIG_HH
#define ENV_SCHEDULER_RPC_COMMON_CONFIG_HH

#include <cstdint>

namespace pump::rpc {

    struct channel_config {
        uint32_t max_pending         = 1024;
        uint64_t default_timeout_ms  = 5000;   // 0 = no timeout
        bool     enable_heartbeat    = false;
        uint64_t heartbeat_interval_ms = 1000;
        uint32_t heartbeat_max_miss  = 3;
    };

}

#endif //ENV_SCHEDULER_RPC_COMMON_CONFIG_HH
