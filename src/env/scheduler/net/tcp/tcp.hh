//
// Created by null on 2025/7/21.
//

#ifndef ENV_SCHEDULER_TCP_NET_HH
#define ENV_SCHEDULER_TCP_NET_HH

#include "pump/sender/flat.hh"
#include "./senders/conn.hh"
#include "./senders/connect.hh"
#include "./senders/join.hh"
#include "./senders/stop.hh"
#include "./common/layers.hh"
#include "env/scheduler/net/common/session.hh"
#include "env/scheduler/net/common/frame_receiver.hh"
#include "env/scheduler/net/common/send_sender.hh"
#include "env/scheduler/net/common/recv_sender.hh"

namespace pump::scheduler::tcp {

    template <typename scheduler_t>
    inline auto
    wait_connection(scheduler_t* sche) {
        return senders::conn::sender<scheduler_t>(sche);
    }

    template <typename scheduler_t>
    inline auto
    connect(scheduler_t* sche, const char* address, uint16_t port) {
        return senders::connect::sender<scheduler_t>(sche, address, port);
    }

    template <typename scheduler_t, typename session_t>
    inline auto
    join(scheduler_t* sche, session_t* session) {
        return senders::join::sender<scheduler_t, session_t>(sche, session);
    }

    template <typename scheduler_t, typename session_t>
    inline auto
    stop(scheduler_t* sche, session_t* session) {
        return senders::stop::sender<scheduler_t, session_t>(sche, session);
    }

    // recv: session-only, no scheduler needed
    template <typename session_t>
    inline auto
    recv(session_t* session){
        return pump::scheduler::net::recv(session);
    }

    // send: session-only, routes through do_send tag in bind layer
    template <typename session_t>
    inline auto
    send(session_t* session, void* data, uint32_t len) {
        return pump::scheduler::net::send(session, data, len);
    }
}

#endif //ENV_SCHEDULER_TCP_NET_HH
