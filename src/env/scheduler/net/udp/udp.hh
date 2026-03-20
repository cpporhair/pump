
#ifndef ENV_SCHEDULER_UDP_HH
#define ENV_SCHEDULER_UDP_HH

#include "./senders/recv.hh"
#include "./senders/send.hh"

namespace pump::scheduler::udp {

    template <typename scheduler_t>
    inline auto
    recv(scheduler_t* sche) {
        return senders::recv::sender<scheduler_t>(sche);
    }

    template <typename scheduler_t>
    inline auto
    send(scheduler_t* sche, common::endpoint ep, void* data, uint32_t len) {
        return senders::send::sender<scheduler_t>(
            sche, ep,
            common::datagram(static_cast<char*>(data), len)
        );
    }
}

#endif //ENV_SCHEDULER_UDP_HH
