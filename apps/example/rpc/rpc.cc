#include <print>
#include <cstdio>
#include <cassert>
#include "env/scheduler/rpc/rpc.hh"

#include "../../kv/runtime/scheduler_objects.hh"
#include "env/runtime/runner.hh"
#include "env/runtime/share_nothing.hh"
#include "env/scheduler/net/io_uring/scheduler.hh"
#include "env/scheduler/task/sender.hh"
#include "pump/sender/repeat.hh"
#include "pump/sender/submit.hh"

#include "./server.hh"
#include "./client.hh"
#include "env/scheduler/net/io_uring/connect_scheduler.hh"

int
main(int argc, char **argv) {
    using conn_sched_t = pump::scheduler::net::io_uring::connect_scheduler<pump::scheduler::net::senders::conn::op>;
    using acpt_sched_t = pump::scheduler::net::io_uring::accept_scheduler<pump::scheduler::net::senders::conn::op>;
    using sess_sched_t = pump::scheduler::net::io_uring::session_scheduler<
        pump::scheduler::net::senders::join::op,
        pump::scheduler::net::senders::recv::op,
        pump::scheduler::net::senders::send::op,
        pump::scheduler::net::senders::stop::op
    >;

    std::jthread([](){ apps::rpc::server::start<acpt_sched_t, sess_sched_t>(0); }).detach();
    std::jthread([](){ apps::rpc::client::start<conn_sched_t, sess_sched_t>(0); }).detach();

    std::this_thread::sleep_for(std::chrono::seconds(100));
}
