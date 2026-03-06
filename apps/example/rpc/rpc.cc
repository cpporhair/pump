#include <print>
#include <cstdio>
#include <cassert>
#include "env/scheduler/rpc/rpc.hh"

#include "../../kv/runtime/scheduler_objects.hh"
#include "env/runtime/runner.hh"
#include "env/runtime/share_nothing.hh"
#include "env/scheduler/tcp/io_uring/scheduler.hh"
#include "env/scheduler/task/sender.hh"
#include "pump/sender/repeat.hh"
#include "pump/sender/submit.hh"

#include "./server.hh"
#include "./client.hh"
#include "env/scheduler/tcp/io_uring/connect_scheduler.hh"

int
main(int argc, char **argv) {
    using conn_sched_t = pump::scheduler::tcp::io_uring::connect_scheduler<pump::scheduler::tcp::senders::conn::op>;
    using acpt_sched_t = pump::scheduler::tcp::io_uring::accept_scheduler<pump::scheduler::tcp::senders::conn::op>;
    using sess_sched_t = pump::scheduler::tcp::io_uring::session_scheduler<
        pump::scheduler::tcp::senders::join::op,
        pump::scheduler::tcp::senders::recv::op,
        pump::scheduler::tcp::senders::send::op,
        pump::scheduler::tcp::senders::stop::op
    >;

    std::jthread([](){ apps::rpc::server::start<acpt_sched_t, sess_sched_t>(0); }).detach();
    std::jthread([](){ apps::rpc::client::start<conn_sched_t, sess_sched_t>(0); }).detach();

    std::this_thread::sleep_for(std::chrono::seconds(100));
}
