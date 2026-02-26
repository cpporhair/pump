//
// Created by null on 2025/7/21.
//

#ifndef ENV_SCHEDULER_NET_NET_HH
#define ENV_SCHEDULER_NET_NET_HH

#include "pump/sender/flat.hh"
#include "./senders/conn.hh"
#include "./senders/connect.hh"
#include "./senders/recv.hh"
#include "./senders/send.hh"
#include "./senders/join.hh"
#include "./senders/stop.hh"

namespace pump::scheduler::net {
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

    template <typename scheduler_t>
    inline auto
    join(scheduler_t* sche, common::session_id_t session_id) {
        return senders::join::sender<scheduler_t>(sche, session_id);
    }

    template <typename scheduler_t>
    inline auto
    stop(scheduler_t* sche, common::session_id_t session_id) {
        return senders::stop::sender<scheduler_t>(sche, session_id);
    }

    template <typename scheduler_t>
    inline auto
    recv(scheduler_t *scheduler, common::session_id_t session_id){
        return senders::recv::sender<scheduler_t>(scheduler, session_id);
    }

    template <typename scheduler_t>
    inline auto
    send(scheduler_t *scheduler, common::session_id_t sid, void* data, uint16_t len) {
        return senders::send::sender<scheduler_t>(
            scheduler,
            sid,
            common::net_frame(static_cast<char *>(data), len + sizeof(common::net_frame::_len))
        );
    }
}

#endif //ENV_SCHEDULER_NET_NET_HH
