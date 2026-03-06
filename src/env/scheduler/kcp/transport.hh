
#ifndef ENV_SCHEDULER_KCP_TRANSPORT_HH
#define ENV_SCHEDULER_KCP_TRANSPORT_HH

#include "env/scheduler/kcp/kcp.hh"

namespace pump::scheduler::rpc::transport {

    struct kcp {
        using address_type = ::pump::scheduler::kcp::common::conv_id_t;
        using frame_type   = ::pump::common::net_frame;

        static auto recv(auto* sche, address_type addr) {
            return ::pump::scheduler::kcp::recv(sche, addr);
        }

        static auto send(auto* sche, address_type addr, void* data, uint32_t len) {
            return ::pump::scheduler::kcp::send(sche, addr, data, len);
        }

        static uint64_t address_raw(const address_type& addr) {
            return addr.value;
        }
    };

}

#endif //ENV_SCHEDULER_KCP_TRANSPORT_HH
