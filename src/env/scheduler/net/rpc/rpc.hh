
#ifndef PUMP_ENV_SCHEDULER_RPC_RPC_HH
#define PUMP_ENV_SCHEDULER_RPC_RPC_HH

#include "./server/serv.hh"
#include "./client/call.hh"
#include "./common/rpc_layer.hh"

namespace pump::scheduler::rpc {

    // --- Server API ---
    // session_t deduced from session, must have rpc_session_layer

    template<uint16_enum_concept auto ...service_ids, typename session_t>
    auto
    serv(session_t* session) {
        return server::serv<0, session_t, service_ids...>(session);
    }

    template<uint16_t concurrency, uint16_enum_concept auto ...service_ids, typename session_t>
    auto
    serv(session_t* session) {
        return server::serv<concurrency, session_t, service_ids...>(session);
    }

    // --- Client API ---

    template <uint16_enum_concept auto service_id, typename session_t, typename ...args_t>
    auto
    call(session_t* session, args_t&& ...args) {
        return client::call<service_id>(session, __fwd__(args)...);
    }
}

#endif //PUMP_ENV_SCHEDULER_RPC_RPC_HH
