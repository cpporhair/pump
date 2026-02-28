
#ifndef PUMP_ENV_SCHEDULER_RPC_RPC_HH
#define PUMP_ENV_SCHEDULER_RPC_RPC_HH

#include "./server/serv.hh"
#include "./client/call.hh"

namespace pump::scheduler::rpc {
    template<server::uint16_enum_concept auto ...service_ids>
    auto
    serv(auto sche, auto seid) {
        return server::serv<__typ__(sche), service_ids...>(sche, seid);
    }

    template <server::uint16_enum_concept auto service_id, typename scheduler_t, typename ...args_t>
    auto
    call(scheduler_t* sche, net::common::session_id_t sid, args_t&& ...args) {
        return client::call<service_id>(sche, sid, __fwd__(args)...);
    }
}

#endif //PUMP_ENV_SCHEDULER_RPC_RPC_HH