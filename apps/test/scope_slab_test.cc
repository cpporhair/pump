#include <iostream>
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <chrono>
#include <ranges>

#include "pump/core/context.hh"
#include "pump/core/scope.hh"

#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/for_each.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/on.hh"
#include "pump/sender/any_exception.hh"

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
    do { test_count++; std::cout << "  TEST: " << name << " ... " << std::flush; } while(0)

#define PASS() \
    do { pass_count++; std::cout << "PASS" << std::endl; } while(0)

#define FAIL(msg) \
    do { fail_count++; std::cout << "FAIL: " << msg << std::endl; } while(0)

// ============================================================================
// Multi-core infrastructure
// ============================================================================
static constexpr uint32_t NUM_CORES = 8;
static std::vector<pump::scheduler::task::scheduler*> g_schedulers;
static std::vector<std::thread> g_worker_threads;
static std::atomic<bool> g_workers_running{false};

pump::scheduler::task::scheduler*
random_scheduler() {
    static thread_local uint64_t state = reinterpret_cast<uintptr_t>(&state) ^
        static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    state ^= state >> 12; state ^= state << 25; state ^= state >> 27;
    return g_schedulers[(state * 0x2545F4914F6CDD1DULL) % g_schedulers.size()];
}

void start_workers() {
    g_workers_running.store(true);
    for (uint32_t i = 0; i < NUM_CORES; ++i)
        g_schedulers.push_back(new pump::scheduler::task::scheduler(i));
    for (uint32_t i = 0; i < NUM_CORES; ++i) {
        g_worker_threads.emplace_back([i]() {
            pump::core::this_core_id = i;
            auto* sche = g_schedulers[i];
            while (g_workers_running.load(std::memory_order_relaxed)) {
                if (!sche->advance()) std::this_thread::yield();
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

void stop_workers() {
    g_workers_running.store(false);
    for (auto& t : g_worker_threads) if (t.joinable()) t.join();
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
// Test 1: flat_map pool reuse (same thread alloc/dealloc)
// ============================================================================
void test_flat_map_pool_reuse() {
    TEST("flat_map pool reuse (10000 items, single thread)");

    std::atomic<bool> done{false};
    std::atomic<uint64_t> sum{0};
    auto ctx = make_root_context();

    just()
        >> for_each(std::views::iota(0u, 10000u))
        >> flat_map([](auto&& i) {
            return just(uint32_t(i)) >> then([](auto&& v) { return v; });
        })
        >> then([&sum](auto&& v) {
            sum.fetch_add(v, std::memory_order_relaxed);
        })
        >> reduce()
        >> then([&done](...) { done.store(true, std::memory_order_release); })
        >> submit(ctx);

    // single thread: run on main thread
    pump::core::this_core_id = NUM_CORES;  // avoid conflict with workers
    auto* sche = new pump::scheduler::task::scheduler(NUM_CORES);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        sche->advance();

    uint64_t expected = (uint64_t)10000 * 9999 / 2;
    if (done.load() && sum.load() == expected) PASS();
    else FAIL("sum=" + std::to_string(sum.load()) + " expected=" + std::to_string(expected));

    delete sche;
}

// ============================================================================
// Test 2: concurrent pool reuse (cross-thread alloc/dealloc)
// ============================================================================
void test_concurrent_pool_reuse() {
    TEST("concurrent pool reuse (20000 items, cross-core)");

    std::atomic<bool> done{false};
    std::atomic<uint64_t> counter{0};
    auto ctx = make_root_context();

    just()
        >> for_each(std::views::iota(0u, 20000u))
        >> concurrent(500)
        >> on(random_scheduler()->as_task())
        >> then([&counter](auto&&) {
            counter.fetch_add(1, std::memory_order_relaxed);
        })
        >> reduce()
        >> then([&done](...) { done.store(true, std::memory_order_release); })
        >> submit(ctx);

    if (wait_for(done) && counter.load() == 20000) PASS();
    else FAIL("counter=" + std::to_string(counter.load()));
}

// ============================================================================
// Test 3: concurrent + flat_map combined (cross-thread + sub-pipeline)
// ============================================================================
void test_concurrent_flat_map_combined() {
    TEST("concurrent + flat_map combined (10000 items)");

    std::atomic<bool> done{false};
    std::atomic<uint64_t> sum{0};
    auto ctx = make_root_context();

    just()
        >> for_each(std::views::iota(0u, 10000u))
        >> concurrent(200)
        >> on(random_scheduler()->as_task())
        >> flat_map([](auto&& i) {
            return just(uint32_t(i)) >> then([](auto&& v) { return uint32_t(v) * 2u; });
        })
        >> then([&sum](auto&& v) {
            sum.fetch_add(v, std::memory_order_relaxed);
        })
        >> reduce()
        >> then([&done](...) { done.store(true, std::memory_order_release); })
        >> submit(ctx);

    uint64_t expected = (uint64_t)10000 * 9999;  // sum(i*2) = 2 * sum(i) = 2 * 10000*9999/2
    if (wait_for(done) && sum.load() == expected) PASS();
    else FAIL("sum=" + std::to_string(sum.load()) + " expected=" + std::to_string(expected));
}

// ============================================================================
// Test 4: rapid fire-and-forget (root_scope pool reuse)
// ============================================================================
void test_rapid_fire_and_forget() {
    TEST("rapid fire-and-forget (50000 submits, root_scope pool)");

    std::atomic<uint64_t> counter{0};
    constexpr uint32_t N = 50000;

    for (uint32_t i = 0; i < N; ++i) {
        auto ctx = make_root_context();
        just(1u)
            >> on(random_scheduler()->as_task())
            >> then([&counter](auto&& v) {
                counter.fetch_add(v, std::memory_order_relaxed);
            })
            >> submit(ctx);
    }

    // wait for all to complete
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (counter.load(std::memory_order_acquire) < N &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (counter.load() == N) PASS();
    else FAIL("counter=" + std::to_string(counter.load()) + "/" + std::to_string(N));
}

// ============================================================================
// Test 5: pool hit rate verification
// ============================================================================
void test_pool_hit_rate() {
    TEST("pool hit rate > 90% on repeated flat_map");

    // Run flat_map on a dedicated thread so we can check thread_local stats
    std::atomic<bool> done{false};
    uint64_t total = 0, hits = 0;

    std::thread t([&]() {
        pump::core::this_core_id = NUM_CORES + 1;
        auto* sche = new pump::scheduler::task::scheduler(NUM_CORES + 1);

        scope_slab_stats::reset();

        std::atomic<bool> pipeline_done{false};
        auto ctx = make_root_context();

        just()
            >> for_each(std::views::iota(0u, 5000u))
            >> flat_map([](auto&& i) {
                return just(uint32_t(i)) >> then([](auto&& v) { return v; });
            })
            >> then([](auto&&) {})
            >> reduce()
            >> then([&pipeline_done](...) { pipeline_done.store(true, std::memory_order_release); })
            >> submit(ctx);

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!pipeline_done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline)
            sche->advance();

        total = scope_slab_stats::total_allocs;
        hits = scope_slab_stats::pool_hits;

        delete sche;
        done.store(true, std::memory_order_release);
    });

    t.join();

    double hit_rate = total > 0 ? (double)hits / total * 100.0 : 0.0;
    std::cout << "(allocs=" << total << " hits=" << hits
              << " rate=" << (int)hit_rate << "%) ... " << std::flush;

    // First alloc is always a miss, rest should be hits
    // With 5000 flat_maps, expect > 90% hit rate
    if (total > 0 && hit_rate > 90.0) PASS();
    else FAIL("hit_rate=" + std::to_string((int)hit_rate) + "% (expected >90%)");
}

// ============================================================================
// Test 6: nested flat_map (multi-level scope creation)
// ============================================================================
void test_nested_flat_map() {
    TEST("nested flat_map (2000 × 3-level nesting)");

    std::atomic<bool> done{false};
    std::atomic<uint64_t> counter{0};
    auto ctx = make_root_context();

    just()
        >> for_each(std::views::iota(0u, 2000u))
        >> concurrent(100)
        >> on(random_scheduler()->as_task())
        >> flat_map([](auto&&) {
            return just(1u) >> flat_map([](auto&& v) {
                auto val = uint32_t(v);
                return just(std::move(val)) >> then([](auto&& x) { return x; });
            });
        })
        >> then([&counter](auto&& v) {
            counter.fetch_add(v, std::memory_order_relaxed);
        })
        >> reduce()
        >> then([&done](...) { done.store(true, std::memory_order_release); })
        >> submit(ctx);

    if (wait_for(done) && counter.load() == 2000) PASS();
    else FAIL("counter=" + std::to_string(counter.load()));
}

// ============================================================================
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::cout << "========================================" << std::endl;
    std::cout << "Scope Slab Pool Tests (" << NUM_CORES << " cores)" << std::endl;
    std::cout << "========================================" << std::endl;

    start_workers();

    std::cout << std::endl << "--- Same-thread Pool Tests ---" << std::endl;
    test_flat_map_pool_reuse();
    test_pool_hit_rate();

    std::cout << std::endl << "--- Cross-thread Pool Tests ---" << std::endl;
    test_concurrent_pool_reuse();
    test_concurrent_flat_map_combined();

    std::cout << std::endl << "--- Stress Tests ---" << std::endl;
    test_rapid_fire_and_forget();
    test_nested_flat_map();

    stop_workers();

    std::cout << std::endl << "========================================" << std::endl;
    std::cout << "Results: " << pass_count << "/" << test_count << " passed" << std::endl;
    std::cout << "========================================" << std::endl;

    return fail_count > 0 ? 1 : 0;
}
