#include <print>
#include <cstdio>
#include <cstring>
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
#include "./kcp_rpc.hh"
#include "env/scheduler/tcp/io_uring/connect_scheduler.hh"

static void run_tcp_rpc() {
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

static void usage() {
    printf("Usage: rpc [mode] [options]\n\n");
    printf("Modes:\n");
    printf("  tcp      TCP RPC (default)\n");
    printf("  kcp      KCP RPC (self-contained server+client)\n\n");
    printf("Options:\n");
    printf("  --epoll  Use epoll backend for KCP (default: io_uring)\n");
}

int
main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    const char* mode = argc > 1 ? argv[1] : "tcp";
    bool epoll = false;
    for (int i = 2; i < argc; ++i)
        if (std::strcmp(argv[i], "--epoll") == 0) epoll = true;

    if (std::strcmp(mode, "tcp") == 0) {
        run_tcp_rpc();
    } else if (std::strcmp(mode, "kcp") == 0) {
        run_kcp_rpc(epoll);
    } else {
        printf("Unknown mode: %s\n\n", mode);
        usage();
        return 1;
    }

    return 0;
}
