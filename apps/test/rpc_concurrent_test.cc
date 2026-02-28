
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
#include "pump/sender/for_each.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/sequential.hh"
#include "pump/sender/when_all.hh"
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

// Global task_scheduler for delay handler
static task_scheduler_t* g_server_task_sched = nullptr;

// Method IDs
enum rpc_method : uint16_t {
    echo = 1,
    delayed_echo = 2,
};

// Echo service: returns the value immediately
template<>
struct rpc::service<rpc_method::echo> {
    static constexpr bool is_service = true;
    static auto handle(rpc::handler_context&& ctx) {
        rpc::raw_codec codec;
        auto val = codec.decode_payload<uint64_t>(ctx.payload());
        return just(std::move(val));
    }
};

// Delayed echo service: delays by (val % 50) ms before returning
template<>
struct rpc::service<rpc_method::delayed_echo> {
    static constexpr bool is_service = true;
    static auto handle(rpc::handler_context&& ctx) {
        rpc::raw_codec codec;
        auto val = codec.decode_payload<uint64_t>(ctx.payload());
        uint64_t delay_ms = (val % 50) + 1; // 1-50ms delay based on value
        return g_server_task_sched->delay(delay_ms)
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

// Custom advance loop (comma-fold, no short-circuit)
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
            return rpc::serve<rpc_method::echo, rpc_method::delayed_echo>(ch);
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

    // Background serve loop
    rpc::serve<>(ch) >> submit(core::make_root_context());

    // Shared test state
    struct test_state {
        std::atomic<uint32_t> completed{0};
        std::atomic<bool> all_correct{true};
    };
    auto state = std::make_shared<test_state>();

    return scheduler::net::join(session_sched, sid)
        // Test 1: N concurrent calls via fire-and-forget, verify all return correctly
        >> flat_map([ch, state, task_sched](...) {
            std::println("[Test 1] 100 concurrent echo calls");
            constexpr uint32_t N = 100;

            for (uint32_t i = 0; i < N; ++i) {
                uint64_t val = i + 1000;
                rpc::call(ch, rpc_method::echo, val)
                    >> then([val, state](rpc::payload_view pv) {
                        rpc::raw_codec codec;
                        auto result = codec.decode_payload<uint64_t>(pv);
                        if (result != val) {
                            state->all_correct.store(false);
                        }
                        state->completed.fetch_add(1);
                    })
                    >> any_exception([state](std::exception_ptr) {
                        state->all_correct.store(false);
                        state->completed.fetch_add(1);
                        return just();
                    })
                    >> submit(core::make_root_context());
            }

            // Wait for all to complete (poll every 50ms)
            return task_sched->delay(2000);
        })
        >> then([state]() {
            std::println("[Test 1] completed={}/100", state->completed.load());
            if (state->completed.load() != 100) {
                std::println(stderr, "FAIL: not all calls completed");
                _exit(1);
            }
            if (!state->all_correct.load()) {
                std::println(stderr, "FAIL: some calls returned wrong values");
                _exit(1);
            }
            std::println("[Test 1] PASS: all 100 concurrent calls correct");
        })

        // Test 2: Out-of-order responses (delayed_echo with varying delays)
        >> flat_map([ch, task_sched](...) {
            std::println("[Test 2] 20 delayed_echo calls (out-of-order responses)");

            struct ooo_state {
                std::atomic<uint32_t> completed{0};
                std::atomic<bool> all_correct{true};
            };
            auto ooo = std::make_shared<ooo_state>();
            constexpr uint32_t N = 20;

            for (uint32_t i = 0; i < N; ++i) {
                uint64_t val = i;
                rpc::call(ch, rpc_method::delayed_echo, val)
                    >> then([val, ooo](rpc::payload_view pv) {
                        rpc::raw_codec codec;
                        auto result = codec.decode_payload<uint64_t>(pv);
                        if (result != val) {
                            std::println(stderr, "FAIL: delayed_echo mismatch: "
                                         "expected={} got={}", val, result);
                            ooo->all_correct.store(false);
                        }
                        ooo->completed.fetch_add(1);
                    })
                    >> any_exception([ooo](std::exception_ptr) {
                        ooo->all_correct.store(false);
                        ooo->completed.fetch_add(1);
                        return just();
                    })
                    >> submit(core::make_root_context());
            }

            // Wait enough time for all delayed responses
            return task_sched->delay(3000)
                >> then([ooo]() {
                    std::println("[Test 2] completed={}/20", ooo->completed.load());
                    if (ooo->completed.load() != N) {
                        std::println(stderr, "FAIL: not all delayed calls completed");
                        _exit(1);
                    }
                    if (!ooo->all_correct.load()) {
                        std::println(stderr, "FAIL: request_id mismatch in delayed calls");
                        _exit(1);
                    }
                    std::println("[Test 2] PASS: all delayed calls matched correctly");
                });
        })

        // Test 3: pending_map should be clean after all calls
        >> then([ch]() {
            if (ch->pending.active_count != 0) {
                std::println(stderr, "FAIL: pending not clean ({})",
                             ch->pending.active_count);
                _exit(1);
            }
            std::println("[Test 3] PASS: pending_map clean");
        })

        // Test 4: pending_map overflow
        >> flat_map([ch, task_sched](...) {
            std::println("[Test 4] pending_map overflow test");

            // Fill up pending_map to MaxPending (1024)
            // We send calls but the server will respond — we just need
            // to check the overflow error when we exceed the limit
            struct overflow_state {
                std::atomic<uint32_t> completed{0};
                std::atomic<bool> overflow_seen{false};
            };
            auto ofs = std::make_shared<overflow_state>();

            // Send 1025 calls rapidly (MaxPending = 1024)
            constexpr uint32_t OVER = 1025;
            for (uint32_t i = 0; i < OVER; ++i) {
                uint64_t val = i;
                rpc::call(ch, rpc_method::delayed_echo, val)
                    >> then([ofs](rpc::payload_view) {
                        ofs->completed.fetch_add(1);
                    })
                    >> any_exception([ofs](std::exception_ptr e) {
                        try { std::rethrow_exception(e); }
                        catch (const rpc::pending_overflow_error&) {
                            ofs->overflow_seen.store(true);
                        }
                        catch (...) {}
                        ofs->completed.fetch_add(1);
                        return just();
                    })
                    >> submit(core::make_root_context());
            }

            return task_sched->delay(5000)
                >> then([ofs]() {
                    std::println("[Test 4] completed={}/1025, overflow_seen={}",
                                 ofs->completed.load(),
                                 ofs->overflow_seen.load());
                    if (!ofs->overflow_seen.load()) {
                        std::println(stderr, "FAIL: no pending_overflow_error seen");
                        _exit(1);
                    }
                    std::println("[Test 4] PASS: pending_overflow_error received");
                });
        })

        >> then([]() {
            std::println("\nAll concurrent tests PASSED!");
            _exit(0);
        });
}

// --- Setup ---

auto
create_rs() {
    auto* rs = new rs_t();
    uint32_t num_cores = std::thread::hardware_concurrency();

    auto* accept_sched = new accept_sched_t();
    if (accept_sched->init("0.0.0.0", 19091, 256) < 0) {
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
    std::println("=== RPC Concurrent Test ===");

    auto* rs = create_rs();

    // Server accept loop
    just()
        >> forever()
        >> flat_map([rs](...) {
            return scheduler::net::wait_connection(
                rs->template get_schedulers<accept_sched_t>()[0]);
        })
        >> then([rs](scheduler::net::common::session_id_t s) {
            server_session(rs, s) >> submit(core::make_root_context());
        })
        >> count()
        >> submit(core::make_root_context());

    // Client connect
    scheduler::net::connect(
        rs->template get_schedulers<connect_sched_t>()[0],
        "127.0.0.1", uint16_t(19091))
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

    run_all_cores(rs);
}
