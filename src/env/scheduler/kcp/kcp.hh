
#ifndef ENV_SCHEDULER_KCP_HH
#define ENV_SCHEDULER_KCP_HH

#include <chrono>

#include "./senders/recv.hh"
#include "./senders/send.hh"
#include "./senders/accept.hh"
#include "./senders/connect.hh"

namespace pump::scheduler::kcp {

    inline uint32_t clock_ms() {
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
        return static_cast<uint32_t>(ms.count());
    }

    template <typename scheduler_t>
    inline auto
    recv(scheduler_t* sche, common::conv_id_t conv) {
        return senders::recv::sender<scheduler_t>(sche, conv);
    }

    template <typename scheduler_t>
    inline auto
    send(scheduler_t* sche, common::conv_id_t conv, void* data, uint32_t len) {
        return senders::send::sender<scheduler_t>(
            sche, conv,
            static_cast<char*>(data), len
        );
    }

    template <typename scheduler_t>
    inline auto
    accept(scheduler_t* sche) {
        return senders::accept::sender<scheduler_t>(sche);
    }

    template <typename scheduler_t>
    inline auto
    connect(scheduler_t* sche, const char* ip, uint16_t port) {
        return senders::connect::sender<scheduler_t>(sche, ip, port);
    }

}  // namespace pump::scheduler::kcp

#endif //ENV_SCHEDULER_KCP_HH
