
#include <print>
#include <thread>
#include <cstring>
#include <atomic>
#include <memory>
#include "pump/sender/flat.hh"
#include "pump/sender/just.hh"
#include "pump/sender/repeat.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/on.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/submit.hh"
#include "pump/core/context.hh"
#include "env/runtime/runner.hh"
#include "env/scheduler/net/net.hh"
#include "env/scheduler/net/io_uring/scheduler.hh"
#include "env/scheduler/net/io_uring/connect_scheduler.hh"
#include "env/scheduler/task/tasks_scheduler.hh"

#include "env/scheduler/rpc/rpc.hh"

using namespace pump;
using namespace pump::sender;

using task_scheduler_t = scheduler::task::scheduler;

// Global task_scheduler for slow handler
static task_scheduler_t* g_server_task_sched = nullptr;

// Method IDs
enum rpc_method : uint16_t {
    echo = 1,
    slow = 2,
};

// Echo service
template<>
struct rpc::service<rpc_method::echo> {
    static constexpr bool is_service = true;
    static auto handle(rpc::handler_context&& ctx) {
        rpc::raw_codec codec;
        auto val = codec.decode_payload<uint64_t>(ctx.payload());
        return just(std::move(val));
    }
};

// Slow service: delays 5s before returning
template<>
struct rpc::service<rpc_method::slow> {
    static constexpr bool is_service = true;
    static auto handle(rpc::handler_context&& ctx) {
        rpc::raw_codec codec;
        auto val = codec.decode_payload<uint64_t>(ctx.payload());
        return g_server_task_sched->delay(5000)
            >> then([val]() { return val; });
    }
};

// --- Scheduler types ---

using accept_sched_t = scheduler::net::io_uring::accept_scheduler<
    pump::scheduler::net::senders::conn::op>;
using connect_sched_t = scheduler::net::io_uring::connect_scheduler<
    pump::scheduler::net::senders::conn::op>;
using session_sched_t = scheduler::net::io_uring::session_scheduler<
    scheduler::net::senders::join::op,
    scheduler::net::senders::recv::op,
    scheduler::net::senders::send::op,
    scheduler::net::senders::stop::op
>;

using rs_t = env::runtime::runtime_schedulers<
    task_scheduler_t, accept_sched_t, connect_sched_t, session_sched_t>;

// Custom advance loop: uses comma-fold to advance ALL schedulers
// (avoids ||  short-circuit in env::runtime::run that skips schedulers
//  when io_uring advance() always returns true)
static std::atomic<bool> g_running{false};

void
run_all_cores(rs_t* rs) {
    g_running.store(true);
    uint32_t num_cores = rs->schedulers_by_core.size();
    uint32_t cur_core = sched_getcpu() % num_cores;

    for (uint32_t i = 0; i < num_cores; ++i) {
        if (i == cur_core) continue;
        std::thread([rs, i]() {
            std::apply([](auto*... sche) {
                while (g_running.load(std::memory_order_relaxed)) {
                    bool worked = false;
                    auto adv = [&](auto* s) { if (s) worked |= s->advance(); };
                    (adv(sche), ...);
                    if (!worked) std::this_thread::yield();
                }
            }, rs->schedulers_by_core[i]);
        }).detach();
    }

    // Current core runs inline (blocking)
    std::apply([](auto*... sche) {
        while (g_running.load(std::memory_order_relaxed)) {
            bool worked = false;
            auto adv = [&](auto* s) { if (s) worked |= s->advance(); };
            (adv(sche), ...);
            if (!worked) std::this_thread::yield();
        }
    }, rs->schedulers_by_core[cur_core]);
}

// --- Server session ---

auto
server_session(rs_t* rs, scheduler::net::common::session_id_t sid) {
    auto core_idx = sid.raw() % rs->template get_schedulers<session_sched_t>().size();
    auto* session_sched = rs->template get_schedulers<session_sched_t>()[core_idx];
    auto* task_sched = rs->template get_schedulers<task_scheduler_t>()[core_idx];
    g_server_task_sched = task_sched;

    auto ch = rpc::make_channel<rpc::raw_codec>(sid, session_sched, task_sched);

    return scheduler::net::join(session_sched, sid)
        >> flat_map([ch](...) {
            std::println("[Server] Session joined");
            return rpc::serve<rpc_method::echo, rpc_method::slow>(ch);
        })
        >> any_exception([](std::exception_ptr e) {
            try { std::rethrow_exception(e); }
            catch (const std::exception& ex) {
                std::println(stderr, "[Server] Error: {}", ex.what());
            }
            return just();
        });
}

// --- Client session ---

