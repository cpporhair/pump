
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
#include "env/scheduler/net/epoll/scheduler.hh"
#include "env/scheduler/task/tasks_scheduler.hh"

#include "env/scheduler/rpc/rpc.hh"

using namespace pump;
using namespace pump::sender;

using task_scheduler_t = scheduler::task::scheduler;

// --- Service definitions ---

// Method IDs
enum rpc_method : uint16_t {
    echo  = 1,
    add   = 2,
};

// Echo service: returns payload unchanged
template<>
struct rpc::service<rpc_method::echo> {
    static constexpr bool is_service = true;

    static auto
    handle(rpc::handler_context&& ctx) {
        rpc::raw_codec codec;
        auto val = codec.decode_payload<uint64_t>(ctx.payload());
        return just(std::move(val));
    }
};

// Add service: adds 100 to the uint64_t payload
template<>
struct rpc::service<rpc_method::add> {
    static constexpr bool is_service = true;

    static auto
    handle(rpc::handler_context&& ctx) {
        rpc::raw_codec codec;
        auto val = codec.decode_payload<uint64_t>(ctx.payload());
        auto result = val + 100;
        return just(std::move(result));
    }
};

// --- Runtime setup ---

template <typename accept_scheduler_t, typename session_scheduler_t>
using runtime_schedulers = env::runtime::runtime_schedulers<
    task_scheduler_t,
    accept_scheduler_t,
    session_scheduler_t
>;

template <typename accept_scheduler_t, typename session_scheduler_t>
auto
session_proc(const runtime_schedulers<accept_scheduler_t, session_scheduler_t> *rs,
             const scheduler::net::common::session_id_t sid)
{
    auto session_count = rs->template get_schedulers<session_scheduler_t>().size();
    auto core_idx = sid.raw() % session_count;
    auto* session_sched = rs->template get_schedulers<session_scheduler_t>()[core_idx];
    auto* task_sched = rs->template get_schedulers<task_scheduler_t>()[core_idx];

    auto ch = rpc::make_channel<rpc::raw_codec>(
        sid, session_sched, task_sched);

    return scheduler::net::join(session_sched, sid)
        >> flat_map([ch](...) {
            std::println("Session joined, starting serve loop");
            return rpc::serve<rpc_method::echo, rpc_method::add>(ch);
        })
        >> any_exception([](std::exception_ptr e) {
            try { std::rethrow_exception(e); }
            catch (const std::exception& ex) {
                std::println(stderr, "Session error: {}", ex.what());
            }
            return just();
        });
}

template <typename accept_scheduler_t, typename session_scheduler_t>
auto
create_runtime_schedulers() {
    using rs_t = runtime_schedulers<accept_scheduler_t, session_scheduler_t>;
    auto* rs = new rs_t();

    const char* address = "0.0.0.0";
    uint16_t port = 9090;
    unsigned queue_depth = 256;
    uint32_t num_cores = std::thread::hardware_concurrency();

    auto* accept_sched = new accept_scheduler_t();
    if constexpr (requires { accept_sched->init(address, port, queue_depth); }) {
        if (accept_sched->init(address, port, queue_depth) < 0) {
            std::println(stderr, "Failed to init accept_scheduler");
            std::exit(1);
        }
    } else {
        if (accept_sched->init(address, port) < 0) {
            std::println(stderr, "Failed to init accept_scheduler");
            std::exit(1);
        }
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
            i == 0 ? accept_sched : nullptr,
            session_scheds[i]
        );
    }

    return rs;
}

template <typename accept_scheduler_t, typename session_scheduler_t>
void
run_server() {
    using rs_t = runtime_schedulers<accept_scheduler_t, session_scheduler_t>;
    just()
        >> get_context<rs_t *>()
        >> then([](rs_t *rs) {
            std::println("RPC Echo Server listening on port 9090");
            return just()
                >> forever()
                >> flat_map([rs](...) {
                    return scheduler::net::wait_connection(
                        rs->template get_schedulers<accept_scheduler_t>()[0]);
                })
                >> then([rs](scheduler::net::common::session_id_t s) {
                    std::println("New connection: {}", s.raw());
                    session_proc<accept_scheduler_t, session_scheduler_t>(rs, s)
                        >> submit(core::make_root_context());
                })
                >> count()
                >> submit(core::make_root_context(rs));
        })
        >> get_context<rs_t *>()
        >> then([](rs_t *rs) {
            env::runtime::start(rs->schedulers_by_core);
        })
        >> submit(core::make_root_context(
            create_runtime_schedulers<accept_scheduler_t, session_scheduler_t>()));
}

int
main(int argc, char **argv) {
    bool use_epoll = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--epoll") == 0) {
            use_epoll = true;
        }
    }

    if (use_epoll) {
        std::println("Using epoll backend");
        using accept_sched_t = scheduler::net::epoll::accept_scheduler<
            pump::scheduler::net::senders::conn::op>;
        using session_sched_t = scheduler::net::epoll::session_scheduler<
            scheduler::net::senders::join::op,
            scheduler::net::senders::recv::op,
            scheduler::net::senders::send::op,
            scheduler::net::senders::stop::op
        >;
        run_server<accept_sched_t, session_sched_t>();
    } else {
        std::println("Using io_uring backend");
        using accept_sched_t = scheduler::net::io_uring::accept_scheduler<
            pump::scheduler::net::senders::conn::op>;
        using session_sched_t = scheduler::net::io_uring::session_scheduler<
            scheduler::net::senders::join::op,
            scheduler::net::senders::recv::op,
            scheduler::net::senders::send::op,
            scheduler::net::senders::stop::op
        >;
        run_server<accept_sched_t, session_sched_t>();
    }

    return 0;
}
