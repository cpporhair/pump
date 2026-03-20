#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <cassert>
#include <atomic>
#include <thread>
#include <chrono>
#include <variant>
#include <ranges>

#include "pump/core/context.hh"
#include "pump/core/meta.hh"
#include "pump/core/scope.hh"

#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/for_each.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/on.hh"
#include "pump/sender/when_all.hh"
#include "pump/sender/when_any.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/visit.hh"
#include "pump/sender/push_context.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/pop_context.hh"

#include "env/scheduler/task/tasks_scheduler.hh"

using namespace pump::sender;
using namespace pump::core;

// ============================================================================
// Test infrastructure
// ============================================================================
static int test_count = 0;
static int pass_count = 0;
static int fail_count = 0;

#define TEST(name) \
    do { \
        test_count++; \
        std::cout << "  TEST: " << name << " ... " << std::flush; \
    } while(0)

#define PASS() \
    do { \
        pass_count++; \
        std::cout << "PASS" << std::endl; \
    } while(0)

#define FAIL(msg) \
    do { \
        fail_count++; \
        std::cout << "FAIL: " << msg << std::endl; \
    } while(0)

// ============================================================================
// Multi-core infrastructure
// ============================================================================
static constexpr uint32_t NUM_CORES = 12;
static std::vector<pump::scheduler::task::scheduler*> g_schedulers;
static std::vector<std::thread> g_worker_threads;
static std::atomic<bool> g_workers_running{false};

inline uint64_t
fast_random() {
    static thread_local uint64_t state = []() {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&state) ^
               static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    }();
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545F4914F6CDD1DULL;
}

inline pump::scheduler::task::scheduler*
random_scheduler() {
    return g_schedulers[fast_random() % g_schedulers.size()];
}

