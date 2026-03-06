
#ifndef PUMP_ENV_SCHEDULER_RPC_TRANSPORT_TCP_HH
#define PUMP_ENV_SCHEDULER_RPC_TRANSPORT_TCP_HH

#include "env/scheduler/tcp/tcp.hh"

namespace pump::scheduler::rpc::transport {

    struct tcp {
        using address_type = ::pump::scheduler::tcp::common::session_id_t;
        using frame_type   = ::pump::common::net_frame;

        static auto recv(auto* sche, address_type addr) {
            return ::pump::scheduler::tcp::recv(sche, addr);
        }

        static auto send(auto* sche, address_type addr, void* data, uint32_t len) {
            return ::pump::scheduler::tcp::send(sche, addr, data, len);
        }

        static uint64_t address_raw(const address_type& addr) {
            return addr._value;
        }
    };

}

#endif //PUMP_ENV_SCHEDULER_RPC_TRANSPORT_TCP_HH
