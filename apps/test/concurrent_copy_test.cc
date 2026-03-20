// Test: concurrent_copy semantics for reduce, for_each, etc.
//
// Validates that:
// 1. reduce::op with copyable result_t has concurrent_copy (returns fresh initial state)
// 2. reduce::op with move-only result_t does NOT have concurrent_copy (compile-time check)
// 3. reduce(init, f) concurrent_copy preserves the original init value
// 4. Nested pipeline inside concurrent(N) via flat_map works correctly
// 5. reduce_carrier delegates concurrent_copy to inner op

#include <cstdio>
#include <atomic>
#include <cassert>
#include <memory>

#include "env/scheduler/task/tasks_scheduler.hh"
#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/for_each.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/on.hh"
#include "pump/core/context.hh"
#include "pump/core/concurrent_copy.hh"

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

// ── Compile-time checks ──

// reduce::op with copyable result → concurrent_copy exists
static_assert(requires {
    std::declval<const pump::sender::_reduce::op<int, decltype([](int&, int){})>>()
        .concurrent_copy();
}, "reduce::op<int, f> should have concurrent_copy");

// reduce::op with move-only result → concurrent_copy does NOT exist
struct move_only_result {
    int value;
    move_only_result(int v) : value(v) {}
    move_only_result(move_only_result&&) = default;
    move_only_result(const move_only_result&) = delete;
};
// move-only result_t → copyable_result is false → concurrent_copy requires fails
static_assert(!std::copy_constructible<move_only_result>,
    "move_only_result should not be copy constructible");
static_assert(!pump::sender::_reduce::op<move_only_result, decltype([](move_only_result&, int){})>::copyable_result,
    "reduce::op<move_only_result, f> should have copyable_result = false");

// ── Test 1: reduce::op concurrent_copy returns initial value, not current ──
static void test_reduce_op_concurrent_copy_initial_value() {
    printf("Test 1: reduce::op concurrent_copy returns initial value ... ");

    auto func = [](int& acc, int v) { acc += v; };
    pump::sender::_reduce::op<int, decltype(func)> op(int(42), decltype(func)(func));

    // Simulate accumulation
    op.do_run(10);
    op.do_run(20);
    assert(op.result == 72); // 42 + 10 + 20

    // concurrent_copy should return fresh op with initial value 42, not 72
    auto copy = op.concurrent_copy();
    assert(copy.result == 42);

    // Original unchanged
    assert(op.result == 72);

    printf("PASS\n");
}

// ── Test 2: reduce::op concurrent_copy with bool (default reduce) ──
static void test_reduce_op_default_concurrent_copy() {
    printf("Test 2: reduce::op default (bool) concurrent_copy ... ");

    auto func = [](bool& acc, bool v) {};
    pump::sender::_reduce::op<bool, decltype(func)> op(bool(false), decltype(func)(func));

    op.result = true; // simulate done

    auto copy = op.concurrent_copy();
    assert(copy.result == false); // reset to initial

    printf("PASS\n");
}

// ── Test 3: concurrent(N) >> flat_map(sub_pipeline_with_reduce) ──
static void test_concurrent_flat_map_with_inner_reduce() {
    printf("Test 3: concurrent(N) >> flat_map(inner reduce) ... ");
    fflush(stdout);

    std::atomic<bool> done{false};
    std::atomic<int> total{0};

    auto ctx = pump::core::make_root_context();

    // 8 concurrent elements, each runs a sub-pipeline with for_each + reduce
    just()
        >> on(g_sched->as_task())
        >> loop(8u)
        >> concurrent(4)
        >> flat_map([&total](uint64_t idx) {
            return just(std::views::iota(0u, 4u))
                >> for_each()
                >> then([&total](uint32_t v) {
                    total.fetch_add(1, std::memory_order_relaxed);
                })
                >> reduce();
        })
        >> reduce()
        >> then([&done](auto&&) {
            done.store(true, std::memory_order_release);
        })
        >> submit(ctx);

    run_until_done(g_sched, done);
    assert(total.load() == 32); // 8 * 4
    printf("PASS (%d elements)\n", total.load());
}

// ── Test 4: concurrent(N) >> reduce(init, f) accumulation ──
static void test_concurrent_reduce_with_init() {
    printf("Test 4: concurrent(N) >> flat_map >> reduce(init, f) ... ");
    fflush(stdout);

    std::atomic<bool> done{false};
    std::atomic<int> final_sum{0};

    auto ctx = pump::core::make_root_context();

    // 6 elements, each sub-pipeline reduces [0,1,2] with init=100
    // Expected per element: 100 + 0 + 1 + 2 = 103
    just()
        >> on(g_sched->as_task())
        >> loop(6u)
        >> concurrent(3)
        >> flat_map([&final_sum](uint64_t idx) {
            return just(std::views::iota(0, 3))
                >> for_each()
                >> reduce(100, [](int& acc, int v) { acc += v; })
                >> then([&final_sum](int sum) {
                    final_sum.fetch_add(sum, std::memory_order_relaxed);
                });
        })
        >> reduce()
        >> then([&done](auto&&) {
            done.store(true, std::memory_order_release);
        })
        >> submit(ctx);

    run_until_done(g_sched, done);
    assert(final_sum.load() == 618); // 6 * 103
    printf("PASS (sum=%d)\n", final_sum.load());
}

// ── Test 5: reduce_carrier concurrent_copy delegates correctly ──
static void test_reduce_carrier_concurrent_copy() {
    printf("Test 5: reduce_carrier concurrent_copy delegates ... ");

    auto func = [](int& acc, int v) { acc += v; };
    using op_t = pump::sender::_reduce::op<int, decltype(func)>;
    pump::core::reduce_carrier<3, op_t> carrier(op_t(int(50), decltype(func)(func)));

    // Accumulate
    carrier.do_run(10);
    assert(carrier.result == 60);

    // concurrent_copy should delegate to op_t's concurrent_copy → fresh with init=50
    auto copy = carrier.concurrent_copy();
    assert(copy.result == 50);

    printf("PASS\n");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("=== concurrent_copy semantics test ===\n");

    g_sched = new task_scheduler_t(0);

    test_reduce_op_concurrent_copy_initial_value();
    test_reduce_op_default_concurrent_copy();
    test_concurrent_flat_map_with_inner_reduce();
    test_concurrent_reduce_with_init();
    test_reduce_carrier_concurrent_copy();

    printf("=== all tests passed ===\n");
    delete g_sched;
    return 0;
}
