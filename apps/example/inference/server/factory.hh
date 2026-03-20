#pragma once

#include "env/scheduler/net/tcp/common/layers.hh"
#include "env/scheduler/net/common/session.hh"
#include "env/scheduler/net/common/frame_receiver.hh"

namespace inference {

// Session factory: TCP + length-prefix framing + frame_receiver
// Uses default length_prefix_unpacker (4-byte total_len header)
struct session_factory {
    template<typename sched_t>
    using session_type = pump::scheduler::net::session_t<
        pump::scheduler::tcp::common::tcp_bind<sched_t>,
        pump::scheduler::tcp::common::tcp_ring_buffer<>,
        pump::scheduler::net::frame_receiver
    >;

    template<typename sched_t>
    static auto* create(int fd, sched_t* sche) {
        return new session_type<sched_t>(
            pump::scheduler::tcp::common::tcp_bind<sched_t>(fd, sche),
            pump::scheduler::tcp::common::tcp_ring_buffer<>(),
            pump::scheduler::net::frame_receiver()
        );
    }
};

} // namespace inference