auto
client_session(rs_t* rs, scheduler::net::common::session_id_t sid) {
    auto core_idx = sid.raw() % rs->template get_schedulers<session_sched_t>().size();
    auto* session_sched = rs->template get_schedulers<session_sched_t>()[core_idx];
    auto* task_sched = rs->template get_schedulers<task_scheduler_t>()[core_idx];

    auto ch = rpc::make_channel<rpc::raw_codec>(sid, session_sched, task_sched);

    // Background serve loop (receives responses)
    rpc::serve<>(ch) >> submit(core::make_root_context());

    struct test_state {
        std::atomic<bool> timeout_received{false};
    };
    auto state = std::make_shared<test_state>();

    return scheduler::net::join(session_sched, sid)
        >> flat_map([ch, state, task_sched](...) {
            std::println("[Client] Connected, starting timeout test");

            // Fire-and-forget: slow call with 200ms timeout
            std::println("[Test 1] slow call, timeout=200ms");
            rpc::call(ch, rpc_method::slow, uint64_t(42), 200)
                >> then([](rpc::payload_view) {
                    std::println(stderr, "FAIL: slow call should have timed out");
                    _exit(1);
                })
                >> any_exception([state](std::exception_ptr e) {
                    try { std::rethrow_exception(e); }
                    catch (const rpc::rpc_timeout_error&) {
                        std::println("[Test 1] Got rpc_timeout_error");
                        state->timeout_received.store(true);
                    }
                    catch (const std::exception& ex) {
                        std::println(stderr, "FAIL: unexpected: {}", ex.what());
                        _exit(1);
                    }
                    return just();
                })
                >> submit(core::make_root_context());

            // Wait 500ms past the 200ms timeout, then send echo to trigger
            // process_buffer → check_timeouts
            return task_sched->delay(500);
        })
        >> flat_map([ch](...) {
            std::println("[Test 2] echo call to trigger timeout check");
            return rpc::call(ch, rpc_method::echo, uint64_t(99));
        })
        >> then([](rpc::payload_view pv) {
            rpc::raw_codec codec;
            auto result = codec.decode_payload<uint64_t>(pv);
            std::println("[Test 2] echo={} (expected 99)", result);
            if (result != 99) {
                std::println(stderr, "FAIL: wrong echo value");
                _exit(1);
            }
        })
        >> flat_map([task_sched](...) {
            return task_sched->delay(100);
        })
        >> then([state, ch]() {
            if (!state->timeout_received.load()) {
                std::println(stderr, "FAIL: timeout not received");
                _exit(1);
            }
            std::println("[Test 1] PASS: timeout received");

            if (ch->pending.active_count != 0) {
                std::println(stderr, "FAIL: pending not clean ({})",
                             ch->pending.active_count);
                _exit(1);
            }
            std::println("[Test 3] PASS: pending_map clean");

            ch->close();
        })
        >> flat_map([ch](...) {
            std::println("[Test 4] call after close");
            return rpc::call(ch, rpc_method::echo, uint64_t(0));
        })
        >> then([](rpc::payload_view) {
            std::println(stderr, "FAIL: call after close should fail");
            _exit(1);
        })
        >> any_exception([](std::exception_ptr e) {
            try { std::rethrow_exception(e); }
            catch (const rpc::channel_closed_error&) {
                std::println("[Test 4] PASS: channel_closed_error");
            }
            catch (const std::exception& ex) {
                std::println(stderr, "FAIL: unexpected: {}", ex.what());
                _exit(1);
            }
            return just();
        })
        >> then([]() {
            std::println("\nAll timeout tests PASSED!");
            _exit(0);
        });
}

// --- Setup ---

auto
create_rs() {
    auto* rs = new rs_t();
    uint32_t num_cores = std::thread::hardware_concurrency();

    auto* accept_sched = new accept_sched_t();
    if (accept_sched->init("0.0.0.0", 19090, 256) < 0) {
        std::println(stderr, "Failed to init accept_scheduler");
        std::exit(1);
    }

    auto* connect_sched = new connect_sched_t();
    if (connect_sched->init(scheduler::net::common::scheduler_config{}) < 0) {
        std::println(stderr, "Failed to init connect_scheduler");
        std::exit(1);
    }

    std::vector<session_sched_t*> session_scheds(num_cores);
    for (uint32_t i = 0; i < num_cores; ++i) {
        session_scheds[i] = new session_sched_t();
        if (session_scheds[i]->init(256) < 0) {
            std::println(stderr, "Failed to init session_scheduler {}", i);
            std::exit(1);
        }
    }

    for (uint32_t i = 0; i < num_cores; ++i) {
        rs->add_core_schedulers(
            new task_scheduler_t(i),
            i == 0 ? accept_sched : nullptr,
            i == 0 ? connect_sched : nullptr,
            session_scheds[i]
        );
    }
    return rs;
}

int
main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::println("=== RPC Timeout Test ===");

    auto* rs = create_rs();

    // Setup server accept loop
    just()
        >> forever()
        >> flat_map([rs](...) {
            return scheduler::net::wait_connection(
                rs->template get_schedulers<accept_sched_t>()[0]);
        })
        >> then([rs](scheduler::net::common::session_id_t s) {
            std::println("[Server] accept sid={}", s.raw());
            server_session(rs, s) >> submit(core::make_root_context());
        })
        >> count()
        >> submit(core::make_root_context());

    // Setup client connect
    scheduler::net::connect(
        rs->template get_schedulers<connect_sched_t>()[0],
        "127.0.0.1", uint16_t(19090))
        >> then([rs](scheduler::net::common::session_id_t sid) {
            std::println("[Client] connected sid={}", sid.raw());
            client_session(rs, sid) >> submit(core::make_root_context());
        })
        >> any_exception([](std::exception_ptr e) {
            try { std::rethrow_exception(e); }
            catch (const std::exception& ex) {
                std::println(stderr, "Connect error: {}", ex.what());
            }
            _exit(1);
            return just();
        })
        >> submit(core::make_root_context());

    // Start advance loops (blocks on current core)
    run_all_cores(rs);
}
