//
// Created by null on 2025/7/21.
//

#ifndef ENV_SCHEDULER_NET_NET_HH
#define ENV_SCHEDULER_NET_NET_HH

#include "pump/sender/flat.hh"
#include "./senders/conn.hh"
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

    inline auto
    wait_connection() {
        return pump::sender::flat_map([]<typename scheduler_t>(scheduler_t* sche) {
            return wait_connection(sche);
        });
    }

    template <typename scheduler_t>
    inline auto
    join(scheduler_t* sche, uint64_t session_id) {
        return senders::join::sender<scheduler_t>(sche, session_id);
    }

    // 5.1: flat_map overload for join
    inline auto
    join(uint64_t session_id) {
        return pump::sender::flat_map([session_id]<typename scheduler_t>(scheduler_t* sche) {
            return join(sche, session_id);
        });
    }

    template <typename scheduler_t>
    inline auto
    stop(scheduler_t* sche, uint64_t session_id) {
        return senders::stop::sender<scheduler_t>(sche, session_id);
    }

    // 5.1: flat_map overload for stop
    inline auto
    stop(uint64_t session_id) {
        return pump::sender::flat_map([session_id]<typename scheduler_t>(scheduler_t* sche) {
            return stop(sche, session_id);
        });
    }

    template <typename scheduler_t>
    inline auto
    recv(scheduler_t *scheduler, uint64_t session_id){
        return senders::recv::sender<scheduler_t>(scheduler, session_id);
    }

    // 5.1: flat_map overload for recv
    inline auto
    recv(uint64_t session_id) {
        return pump::sender::flat_map([session_id]<typename scheduler_t>(scheduler_t* sche) {
            return recv(sche, session_id);
        });
    }

    template <typename scheduler_t>
    inline auto
    send(scheduler_t *scheduler, uint64_t sid, iovec *vec, size_t cnt) {
        return senders::send::sender<scheduler_t>(scheduler, sid, vec, cnt);
    }

    // 5.1: flat_map overload for send
    inline auto
    send(uint64_t sid, iovec *vec, size_t cnt) {
        return pump::sender::flat_map([sid, vec, cnt]<typename scheduler_t>(scheduler_t* sche) {
            return send(sche, sid, vec, cnt);
        });
    }
}

#endif //ENV_SCHEDULER_NET_NET_HH
