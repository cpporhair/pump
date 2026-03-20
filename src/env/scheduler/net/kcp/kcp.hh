
#ifndef ENV_SCHEDULER_KCP_HH
#define ENV_SCHEDULER_KCP_HH

#include <chrono>

#include "./senders/accept.hh"
#include "./senders/connect.hh"
#include "./common/layers.hh"
#include "env/scheduler/net/common/session.hh"
#include "env/scheduler/net/common/frame_receiver.hh"
#include "env/scheduler/net/common/send_sender.hh"
#include "env/scheduler/net/common/recv_sender.hh"

namespace pump::scheduler::kcp {

    inline uint32_t clock_ms() {
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
        return static_cast<uint32_t>(ms.count());
    }

    // recv: session-only, no scheduler needed (same as TCP recv)
    template <typename session_t>
    inline auto
    recv(session_t* session) {
        return pump::scheduler::net::recv(session);
    }

    // send: session-only, routes through do_send tag in bind layer
    template <typename session_t>
    inline auto
    send(session_t* session, void* data, uint32_t len) {
        return pump::scheduler::net::send(session, data, len);
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
