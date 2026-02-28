
#include <print>
#include <cassert>
#include <thread>
#include <cstring>

#include "pump/sender/flat.hh"
#include "pump/sender/just.hh"
#include "pump/sender/repeat.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/on.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/submit.hh"
#include "pump/core/context.hh"
#include "env/runtime/share_nothing.hh"
#include "env/runtime/runner.hh"
#include "env/scheduler/net/net.hh"
#include "env/scheduler/net/io_uring/scheduler.hh"
#include "env/scheduler/net/io_uring/connect_scheduler.hh"
#include "env/scheduler/net/epoll/scheduler.hh"
#include "env/scheduler/net/epoll/connect_scheduler.hh"
#include "env/scheduler/task/tasks_scheduler.hh"

#include "env/scheduler/rpc/rpc.hh"

using namespace pump;
using namespace pump::sender;

using task_scheduler_t = scheduler::task::scheduler;

// Same method IDs as server
enum rpc_method : uint16_t {
    echo  = 1,
    add   = 2,
};

template <typename connect_scheduler_t, typename session_scheduler_t>
using runtime_schedulers = env::runtime::runtime_schedulers<
    task_scheduler_t,
    connect_scheduler_t,
    session_scheduler_t
>;

template <typename connect_scheduler_t, typename session_scheduler_t>
auto
session_proc(const runtime_schedulers<connect_scheduler_t, session_scheduler_t> *rs,
             const scheduler::net::common::session_id_t sid)
{
    auto session_count = rs->template get_schedulers<session_scheduler_t>().size();
    auto core_idx = sid.raw() % session_count;
    auto* session_sched = rs->template get_schedulers<session_scheduler_t>()[core_idx];
    auto* task_sched = rs->template get_schedulers<task_scheduler_t>()[core_idx];

    auto ch = rpc::make_channel<rpc::raw_codec>(
        sid, session_sched, task_sched);

    // Start serve loop in background (processes responses from server)
    rpc::serve<>(ch)
        >> submit(core::make_root_context());

    rpc::raw_codec codec;

    // Test 1: echo call
    uint64_t echo_val = 42;
    return scheduler::net::join(session_sched, sid)
        >> flat_map([ch, echo_val](...) {
            std::println("Test 1: echo call with value {}", echo_val);
            return rpc::call(ch, rpc_method::echo, echo_val);
        })
        >> then([](rpc::payload_view pv) {
            rpc::raw_codec codec;
            auto result = codec.decode_payload<uint64_t>(pv);
            std::println("  Echo response: {} (expected: 42)", result);
            assert(result == 42);
        })
        // Test 2: add call
        >> flat_map([ch](...) {
            uint64_t add_val = 200;
            std::println("Test 2: add call with value {}", add_val);
            return rpc::call(ch, rpc_method::add, add_val);
        })
        >> then([](rpc::payload_view pv) {
            rpc::raw_codec codec;
            auto result = codec.decode_payload<uint64_t>(pv);
            std::println("  Add response: {} (expected: 300)", result);
            assert(result == 300);
        })
        // Test 3: notify (fire-and-forget, no response expected)
        >> flat_map([ch](...) {
            uint64_t notify_val = 999;
            std::println("Test 3: notify with value {}", notify_val);
            return rpc::notify(ch, rpc_method::echo, notify_val);
        })
        >> then([]() {
            std::println("  Notify sent successfully");
            std::println("\nAll tests passed!");
        })
        // Cleanup
        >> then([ch]() {
            ch->close();
        })
        >> any_exception([](std::exception_ptr e) {
            try { std::rethrow_exception(e); }
            catch (const std::exception& ex) {
                std::println(stderr, "Test failed: {}", ex.what());
            }
            return just();
        });
}