void start_workers() {
    g_workers_running.store(true);
    for (uint32_t i = 0; i < NUM_CORES; ++i) {
        g_schedulers.push_back(new pump::scheduler::task::scheduler(i));
    }
    for (uint32_t i = 0; i < NUM_CORES; ++i) {
        g_worker_threads.emplace_back([i]() {
            pump::core::this_core_id = i;
            auto* sche = g_schedulers[i];
            while (g_workers_running.load(std::memory_order_relaxed)) {
                if (!sche->advance()) {
                    std::this_thread::yield();
                }
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

void stop_workers() {
    g_workers_running.store(false);
    for (auto& t : g_worker_threads) {
        if (t.joinable()) t.join();
    }
    for (auto* s : g_schedulers) delete s;
    g_schedulers.clear();
    g_worker_threads.clear();
}

bool wait_for(std::atomic<bool>& flag, int timeout_ms = 10000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// ============================================================================
// Test 1: Massive flat_map across cores
// ============================================================================
void test_massive_flat_map() {
    TEST("massive flat_map (20000 items, concurrency 1000, cross-core)");

    std::atomic<bool> done{false};
    std::atomic<uint64_t> counter{0};
    auto ctx = make_root_context();

    just()
        >> for_each(std::views::iota(0u, 20000u))
        >> concurrent(1000)
        >> on(random_scheduler()->as_task())
        >> flat_map([](auto&&...) {
            // nested flat_map → schedule to another random core
            return just(1u)
                >> on(random_scheduler()->as_task())
                >> then([](uint32_t v) { return v; });
        })
        >> then([&counter](uint32_t) {
            counter.fetch_add(1, std::memory_order_relaxed);
        })
        >> reduce()
        >> then([&done](...) {
            done.store(true, std::memory_order_release);
        })
        >> submit(ctx);

    if (wait_for(done) && counter.load() == 20000) PASS();
    else FAIL("counter=" + std::to_string(counter.load()));
}

// ============================================================================
// Test 2: Exception storm — half throw, recover with any_exception
// ============================================================================
void test_exception_storm() {
    TEST("exception storm (10000 items, 50% throw, any_exception recover)");

    std::atomic<bool> done{false};
    std::atomic<uint64_t> normal_count{0};
    std::atomic<uint64_t> recovered_count{0};
    auto ctx = make_root_context();

    just()
        >> for_each(std::views::iota(0u, 10000u))
        >> concurrent(500)
        >> on(random_scheduler()->as_task())
        >> then([](uint32_t i) -> uint32_t {
            if (i % 2 == 0) throw std::runtime_error("even");
            return i;
        })
        >> then([&normal_count](uint32_t i) {
            normal_count.fetch_add(1, std::memory_order_relaxed);
            return i;
        })
        >> any_exception([&recovered_count](std::exception_ptr) {
            recovered_count.fetch_add(1, std::memory_order_relaxed);
            return just(uint32_t(0));
        })
        >> reduce()
        >> then([&done](...) {
            done.store(true, std::memory_order_release);
        })
        >> submit(ctx);

    if (wait_for(done) && normal_count.load() == 5000 && recovered_count.load() == 5000) PASS();
    else FAIL("normal=" + std::to_string(normal_count.load()) + " recovered=" + std::to_string(recovered_count.load()));
}

// ============================================================================
// Test 3: when_all stress — concurrent when_all with 3 branches each
// ============================================================================
void test_when_all_stress() {
    TEST("when_all stress (5000 items, concurrent, 3 branches each)");

    std::atomic<bool> done{false};
    std::atomic<uint64_t> counter{0};
    auto ctx = make_root_context();

    just()
        >> for_each(std::views::iota(0u, 5000u))
        >> concurrent(200)
        >> on(random_scheduler()->as_task())
        >> flat_map([](auto&&...) {
            return just()
                >> when_all(
                    just(1) >> on(random_scheduler()->as_task()) >> then([](int v) { return v; }),
                    just(2) >> on(random_scheduler()->as_task()) >> then([](int v) { return v; }),
                    just(3) >> on(random_scheduler()->as_task()) >> then([](int v) { return v; })
                );
        })
        >> then([&counter](auto&&...) {
            counter.fetch_add(1, std::memory_order_relaxed);
        })
        >> reduce()
        >> then([&done](...) {
            done.store(true, std::memory_order_release);
        })
        >> submit(ctx);

    if (wait_for(done) && counter.load() == 5000) PASS();
    else FAIL("counter=" + std::to_string(counter.load()));
}

// ============================================================================
// Test 4: when_any stress — race between branches
// ============================================================================
void test_when_any_stress() {
    TEST("when_any stress (5000 items, concurrent, 2 racing branches)");

    std::atomic<bool> done{false};
    std::atomic<uint64_t> counter{0};
    auto ctx = make_root_context();

    just()
        >> for_each(std::views::iota(0u, 5000u))
        >> concurrent(200)
        >> on(random_scheduler()->as_task())
        >> flat_map([](auto&&...) {
            return just()
                >> when_any(
                    just(42) >> on(random_scheduler()->as_task()) >> then([](int v) { return v; }),
                    just(99) >> on(random_scheduler()->as_task()) >> then([](int v) { return v; })
                );
        })
        >> then([&counter](uint32_t, auto&&) {
            counter.fetch_add(1, std::memory_order_relaxed);
        })
        >> reduce()
        >> then([&done](...) {
            done.store(true, std::memory_order_release);
        })
        >> submit(ctx);

    if (wait_for(done) && counter.load() == 5000) PASS();
    else FAIL("counter=" + std::to_string(counter.load()));
}

// ============================================================================
// Test 5: Nested concurrency — outer × inner fan-out
// ============================================================================
void test_nested_concurrency() {
    TEST("nested concurrency (200 outer × 100 inner, 2-level fan-out)");

    std::atomic<bool> done{false};
    std::atomic<uint64_t> counter{0};
    auto ctx = make_root_context();

    just()
        >> for_each(std::views::iota(0u, 200u))
        >> concurrent(50)
        >> on(random_scheduler()->as_task())
        >> flat_map([&counter](auto&&...) {
            return just()
                >> for_each(std::views::iota(0u, 100u))
                >> concurrent(50)
                >> on(random_scheduler()->as_task())
                >> then([&counter](uint32_t) {
                    counter.fetch_add(1, std::memory_order_relaxed);
                })
                >> reduce();
        })
        >> reduce()
        >> then([&done](...) {
            done.store(true, std::memory_order_release);
        })
        >> submit(ctx);

    if (wait_for(done, 30000) && counter.load() == 20000) PASS();
    else FAIL("counter=" + std::to_string(counter.load()));
}

// ============================================================================
// Test 6: visit + variant branching with flat
// ============================================================================
void test_visit_branching() {
    TEST("visit + variant branching (10000 items, type dispatch + flat)");

    std::atomic<bool> done{false};
    std::atomic<uint64_t> true_count{0};
    std::atomic<uint64_t> false_count{0};
    auto ctx = make_root_context();

    just()
        >> for_each(std::views::iota(0u, 10000u))
        >> concurrent(500)
        >> on(random_scheduler()->as_task())
        >> then([](uint32_t i) { return i % 3 == 0; })
        >> visit()
        >> then([&true_count, &false_count](auto&& flag) {
            if constexpr (std::is_same_v<__typ__(flag), std::true_type>) {
                true_count.fetch_add(1, std::memory_order_relaxed);
                return just(uint32_t(1));
            } else {
                false_count.fetch_add(1, std::memory_order_relaxed);
                return just(uint32_t(0));
            }
        })
        >> flat()
        >> reduce()
        >> then([&done](...) {
            done.store(true, std::memory_order_release);
        })
        >> submit(ctx);

    if (wait_for(done) && true_count.load() == 3334 && false_count.load() == 6666) PASS();
    else FAIL("true=" + std::to_string(true_count.load()) + " false=" + std::to_string(false_count.load()));
}

// ============================================================================
// Test 7: Context push/pop under concurrency
// ============================================================================
struct test_ctx_data {
    uint64_t magic = 0xDEADBEEF;
};

void test_context_concurrent() {
    TEST("context push/pop under concurrency (10000 items)");

    std::atomic<bool> done{false};
    std::atomic<uint64_t> ok_count{0};
    auto ctx = make_root_context();

    just()
        >> for_each(std::views::iota(0u, 10000u))
        >> concurrent(500)
        >> on(random_scheduler()->as_task())
        >> push_context(test_ctx_data{0xDEADBEEF})
        >> get_context<test_ctx_data>()
        >> then([&ok_count](test_ctx_data& d, auto&&...) {
            if (d.magic == 0xDEADBEEF)
                ok_count.fetch_add(1, std::memory_order_relaxed);
        })
        >> pop_context()
        >> reduce()
        >> then([&done](...) {
            done.store(true, std::memory_order_release);
        })
        >> submit(ctx);

    if (wait_for(done) && ok_count.load() == 10000) PASS();
    else FAIL("ok_count=" + std::to_string(ok_count.load()));
}

// ============================================================================
// Test 8: Exception + flat + concurrent combo
// ============================================================================
void test_exception_flat_concurrent_combo() {
    TEST("exception + flat + concurrent combo (10000 items)");

    std::atomic<bool> done{false};
    std::atomic<uint64_t> success_count{0};
    std::atomic<uint64_t> error_count{0};
    auto ctx = make_root_context();

    just()
        >> for_each(std::views::iota(0u, 10000u))
        >> concurrent(500)
        >> on(random_scheduler()->as_task())
        >> then([](uint32_t i) -> uint32_t {
            if (i % 2 == 0) throw std::runtime_error("boom");
            return i;
        })
        >> then([&success_count](uint32_t i) {
            success_count.fetch_add(1, std::memory_order_relaxed);
            return i;
        })
        >> any_exception([&error_count](std::exception_ptr) {
            error_count.fetch_add(1, std::memory_order_relaxed);
            return just(uint32_t(0));
        })
        >> reduce()
        >> then([&done](...) {
            done.store(true, std::memory_order_release);
        })
        >> submit(ctx);

    if (wait_for(done) && success_count.load() == 5000 && error_count.load() == 5000) PASS();
    else FAIL("success=" + std::to_string(success_count.load()) + " error=" + std::to_string(error_count.load()));
}

// ============================================================================
// Test 9: Deep flat chain — 5 levels of flat_map nesting
// ============================================================================
void test_deep_flat_chain() {
    TEST("deep flat chain (5000 items, 5-level nested flat_map)");

    std::atomic<bool> done{false};
    std::atomic<uint64_t> counter{0};
    auto ctx = make_root_context();

    just()
        >> for_each(std::views::iota(0u, 5000u))
        >> concurrent(200)
        >> on(random_scheduler()->as_task())
        >> flat_map([](auto&&...) {
            return just(1u)
                >> flat_map([](uint32_t i) {
                    return just(__mov__(i))
                        >> flat_map([](uint32_t i) {
                            return just(__mov__(i))
                                >> flat_map([](uint32_t i) {
                                    return just(__mov__(i))
                                        >> flat_map([](uint32_t i) {
                                            return just(i + 1);
                                        });
                                });
                        });
                });
        })
        >> then([&counter](uint32_t) {
            counter.fetch_add(1, std::memory_order_relaxed);
        })
        >> reduce()
        >> then([&done](...) {
            done.store(true, std::memory_order_release);
        })
        >> submit(ctx);

    if (wait_for(done) && counter.load() == 5000) PASS();
    else FAIL("counter=" + std::to_string(counter.load()));
}

// ============================================================================
// Test 10: when_all + exception — some branches throw
// ============================================================================
void test_when_all_with_exceptions() {
    TEST("when_all + exceptions (3000 items, branch 2 always throws)");

    std::atomic<bool> done{false};
    std::atomic<uint64_t> counter{0};
    auto ctx = make_root_context();

    just()
        >> for_each(std::views::iota(0u, 3000u))
        >> concurrent(200)
        >> on(random_scheduler()->as_task())
        >> flat_map([](auto&&...) {
            return just()
                >> when_all(
                    just(1) >> on(random_scheduler()->as_task()) >> then([](int v) { return v; }),
                    just(2) >> on(random_scheduler()->as_task()) >> then([](int) -> int { throw std::runtime_error("branch2"); }),
                    just(3) >> on(random_scheduler()->as_task()) >> then([](int v) { return v; })
                );
        })
        >> then([&counter](auto&&...) {
            counter.fetch_add(1, std::memory_order_relaxed);
        })
        >> reduce()
        >> then([&done](...) {
            done.store(true, std::memory_order_release);
        })
        >> submit(ctx);

    if (wait_for(done) && counter.load() == 3000) PASS();
    else FAIL("counter=" + std::to_string(counter.load()));
}

// ============================================================================
// Test 11: Mixed when_any + flat_map
// ============================================================================
void test_mixed_combinators() {
    TEST("mixed when_any + flat_map (3000 items)");

    std::atomic<bool> done{false};
    std::atomic<uint64_t> counter{0};
    auto ctx = make_root_context();

    just()
        >> for_each(std::views::iota(0u, 3000u))
        >> concurrent(100)
        >> on(random_scheduler()->as_task())
        >> flat_map([](auto&&...) {
            return just()
                >> when_any(
                    just(10) >> on(random_scheduler()->as_task()) >> then([](int v) { return v; }),
                    just(20) >> on(random_scheduler()->as_task()) >> then([](int v) { return v; })
                )
                >> then([](uint32_t, auto&&) {
                    return uint32_t(1);
                });
        })
        >> then([&counter](uint32_t) {
            counter.fetch_add(1, std::memory_order_relaxed);
        })
        >> reduce()
        >> then([&done](...) {
            done.store(true, std::memory_order_release);
        })
        >> submit(ctx);

    if (wait_for(done) && counter.load() == 3000) PASS();
    else FAIL("counter=" + std::to_string(counter.load()));
}

// ============================================================================
// Test 12: ignore_inner_exception stress
// ============================================================================
void test_ignore_inner_exception() {
    TEST("ignore_inner_exception stress (10000 items, all inner throw)");

    std::atomic<bool> done{false};
    std::atomic<uint64_t> counter{0};
    auto ctx = make_root_context();

    just()
        >> for_each(std::views::iota(0u, 10000u))
        >> concurrent(500)
        >> on(random_scheduler()->as_task())
        >> ignore_inner_exception(
            then([](auto&&...) -> uint32_t {
                throw std::runtime_error("inner");
            })
        )
        >> then([&counter](...) {
            counter.fetch_add(1, std::memory_order_relaxed);
        })
        >> reduce()
        >> then([&done](...) {
            done.store(true, std::memory_order_release);
        })
        >> submit(ctx);

    if (wait_for(done) && counter.load() == 10000) PASS();
    else FAIL("counter=" + std::to_string(counter.load()));
}

// ============================================================================
// Test 13: Rapid fire-and-forget submits (scope leak check)
// ============================================================================
void test_rapid_submit() {
    TEST("rapid fire-and-forget submits (50000 independent pipelines)");

    std::atomic<uint64_t> counter{0};

    for (uint32_t i = 0; i < 50000; ++i) {
        auto ctx = make_root_context();
        just()
            >> on(random_scheduler()->as_task())
            >> then([&counter](...) {
                counter.fetch_add(1, std::memory_order_relaxed);
            })
            >> submit(ctx);
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (counter.load(std::memory_order_relaxed) < 50000) {
        if (std::chrono::steady_clock::now() > deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (counter.load() == 50000) PASS();
    else FAIL("counter=" + std::to_string(counter.load()));
}

// ============================================================================
// Concurrent boundary condition tests
// ============================================================================

// Edge case: concurrent stream with exactly 1 element (sync completion path)
void test_concurrent_single_element() {
    TEST("concurrent single element (sync completion)");

    std::atomic<bool> done{false};
    std::atomic<uint32_t> sum{0};
    auto ctx = make_root_context();

    just()
        >> for_each(std::views::iota(0u, 1u))
        >> concurrent(10)
        >> on(random_scheduler()->as_task())
        >> then([&](uint32_t v) {
            sum.fetch_add(v + 100, std::memory_order_relaxed);
            return true;
        })
        >> reduce()
        >> then([&](bool) {
            done.store(true, std::memory_order_release);
        })
        >> submit(ctx);

    if (wait_for(done) && sum.load() == 100) PASS();
    else FAIL("sum=" + std::to_string(sum.load()) + " done=" + std::to_string(done.load()));
}

// Edge case: concurrent stream with 0 elements (empty stream)
void test_concurrent_empty_stream() {
    TEST("concurrent empty stream");

    std::atomic<bool> done{false};
    std::atomic<uint32_t> count{0};
    auto ctx = make_root_context();

    std::vector<uint32_t> empty_vec;
    just(__mov__(empty_vec))
        >> for_each()
        >> concurrent(10)
        >> on(random_scheduler()->as_task())
        >> then([&](uint32_t v) {
            count.fetch_add(1, std::memory_order_relaxed);
            return true;
        })
        >> reduce()
        >> then([&](bool) {
            done.store(true, std::memory_order_release);
        })
        >> submit(ctx);

    // Empty stream: reduce should complete immediately, no items processed
    if (wait_for(done) && count.load() == 0) PASS();
    else FAIL("count=" + std::to_string(count.load()) + " (expected 0)");
}

// Edge case: concurrent(1) — degrades to serial execution
void test_concurrent_serial_degradation() {
    TEST("concurrent(1) serial degradation");

    std::atomic<bool> done{false};
    std::atomic<uint32_t> max_concurrent{0};
    std::atomic<uint32_t> current_concurrent{0};
    auto ctx = make_root_context();

    just()
        >> for_each(std::views::iota(0u, 100u))
        >> concurrent(1)
        >> on(random_scheduler()->as_task())
        >> then([&](uint32_t v) {
            auto cur = current_concurrent.fetch_add(1, std::memory_order_relaxed) + 1;
            uint32_t prev_max = max_concurrent.load(std::memory_order_relaxed);
            while (cur > prev_max && !max_concurrent.compare_exchange_weak(prev_max, cur));
            current_concurrent.fetch_sub(1, std::memory_order_relaxed);
            return true;
        })
        >> reduce()
        >> then([&](bool) {
            done.store(true, std::memory_order_release);
        })
        >> submit(ctx);

    if (wait_for(done) && max_concurrent.load() <= 2) PASS();
    else FAIL("max_concurrent=" + std::to_string(max_concurrent.load()) + " (expected <=2)");
}

// Edge case: all items complete before set_source_done
// (items dispatched to same core, complete synchronously within start_stream)
void test_concurrent_all_complete_before_source_done() {
    TEST("concurrent all items sync-complete before source_done");

    std::atomic<bool> done{false};
    std::atomic<uint64_t> sum{0};
    auto ctx = make_root_context();

    // Use a small count and same-core scheduler to maximize sync completion chance
    auto* sche = g_schedulers[pump::core::this_core_id % g_schedulers.size()];

    just()
        >> for_each(std::views::iota(0u, 5u))
        >> concurrent()
        >> on(sche->as_task())
        >> then([&](uint32_t v) {
            sum.fetch_add(v, std::memory_order_relaxed);
            return true;
        })
        >> reduce()
        >> then([&](...) {
            done.store(true, std::memory_order_release);
        })
        >> submit(ctx);

    if (wait_for(done) && sum.load() == 10) PASS();
    else FAIL("sum=" + std::to_string(sum.load()) + " (expected 10)");
}

// Edge case: nested concurrent with inner exception
void test_nested_concurrent_inner_exception() {
    TEST("nested concurrent with inner exception recovery");

    std::atomic<bool> done{false};
    std::atomic<uint64_t> recovered{0};
    auto ctx = make_root_context();

    just()
        >> for_each(std::views::iota(0u, 50u))
        >> concurrent(10)
        >> on(random_scheduler()->as_task())
        >> flat_map([&](uint32_t outer) {
            return just()
                >> for_each(std::views::iota(0u, 20u))
                >> concurrent()
                >> on(random_scheduler()->as_task())
                >> then([outer](uint32_t inner) -> bool {
                    if (inner % 5 == 0) throw std::runtime_error("inner fail");
                    return true;
                })
                >> any_exception([](auto) {
                    return just(true);
                })
                >> reduce();
        })
        >> then([&](uint32_t v) {
            recovered.fetch_add(1, std::memory_order_relaxed);
        })
        >> reduce()
        >> then([&](...) {
            done.store(true, std::memory_order_release);
        })
        >> submit(ctx);

    if (wait_for(done) && recovered.load() == 50) PASS();
    else FAIL("recovered=" + std::to_string(recovered.load()) + "/50");
}

// Edge case: concurrent with large fan-out (stress source_done bit packing)
void test_concurrent_large_fanout() {
    TEST("concurrent large fan-out (10000 items, tests packed counter)");

    std::atomic<bool> done{false};
    std::atomic<uint64_t> counter{0};
    auto ctx = make_root_context();

    just()
        >> for_each(std::views::iota(0u, 10000u))
        >> concurrent(500)
        >> on(random_scheduler()->as_task())
        >> then([&](uint32_t v) {
            counter.fetch_add(1, std::memory_order_relaxed);
            return true;
        })
        >> reduce()
        >> then([&](...) {
            done.store(true, std::memory_order_release);
        })
        >> submit(ctx);

    if (wait_for(done) && counter.load() == 10000) PASS();
    else FAIL("counter=" + std::to_string(counter.load()) + "/10000");
}

// Edge case: repeated concurrent streams (tests counter reset)
void test_concurrent_repeated_streams() {
    TEST("repeated concurrent streams (counter reset correctness)");

    std::atomic<bool> done{false};
    std::atomic<uint64_t> total{0};
    auto ctx = make_root_context();

    // Outer loop creates multiple concurrent streams sequentially via flat_map
    just()
        >> for_each(std::views::iota(0u, 10u))
        >> flat_map([&](uint32_t round) {
            return just()
                >> for_each(std::views::iota(0u, 10u))
                >> concurrent(5)
                >> on(random_scheduler()->as_task())
                >> then([&](uint32_t v) {
                    total.fetch_add(1, std::memory_order_relaxed);
                    return true;
                })
                >> reduce();
        })
        >> reduce()
        >> then([&](...) {
            done.store(true, std::memory_order_release);
        })
        >> submit(ctx);

    if (wait_for(done, 15000) && total.load() == 100) PASS();
    else FAIL("total=" + std::to_string(total.load()) + "/100");
}

// ============================================================================
// main
// ============================================================================
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    pump::core::this_core_id = NUM_CORES;  // main thread gets unique core_id

    std::cout << "========================================" << std::endl;
    std::cout << "Scope Stress Tests (" << NUM_CORES << " cores)" << std::endl;
    std::cout << "========================================" << std::endl;

    start_workers();

    std::cout << "\n--- Flat & Concurrency ---" << std::endl;
    test_massive_flat_map();
    test_deep_flat_chain();
    test_nested_concurrency();

    std::cout << "\n--- Exceptions ---" << std::endl;
    test_exception_storm();
    test_exception_flat_concurrent_combo();
    test_ignore_inner_exception();

    std::cout << "\n--- Combinators ---" << std::endl;
    test_when_all_stress();
    test_when_all_with_exceptions();
    test_when_any_stress();
    test_mixed_combinators();

    std::cout << "\n--- Context & Visit ---" << std::endl;
    test_visit_branching();
    test_context_concurrent();

    std::cout << "\n--- Rapid Submit ---" << std::endl;
    test_rapid_submit();

    std::cout << "\n--- Concurrent Boundary Conditions ---" << std::endl;
    test_concurrent_repeated_streams();
    test_concurrent_single_element();
    test_concurrent_empty_stream();
    test_concurrent_serial_degradation();
    test_concurrent_all_complete_before_source_done();
    test_nested_concurrent_inner_exception();
    test_concurrent_large_fanout();

    // Let in-flight work settle
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    stop_workers();

    std::cout << "\n========================================" << std::endl;
    std::cout << "Results: " << pass_count << "/" << test_count << " passed";
    if (fail_count > 0) std::cout << " (" << fail_count << " FAILED)";
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;

    return fail_count > 0 ? 1 : 0;
}
