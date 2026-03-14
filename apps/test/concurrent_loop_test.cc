// Test: concurrent >> flat_map(async) inside loop
//
// Reproduces a bug where concurrent_counter accumulates across loop iterations
// because the concurrent_starter op is reused without resetting.

#include <cstdio>
#include <atomic>
#include <thread>

#include "env/scheduler/task/tasks_scheduler.hh"
#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/for_each.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/sequential.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/on.hh"
#include "pump/core/context.hh"
#include "pump/core/lock_free_queue.hh"

using namespace pump::sender;
using task_scheduler_t = pump::scheduler::task::scheduler;

static task_scheduler_t* g_sched = nullptr;

static void run_until_done(task_scheduler_t* sched, std::atomic<bool>& done) {
    pump::core::this_core_id = 0;
    uint64_t spins = 0;
    while (!done.load(std::memory_order_acquire)) {
        sched->advance();
        if (++spins > 100000000ULL) {
            printf("TIMEOUT — hang detected\n");
            _exit(1);
        }
    }
}

// Test 1: loop(N) >> then(produce_vector) >> for_each() >> concurrent()
//         >> flat_map(sync_sender) >> reduce() >> reduce()
// The flat_map returns just(v) >> then(f) — synchronous, no scheduler hop.
// Without the counter reset fix, this hangs on iteration 2.
static void test_sync_flat_map_in_loop() {
    printf("Test 1: loop >> for_each >> concurrent >> flat_map(sync) ... ");
    fflush(stdout);

    std::atomic<bool> done{false};
    std::atomic<uint32_t> total{0};

    auto ctx = pump::core::make_root_context();

    just()
        >> on(g_sched->as_task())
        >> loop(5u)
        >> then([](auto&&) {
            return std::views::iota(0u, 4u);
        })
        >> for_each()
        >> concurrent()
        >> then([&total](auto&& v) {
            total.fetch_add(1, std::memory_order_relaxed);
            return just(uint32_t(v)) >> then([](uint32_t x) { return x * 2; });
        })
        >> flat()
        >> reduce()
        >> reduce()
        >> then([&done](auto&&) {
            done.store(true, std::memory_order_release);
        })
        >> submit(ctx);

    run_until_done(g_sched, done);
    printf("PASS (%u elements)\n", total.load());
}

// Test 2: Same but flat_map dispatches to task_scheduler (truly async)
static void test_async_flat_map_in_loop() {
    printf("Test 2: loop >> for_each >> concurrent >> flat_map(async) ... ");
    fflush(stdout);

    std::atomic<bool> done{false};
    std::atomic<uint32_t> total{0};

    auto ctx = pump::core::make_root_context();

    just()
        >> on(g_sched->as_task())
        >> loop(4u)
        >> then([](auto&&) {
            return std::views::iota(0u, 3u);
        })
        >> for_each()
        >> concurrent()
        >> then([&total](auto&& v) {
            total.fetch_add(1, std::memory_order_relaxed);
            return g_sched->as_task()
                >> then([v = uint32_t(v)]() { return v * 3; });
        })
        >> flat()
        >> reduce()
        >> reduce()
        >> then([&done](auto&&) {
            done.store(true, std::memory_order_release);
        })
        >> submit(ctx);

    run_until_done(g_sched, done);
    printf("PASS (%u elements)\n", total.load());
}

// Test 3: for_each(range) >> concurrent >> then(async) >> flat >> reduce
// inside loop — using for_each(range) variant
static void test_for_each_range_in_loop() {
    printf("Test 3: loop >> for_each(range) >> concurrent >> async ... ");
    fflush(stdout);

    std::atomic<bool> done{false};
    std::atomic<uint32_t> total{0};

    auto ctx = pump::core::make_root_context();

    auto items = std::views::iota(0u, 6u);

    just()
        >> on(g_sched->as_task())
        >> loop(3u)
        >> ignore_args()
        >> for_each(items)
        >> concurrent()
        >> then([&total](auto&& v) {
            total.fetch_add(1, std::memory_order_relaxed);
            return g_sched->as_task()
                >> then([v = uint32_t(v)]() { return v; });
        })
        >> flat()
        >> reduce()
        >> reduce()
        >> then([&done](auto&&) {
            done.store(true, std::memory_order_release);
        })
        >> submit(ctx);

    run_until_done(g_sched, done);
    printf("PASS (%u elements)\n", total.load());
}

// Test 4: sequential inside loop — source_cache state reset
static void test_sequential_in_loop() {
    printf("Test 4: loop >> for_each >> concurrent >> sequential ... ");
    fflush(stdout);

    std::atomic<bool> done{false};
    std::atomic<uint32_t> total{0};

    auto ctx = pump::core::make_root_context();

    just()
        >> on(g_sched->as_task())
        >> loop(4u)
        >> then([](auto&&) {
            return std::views::iota(0u, 5u);
        })
        >> for_each()
        >> concurrent()
        >> then([&total](auto&& v) {
            total.fetch_add(1, std::memory_order_relaxed);
            return v;
        })
        >> sequential()
        >> reduce()
        >> reduce()
        >> then([&done](auto&&) {
            done.store(true, std::memory_order_release);
        })
        >> submit(ctx);

    run_until_done(g_sched, done);
    printf("PASS (%u elements)\n", total.load());
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("=== stateful op loop reuse test ===\n");

    g_sched = new task_scheduler_t(0);

    test_sync_flat_map_in_loop();
    test_async_flat_map_in_loop();
    test_for_each_range_in_loop();
    test_sequential_in_loop();

    printf("=== all tests passed ===\n");
    delete g_sched;
    return 0;
}
