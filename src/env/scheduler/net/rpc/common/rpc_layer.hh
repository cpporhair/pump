
#ifndef ENV_SCHEDULER_RPC_COMMON_RPC_LAYER_HH
#define ENV_SCHEDULER_RPC_COMMON_RPC_LAYER_HH

#include "env/scheduler/net/common/session_tags.hh"

namespace pump::scheduler::rpc {

    // RPC session tags
    struct rpc_is_closed{};  constexpr inline rpc_is_closed rpc_is_closed{};
    struct rpc_close{};      constexpr inline rpc_close rpc_close{};

    // RPC session layer: tracks RPC-level closed state.
    // Participates in on_error broadcast to detect transport failures.
    struct
    rpc_session_layer {
        bool closed = false;

        template<typename owner_t>
        bool
        invoke(const struct rpc_is_closed&, owner_t&) {
            return closed;
        }

        template<typename owner_t>
        void
        invoke(const struct rpc_close&, owner_t&) {
            closed = true;
        }

        template<typename owner_t>
        void
        invoke(const struct ::pump::scheduler::net::on_error&, owner_t&, std::exception_ptr) {
            closed = true;
        }
    };

}

#endif //ENV_SCHEDULER_RPC_COMMON_RPC_LAYER_HH
