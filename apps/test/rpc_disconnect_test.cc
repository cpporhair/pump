
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

// Global task_scheduler for delay handler
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

// Slow service: 10s delay
template<>
struct rpc::service<rpc_method::slow> {
    static constexpr bool is_service = true;
    static auto handle(rpc::handler_context&& ctx) {
        rpc::raw_codec codec;
        auto val = codec.decode_payload<uint64_t>(ctx.payload());
        return g_server_task_sched->delay(10000)
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
            std::println("[Server] Session joined");
            return rpc::serve<rpc_method::echo, rpc_method::slow>(ch);
        })
        >> any_exception([](std::exception_ptr e) {
            try { std::rethrow_exception(e); }
            catch (const std::exception& ex) {
                std::println("[Server] serve ended: {}", ex.what());
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

    struct test_state {
        std::atomic<uint32_t> conn_lost_count{0};
    };
    auto state = std::make_shared<test_state>();

    // Background serve loop (receives responses)
    rpc::serve<>(ch) >> submit(core::make_root_context());

    return scheduler::net::join(session_sched, sid)
        // Test 0: verify echo works
        >> flat_map([ch](...) {
            std::println("[Test 0] verify echo works");
            return rpc::call(ch, rpc_method::echo, uint64_t(42));
        })
        >> then([](rpc::payload_view pv) {
            rpc::raw_codec codec;
            auto val = codec.decode_payload<uint64_t>(pv);
            if (val != 42) {
                std::println(stderr, "FAIL: echo returned {}", val);
                _exit(1);
            }
            std::println("[Test 0] PASS: echo works");
        })

        // Test 1: pending calls get connection_lost_error on disconnect
        >> then([ch, state]() {
            std::println("[Test 1] Send 5 slow calls, then trigger disconnect");
            constexpr uint32_t N = 5;

            for (uint32_t i = 0; i < N; ++i) {
                uint64_t val = i;
                rpc::call(ch, rpc_method::slow, val)
                    >> then([](rpc::payload_view) {})
                    >> any_exception([state](std::exception_ptr e) {
                        try { std::rethrow_exception(e); }
                        catch (const rpc::connection_lost_error&) {
                            state->conn_lost_count.fetch_add(1);
                        }
                        catch (const std::exception& ex) {
                            std::println(stderr, "  unexpected: {}", ex.what());
                        }
                        return just();
                    })
                    >> submit(core::make_root_context());
            }

            // Trigger connection lost — fail_all fires synchronously
            ch->on_connection_lost(
                std::make_exception_ptr(rpc::connection_lost_error()));

            if (state->conn_lost_count.load() != N) {
                std::println(stderr, "FAIL: expected {} connection_lost_error, got {}",
                             N, state->conn_lost_count.load());
                _exit(1);
            }
            std::println("[Test 1] PASS: all pending got connection_lost_error");
        })

        // Test 2: channel status is closed
        >> then([ch]() {
            if (ch->status != rpc::channel_status::closed) {
                std::println(stderr, "FAIL: channel not closed");
                _exit(1);
            }
            std::println("[Test 2] PASS: channel status = closed");
        })

        // Test 3: pending_map is clean
        >> then([ch]() {
            if (ch->pending.active_count != 0) {
                std::println(stderr, "FAIL: pending not clean ({})",
                             ch->pending.active_count);
                _exit(1);
            }
            std::println("[Test 3] PASS: pending_map clean");
        })

        // Test 4: call after close gets channel_closed_error
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
            std::println("\nAll disconnect tests PASSED!");
            _exit(0);
        });
}

// --- Setup ---

auto
create_rs() {
    auto* rs = new rs_t();
    uint32_t num_cores = std::thread::hardware_concurrency();

    auto* accept_sched = new accept_sched_t();
    if (accept_sched->init("0.0.0.0", 19092, 256) < 0) {
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
    std::println("=== RPC Disconnect Test ===");

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
        "127.0.0.1", uint16_t(19092))
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
