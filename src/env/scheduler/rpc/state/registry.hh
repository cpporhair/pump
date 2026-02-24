
#ifndef ENV_SCHEDULER_RPC_STATE_REGISTRY_HH
#define ENV_SCHEDULER_RPC_STATE_REGISTRY_HH

#include <cstdint>
#include <unordered_map>

#include "env/scheduler/net/common/struct.hh"
#include "./pending_map.hh"

namespace pump::scheduler::rpc::state {

    // session_rpc_state（plan.md §六）
    // 每个 session 维护的 RPC 状态，与具体协议无关
    struct
    session_rpc_state {
        uint32_t next_request_id = 0;                // D-1: session 内自增
        pending_requests_map pending{256};            // D-7: 默认 256 容量
    };

    // rpc_state_registry（plan.md §六, D-9）
    // RPC 层独立维护 session_id → rpc_state 映射
    // 通过 get_or_bind 首次访问时自动创建（lazy bind）
    struct
    rpc_state_registry {
        std::unordered_map<uint64_t, session_rpc_state> states;

        auto& get_or_bind(net::common::session_id_t sid) {
            auto key = sid.raw();
            auto it = states.find(key);
            if (it != states.end()) return it->second;
            return states.emplace(key, session_rpc_state{}).first->second;
        }

        void unbind(net::common::session_id_t sid) {
            states.erase(sid.raw());
        }
    };

    // 全局 registry 实例（每个 scheduler 线程一个，符合单线程不变量）
    inline rpc_state_registry& get_registry() {
        static thread_local rpc_state_registry registry;
        return registry;
    }

}

#endif //ENV_SCHEDULER_RPC_STATE_REGISTRY_HH
