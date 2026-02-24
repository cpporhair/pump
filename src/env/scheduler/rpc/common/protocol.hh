
#ifndef ENV_SCHEDULER_RPC_COMMON_PROTOCOL_HH
#define ENV_SCHEDULER_RPC_COMMON_PROTOCOL_HH

#include <concepts>
#include <cstdint>
#include <cstddef>
#include <bits/types/struct_iovec.h>
#include <array>

namespace pump::scheduler::rpc::common {

    // Protocol concept（plan.md §二）
    // 约束与具体协议相关的能力（编解码、消息类型）
    // request_id、module_id、flags 等帧头字段由 RPC 统一帧头定义，不属于 Protocol 的职责
    template <typename P>
    concept Protocol = requires {
        typename P::message_type;

        { P::decode(std::declval<uint8_t>(), std::declval<const char*>(), std::declval<size_t>()) }
            -> std::same_as<typename P::message_type>;
    };

}

#endif //ENV_SCHEDULER_RPC_COMMON_PROTOCOL_HH