template <typename connect_scheduler_t, typename session_scheduler_t>
auto
create_runtime_schedulers() {
    using rs_t = runtime_schedulers<connect_scheduler_t, session_scheduler_t>;
    auto* rs = new rs_t();

    unsigned queue_depth = 256;
    uint32_t num_cores = std::thread::hardware_concurrency();

    auto* connect_sched = new connect_scheduler_t();
    auto cfg = scheduler::net::common::scheduler_config{};
    if (connect_sched->init(cfg) < 0) {
        std::println(stderr, "Failed to init connect_scheduler");
        std::exit(1);
    }

    std::vector<session_scheduler_t*> session_scheds(num_cores);
    for (uint32_t i = 0; i < num_cores; ++i) {
        session_scheds[i] = new session_scheduler_t();
        if constexpr (requires { session_scheds[i]->init(queue_depth); }) {
            if (session_scheds[i]->init(queue_depth) < 0) {
                std::println(stderr, "Failed to init session_scheduler for core {}", i);
                std::exit(1);
            }
        }
    }

    for (uint32_t i = 0; i < num_cores; ++i) {
        auto* task_sched = new task_scheduler_t(i);
        rs->add_core_schedulers(
            task_sched,
            i == 0 ? connect_sched : nullptr,
            session_scheds[i]
        );
    }

    return rs;
}

template <typename connect_scheduler_t, typename session_scheduler_t>
void
run_client(const char* address, uint16_t port) {
    using rs_t = runtime_schedulers<connect_scheduler_t, session_scheduler_t>;
    just()
        >> get_context<rs_t *>()
        >> then([address, port](rs_t *rs) {
            auto* connect_sched = rs->template get_schedulers<connect_scheduler_t>()[0];
            return scheduler::net::connect(connect_sched, address, port)
                >> then([rs](scheduler::net::common::session_id_t sid) {
                    std::println("Connected to server, session_id: {}", sid.raw());
                    session_proc<connect_scheduler_t, session_scheduler_t>(rs, sid)
                        >> submit(core::make_root_context());
                })
                >> any_exception([](std::exception_ptr e) {
                    try { std::rethrow_exception(e); }
                    catch (const std::exception& ex) {
                        std::println(stderr, "Connect error: {}", ex.what());
                    }
                    return just();
                })
                >> submit(core::make_root_context(rs));
        })
        >> get_context<rs_t *>()
        >> then([](rs_t *rs) {
            env::runtime::start(rs->schedulers_by_core);
        })
        >> submit(core::make_root_context(
            create_runtime_schedulers<connect_scheduler_t, session_scheduler_t>()));
}

int
main(int argc, char **argv) {
    bool use_epoll = false;
    const char* address = "127.0.0.1";
    uint16_t port = 9090;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--epoll") == 0) {
            use_epoll = true;
        } else if (std::strcmp(argv[i], "--address") == 0 && i + 1 < argc) {
            address = argv[++i];
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        }
    }

    if (use_epoll) {
        std::println("Using epoll backend, connecting to {}:{}", address, port);
        using connect_sched_t = scheduler::net::epoll::connect_scheduler<
            pump::scheduler::net::senders::conn::op>;
        using session_sched_t = scheduler::net::epoll::session_scheduler<
            scheduler::net::senders::join::op,
            scheduler::net::senders::recv::op,
            scheduler::net::senders::send::op,
            scheduler::net::senders::stop::op
        >;
        run_client<connect_sched_t, session_sched_t>(address, port);
    } else {
        std::println("Using io_uring backend, connecting to {}:{}", address, port);
        using connect_sched_t = scheduler::net::io_uring::connect_scheduler<
            pump::scheduler::net::senders::conn::op>;
        using session_sched_t = scheduler::net::io_uring::session_scheduler<
            scheduler::net::senders::join::op,
            scheduler::net::senders::recv::op,
            scheduler::net::senders::send::op,
            scheduler::net::senders::stop::op
        >;
        run_client<connect_sched_t, session_sched_t>(address, port);
    }

    return 0;
}
