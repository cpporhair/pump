
#ifndef PUMP_ENV_SCHEDULER_RPC_RPC_HH
#define PUMP_ENV_SCHEDULER_RPC_RPC_HH

#include "./server/serv.hh"
#include "./client/call.hh"
#include "./transport/tcp.hh"

namespace pump::scheduler::rpc {

    // --- Backward-compatible TCP API (default transport) ---

    template<uint16_enum_concept auto ...service_ids>
    auto
    serv(auto sche, tcp::common::session_id_t sid) {
        return server::serv<transport::tcp, 0, __typ__(sche), service_ids...>(sche, sid);
    }

    template<uint16_t concurrency, uint16_enum_concept auto ...service_ids>
    auto
    serv(auto sche, tcp::common::session_id_t sid) {
        return server::serv<transport::tcp, concurrency, __typ__(sche), service_ids...>(sche, sid);
    }

    template <uint16_enum_concept auto service_id, typename scheduler_t, typename ...args_t>
    auto
    call(scheduler_t* sche, tcp::common::session_id_t sid, args_t&& ...args) {
        return client::call<transport::tcp, service_id>(sche, sid, __fwd__(args)...);
    }

    // --- Explicit transport API ---

    template<typename transport_t, uint16_enum_concept auto ...service_ids>
    auto
    serv(auto sche, typename transport_t::address_type addr) {
        return server::serv<transport_t, 0, __typ__(sche), service_ids...>(sche, addr);
    }

    template<typename transport_t, uint16_t concurrency, uint16_enum_concept auto ...service_ids>
    auto
    serv(auto sche, typename transport_t::address_type addr) {
        return server::serv<transport_t, concurrency, __typ__(sche), service_ids...>(sche, addr);
    }

    template <typename transport_t, uint16_enum_concept auto service_id, typename scheduler_t, typename ...args_t>
    auto
    call(scheduler_t* sche, typename transport_t::address_type addr, args_t&& ...args) {
        return client::call<transport_t, service_id>(sche, addr, __fwd__(args)...);
    }
}

#endif //PUMP_ENV_SCHEDULER_RPC_RPC_HH
