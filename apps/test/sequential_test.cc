#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <cassert>
#include <atomic>
#include <ranges>
#include <thread>
#include <chrono>

#include "pump/core/context.hh"
#include "pump/core/meta.hh"
#include "pump/core/lock_free_queue.hh"

#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/for_each.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/sequential.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/on.hh"

#include "env/scheduler/task/tasks_scheduler.hh"

using namespace pump::sender;
using namespace pump::core;

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
// Multi-core test infrastructure
// ============================================================================
static std::vector<pump::scheduler::task::scheduler*> g_schedulers;
static std::vector<std::thread> g_worker_threads;
static std::atomic<bool> g_workers_running{false};

// 极致高效的随机数生成器 (xorshift64*)
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

void start_workers(uint32_t num_cores) {
    g_workers_running.store(true);
    g_schedulers.clear();
    g_worker_threads.clear();

    for (uint32_t i = 0; i < num_cores; ++i) {
        g_schedulers.push_back(new pump::scheduler::task::scheduler(i));
    }

    for (uint32_t i = 0; i < num_cores; ++i) {
        g_worker_threads.emplace_back([i]() {
            this_core_id = i;
            auto* sche = g_schedulers[i];
            while (g_workers_running.load(std::memory_order_relaxed)) {
                if (!sche->advance()) {
                    std::this_thread::yield();
                }
            }
        });
    }
}

void stop_workers() {
    g_workers_running.store(false);
    for (auto& t : g_worker_threads) {
        if (t.joinable()) t.join();
    }
    for (auto* s : g_schedulers) {
        delete s;
    }
    g_schedulers.clear();
    g_worker_threads.clear();
}

// ============================================================================
// Test 1: Transparent sequential (no ops between sequential and all)
// Pattern: for_each >> sequential >> all >> then (no source_cache created)
// ============================================================================
void test_transparent_sequential() {
    TEST("transparent sequential (sequential >> all)");

    auto context = make_root_context();
    bool completed = false;
    bool all_succeed = false;

    just()
        >> for_each(std::views::iota(uint64_t(0), uint64_t(10)))
        >> sequential()
        >> all()
        >> then([&completed, &all_succeed](bool succeed) {
            completed = true;
            all_succeed = succeed;
        })
        >> submit(context);

    if (completed && all_succeed) {
        PASS();
    } else {
        FAIL("transparent sequential failed");
    }
}

// ============================================================================
// Test 2: Empty stream with sequential (immediate DONE)
// Pattern: for_each(empty) >> sequential >> all
// ============================================================================
void test_empty_stream() {
    TEST("empty stream (immediate DONE)");

    auto context = make_root_context();
    bool done_reached = false;

    just()
        >> for_each(std::views::iota(uint64_t(0), uint64_t(0)))
        >> sequential()
        >> all()
        >> then([&done_reached](bool succeed) {
            done_reached = true;
        })
        >> submit(context);

    if (done_reached) {
        PASS();
    } else {
        FAIL("done not reached for empty stream");
    }
}

// ============================================================================
// Test 3: Exception path through sequential
// Pattern: for_each >> then(throws) >> sequential >> all
// ============================================================================
void test_exception_path() {
    TEST("exception propagation through sequential");

    auto context = make_root_context();
    bool completed = false;
    bool all_result = true;

    just()
        >> for_each(std::views::iota(uint64_t(0), uint64_t(1000)))
        >> then([](uint64_t id) -> uint64_t {
            if (id == 999) {
                throw std::runtime_error("test_error");
            }
            return id;
        })
        >> sequential()
        >> all()
        >> then([&completed, &all_result](bool succeed) {
            completed = true;
            all_result = succeed;
        })
        >> submit(context);

    if (completed && !all_result) {
        PASS();
    } else {
        FAIL("exception not propagated correctly");
    }
}

// ============================================================================
// Test 4: Sequential ordering with concurrent (synchronous path)
// Pattern: for_each >> concurrent >> then >> sequential >> then >> all
// This exercises the source_cache with the new state machine
// ============================================================================
void test_concurrent_sequential_ordering() {
    TEST("concurrent >> sequential ordering (sync)");

    auto context = make_root_context();
    std::vector<uint64_t> results;

    just()
        >> for_each(std::views::iota(uint64_t(1), uint64_t(11)))
        >> concurrent(1000)
        >> then([](uint64_t id) {
            return id;
        })
        >> sequential()
        >> then([&results](uint64_t id) {
            results.push_back(id);
            return id;
        })
        >> all()
        >> then([](bool) {})
        >> submit(context);

    // In synchronous execution, concurrent doesn't reorder,
    // and sequential preserves FIFO from the queue
    std::vector<uint64_t> expected = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    if (results == expected) {
        PASS();
    } else {
        FAIL("ordering mismatch");
        std::cout << "    got (" << results.size() << "): ";
        for (auto v : results) std::cout << v << " ";
        std::cout << std::endl;
    }
}

// ============================================================================
// Test 5: Large stream through concurrent >> sequential
// ============================================================================
void test_large_stream() {
    TEST("large stream (1000 elements) concurrent >> sequential");

    auto context = make_root_context();
    std::vector<uint64_t> results;
    results.reserve(1000);

    just()
        >> for_each(std::views::iota(uint64_t(0), uint64_t(1000)))
        >> concurrent(2000)
        >> then([](uint64_t id) {
            return id;
        })
        >> sequential()
        >> then([&results](uint64_t id) {
            results.push_back(id);
            return id;
        })
        >> all()
        >> then([](bool) {})
        >> submit(context);

    bool ordered = true;
    if (results.size() != 1000) {
        ordered = false;
    } else {
        for (uint64_t i = 0; i < 1000; i++) {
            if (results[i] != i) { ordered = false; break; }
        }
    }

    if (ordered) {
        PASS();
    } else {
        FAIL("ordering broken for large stream");
        std::cout << "    size=" << results.size() << std::endl;
    }
}

// ============================================================================
// Test 6: Single element through concurrent >> sequential
// ============================================================================
void test_single_element() {
    TEST("single element concurrent >> sequential");

    auto context = make_root_context();
    std::vector<uint64_t> results;

    just()
        >> for_each(std::views::iota(uint64_t(42), uint64_t(43)))
        >> concurrent(1000)
        >> then([](uint64_t id) {
            return id;
        })
        >> sequential()
        >> then([&results](uint64_t id) {
            results.push_back(id);
            return id;
        })
        >> all()
        >> then([](bool) {})
        >> submit(context);

    if (results.size() == 1 && results[0] == 42) {
        PASS();
    } else {
        FAIL("unexpected result for single element");
    }
}

// ============================================================================
// Test 7: Lockfree queue - adaptive storage for small objects
// ============================================================================
void test_lockfree_small_object() {
    TEST("lockfree queue small object (inline storage)");

    static_assert(std::is_same_v<adaptive_storage_t<int>, int>,
                  "int should use inline storage");

    pump::core::mpmc::queue<int> q{16};
    bool ok = true;

    for (int i = 0; i < 10; i++) {
        ok &= q.try_enqueue(int(i));
    }

    for (int i = 0; i < 10; i++) {
        auto val = q.try_dequeue();
        ok &= val.has_value();
        ok &= (*val == i);
    }

    ok &= q.empty();

    if (ok) {
        PASS();
    } else {
        FAIL("small object queue operations failed");
    }
}

// ============================================================================
// Test 8: Lockfree queue - adaptive storage for large objects
// ============================================================================
struct LargeObject {
    char data[128] = {};
    int id = 0;

    LargeObject() noexcept = default;
    LargeObject(int i) noexcept : id(i) { data[0] = static_cast<char>(i); }
    LargeObject(LargeObject&& o) noexcept : id(o.id) {
        std::copy(std::begin(o.data), std::end(o.data), std::begin(data));
    }
    LargeObject& operator=(LargeObject&& o) noexcept {
        id = o.id;
        std::copy(std::begin(o.data), std::end(o.data), std::begin(data));
        return *this;
    }
};

void test_lockfree_large_object() {
    TEST("lockfree adaptive storage for large objects");

    static_assert(sizeof(LargeObject) > 64, "LargeObject must be > 64B");
    static_assert(std::is_same_v<adaptive_storage_t<LargeObject>, std::unique_ptr<LargeObject>>,
                  "LargeObject should use unique_ptr storage");

    auto wrapped = adaptive_storage<LargeObject>::wrap(LargeObject(42));
    static_assert(std::is_same_v<decltype(wrapped), std::unique_ptr<LargeObject>>);
    auto& unwrapped = adaptive_storage<LargeObject>::unwrap(wrapped);
    bool ok = (unwrapped.id == 42);

    if (ok) {
        PASS();
    } else {
        FAIL("large object adaptive storage failed");
    }
}

// ============================================================================
// Test 9: Lockfree queue - full/overflow behavior
// ============================================================================
void test_lockfree_overflow() {
    TEST("lockfree queue overflow returns false");

    pump::core::mpmc::queue<int> q{4};
    bool ok = true;

    ok &= q.try_enqueue(int(1));
    ok &= q.try_enqueue(int(2));
    ok &= q.try_enqueue(int(3));
    ok &= q.try_enqueue(int(4));

    // Should fail - queue full
    ok &= !q.try_enqueue(int(5));

    // Dequeue one and try again
    auto val = q.try_dequeue();
    ok &= val.has_value() && (*val == 1);
    ok &= q.try_enqueue(int(5));

    if (ok) {
        PASS();
    } else {
        FAIL("queue overflow behavior incorrect");
    }
}

// ============================================================================
// Multi-core Test 10: concurrent >> sequential serialization
// Pattern: for_each >> concurrent >> flat_map(scheduler) >> then(work)
//          >> sequential >> flat_map(scheduler) >> then(verify serial) >> all
// Verifies: sequential 之后不应该并发
// ============================================================================
void test_multicore_sequential_serialization() {
    TEST("multi-core: concurrent >> sequential serialization");

    const uint64_t TOTAL_TASKS = 500;
    const uint64_t MAX_CONCURRENT = 100000;

    std::atomic<uint64_t> after_concurrent_current{0};
    std::atomic<uint64_t> after_concurrent_max{0};

    std::atomic<uint64_t> after_sequential_current{0};
    std::atomic<uint64_t> after_sequential_max{0};

    std::atomic<uint64_t> total_processed{0};
    std::atomic<uint64_t> total_sum{0};
    std::atomic<bool> completed{false};
    std::atomic<bool> all_result{false};

    auto context = make_root_context();

    just()
        >> for_each(std::views::iota(uint64_t(0), TOTAL_TASKS))
        >> concurrent(MAX_CONCURRENT)
        // 调度到随机 scheduler，实现真正并发
        >> flat_map([](uint64_t id) {
            return random_scheduler()->as_task() >> then([id]() { return id; });
        })
        // concurrent 之后，应该并发执行
        >> then([&](uint64_t id) {
            uint64_t cur = after_concurrent_current.fetch_add(1, std::memory_order_relaxed) + 1;
            uint64_t old_max = after_concurrent_max.load(std::memory_order_relaxed);
            while (cur > old_max && !after_concurrent_max.compare_exchange_weak(old_max, cur, std::memory_order_relaxed)) {}

            // 模拟工作负载
            volatile uint64_t dummy = 0;
            for (int i = 0; i < 1000; ++i) dummy += i * id;

            after_concurrent_current.fetch_sub(1, std::memory_order_relaxed);
            return id;
        })
        >> sequential()
        // sequential 之后再次调度
        >> flat_map([](uint64_t id) {
            return random_scheduler()->as_task() >> then([id]() { return id; });
        })
        // sequential 之后，不应该并发执行
        >> then([&](uint64_t id) {
            uint64_t cur = after_sequential_current.fetch_add(1, std::memory_order_relaxed) + 1;
            uint64_t old_max = after_sequential_max.load(std::memory_order_relaxed);
            while (cur > old_max && !after_sequential_max.compare_exchange_weak(old_max, cur, std::memory_order_relaxed)) {}

            // 模拟工作负载
            volatile uint64_t dummy = 0;
            for (int i = 0; i < 1000; ++i) dummy += i;

            after_sequential_current.fetch_sub(1, std::memory_order_relaxed);
            total_processed.fetch_add(1, std::memory_order_relaxed);
            total_sum.fetch_add(id, std::memory_order_relaxed);
            return id;
        })
        >> all()
        >> then([&](bool succeed) {
            all_result.store(succeed);
            completed.store(true);
        })
        >> submit(context);

    // 等待完成
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    uint64_t expected_sum = TOTAL_TASKS * (TOTAL_TASKS - 1) / 2;
    bool sequential_ok = after_sequential_max.load() <= 1;
    bool completion_ok = total_processed.load() == TOTAL_TASKS;
    bool sum_ok = total_sum.load() == expected_sum;
    bool done_ok = completed.load() && all_result.load();

    if (sequential_ok && completion_ok && sum_ok && done_ok) {
        PASS();
    } else {
        FAIL("multi-core serialization failed");
        std::cout << "    sequential_max_concurrent=" << after_sequential_max.load()
                  << " (expect<=1), concurrent_max=" << after_concurrent_max.load()
                  << ", processed=" << total_processed.load() << "/" << TOTAL_TASKS
                  << ", sum=" << total_sum.load() << "/" << expected_sum
                  << ", done=" << completed.load() << ", all=" << all_result.load() << std::endl;
    }
}

// ============================================================================
// Multi-core Test 11: concurrent >> sequential sync path
// Pattern: for_each >> concurrent >> then(sync) >> sequential >> then(verify) >> all
// 不经过 flat_map 直接 then（同步路径）
// ============================================================================
void test_multicore_sequential_sync_path() {
    TEST("multi-core: sequential sync path serialization");

    const uint64_t TOTAL_TASKS = 10000;
    const uint64_t MAX_CONCURRENT = 100000;

    std::atomic<uint64_t> after_sequential_current{0};
    std::atomic<uint64_t> after_sequential_max{0};
    std::atomic<uint64_t> total_processed{0};
    std::atomic<bool> completed{false};
    std::atomic<bool> all_result{false};

    auto context = make_root_context();

    just()
        >> for_each(std::views::iota(uint64_t(0), TOTAL_TASKS))
        >> concurrent(MAX_CONCURRENT)
        >> then([](uint64_t id) {
            return id;
        })
        >> sequential()
        // 同步路径：不经过 flat_map 直接 then
        >> then([&](uint64_t id) {
            uint64_t cur = after_sequential_current.fetch_add(1, std::memory_order_relaxed) + 1;
            uint64_t old_max = after_sequential_max.load(std::memory_order_relaxed);
            while (cur > old_max && !after_sequential_max.compare_exchange_weak(old_max, cur, std::memory_order_relaxed)) {}

            volatile uint64_t dummy = 0;
            for (int i = 0; i < 1000; ++i) dummy += i + id;

            after_sequential_current.fetch_sub(1, std::memory_order_relaxed);
            total_processed.fetch_add(1, std::memory_order_relaxed);
            return id;
        })
        >> all()
        >> then([&](bool succeed) {
            all_result.store(succeed);
            completed.store(true);
        })
        >> submit(context);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    bool sequential_ok = after_sequential_max.load() <= 1;
    bool completion_ok = total_processed.load() == TOTAL_TASKS;
    bool done_ok = completed.load() && all_result.load();

    if (sequential_ok && completion_ok && done_ok) {
        PASS();
    } else {
        FAIL("sync path serialization failed");
        std::cout << "    sequential_max_concurrent=" << after_sequential_max.load()
                  << " (expect<=1), processed=" << total_processed.load() << "/" << TOTAL_TASKS
                  << ", done=" << completed.load() << ", all=" << all_result.load() << std::endl;
    }
}

// ============================================================================
// Multi-core Test 12: empty stream with multi-core scheduler
// ============================================================================
void test_multicore_empty_stream() {
    TEST("multi-core: empty stream DONE");

    std::atomic<bool> done_reached{false};
    std::atomic<bool> succeed_val{false};

    auto context = make_root_context();

    just()
        >> for_each(std::views::iota(uint64_t(0), uint64_t(0)))
        >> sequential()
        >> all()
        >> then([&](bool succeed) {
            succeed_val.store(succeed);
            done_reached.store(true);
        })
        >> submit(context);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done_reached.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (done_reached.load() && succeed_val.load()) {
        PASS();
    } else {
        FAIL("empty stream DONE not reached");
    }
}

// ============================================================================
// Multi-core Test 13: exception path with multi-core scheduler
// ============================================================================
void test_multicore_exception_path() {
    TEST("multi-core: exception propagation through sequential");

    const uint64_t TOTAL_TASKS = 1000;

    std::atomic<bool> completed{false};
    std::atomic<bool> all_result{true};

    auto context = make_root_context();

    just()
        >> for_each(std::views::iota(uint64_t(0), TOTAL_TASKS))
        >> concurrent(100000)
        >> flat_map([](uint64_t id) {
            return random_scheduler()->as_task() >> then([id]() { return id; });
        })
        >> then([](uint64_t id) -> uint64_t {
            if (id == 999) {
                throw std::runtime_error("test exception");
            }
            return id;
        })
        >> sequential()
        >> all()
        >> then([&](bool succeed) {
            all_result.store(succeed);
            completed.store(true);
        })
        >> submit(context);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (completed.load() && !all_result.load()) {
        PASS();
    } else {
        FAIL("exception not propagated correctly");
        std::cout << "    completed=" << completed.load()
                  << ", all_result=" << all_result.load() << std::endl;
    }
}

// ============================================================================
// Multi-core Test 14: sequential non-blocking test
// 验证 sequential 不阻塞：快速任务应该在慢速任务完成前到达下游
// ============================================================================
void test_multicore_non_blocking() {
    TEST("multi-core: sequential non-blocking (fast task before slow)");

    const uint64_t TOTAL_TASKS = 100;

    std::atomic<bool> slow_task_finished_upstream{false};
    std::atomic<bool> fast_task_reached_downstream{false};
    std::atomic<bool> completed{false};
    std::atomic<bool> all_result{false};

    auto context = make_root_context();

    just()
        >> for_each(std::views::iota(uint64_t(0), TOTAL_TASKS))
        >> concurrent(100000)
        >> flat_map([](uint64_t id) {
            return random_scheduler()->as_task() >> then([id]() { return id; });
        })
        >> then([&](uint64_t id) {
            if (id == 0) {
                // 慢速任务：sleep 2 秒
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                slow_task_finished_upstream.store(true);
            } else {
                // 快速任务
                volatile uint64_t dummy = 0;
                for (int i = 0; i < 100; ++i) dummy += i;
            }
            return id;
        })
        >> sequential()
        >> flat_map([](uint64_t id) {
            return random_scheduler()->as_task() >> then([id]() { return id; });
        })
        >> then([&](uint64_t id) {
            // 非 id==0 的快速任务到达下游时，慢速任务可能还没完成
            if (id != 0) {
                if (!slow_task_finished_upstream.load()) {
                    fast_task_reached_downstream.store(true);
                }
            }
            return id;
        })
        >> all()
        >> then([&](bool succeed) {
            all_result.store(succeed);
            completed.store(true);
        })
        >> submit(context);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    bool done_ok = completed.load() && all_result.load();
    bool non_blocking_ok = fast_task_reached_downstream.load();

    if (done_ok && non_blocking_ok) {
        PASS();
    } else {
        FAIL("non-blocking test failed");
        std::cout << "    done=" << completed.load() << ", all=" << all_result.load()
                  << ", non_blocking=" << non_blocking_ok << std::endl;
    }
}

// ============================================================================
// Multi-core Test 15: large stream concurrent >> sequential with sum verification
// 大规模并发测试，验证数据完整性
// ============================================================================
void test_multicore_large_stream() {
    TEST("multi-core: large stream (500 elements) data integrity");

    const uint64_t TOTAL_TASKS = 500;
    const uint64_t MAX_CONCURRENT = 1000000;

    std::atomic<uint64_t> total_sum{0};
    std::atomic<uint64_t> total_processed{0};
    std::atomic<bool> completed{false};
    std::atomic<bool> all_result{false};

    auto context = make_root_context();

    just()
        >> for_each(std::views::iota(uint64_t(0), TOTAL_TASKS))
        >> concurrent(MAX_CONCURRENT)
        >> flat_map([](uint64_t id) {
            return random_scheduler()->as_task() >> then([id]() { return id; });
        })
        >> then([](uint64_t id) {
            volatile uint64_t dummy = 0;
            for (int i = 0; i < 100; ++i) dummy += i * id;
            return id;
        })
        >> sequential()
        >> flat_map([](uint64_t id) {
            return random_scheduler()->as_task() >> then([id]() { return id; });
        })
        >> then([&](uint64_t id) {
            total_sum.fetch_add(id, std::memory_order_relaxed);
            total_processed.fetch_add(1, std::memory_order_relaxed);
            return id;
        })
        >> all()
        >> then([&](bool succeed) {
            all_result.store(succeed);
            completed.store(true);
        })
        >> submit(context);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    uint64_t expected_sum = TOTAL_TASKS * (TOTAL_TASKS - 1) / 2;
    bool completion_ok = total_processed.load() == TOTAL_TASKS;
    bool sum_ok = total_sum.load() == expected_sum;
    bool done_ok = completed.load() && all_result.load();

    if (completion_ok && sum_ok && done_ok) {
        PASS();
    } else {
        FAIL("large stream data integrity failed");
        std::cout << "    processed=" << total_processed.load() << "/" << TOTAL_TASKS
                  << ", sum=" << total_sum.load() << "/" << expected_sum
                  << ", done=" << completed.load() << ", all=" << all_result.load() << std::endl;
    }
}

// ============================================================================
// Multi-core Test 16: before concurrent should NOT be concurrent
// Pattern: for_each >> flat_map(scheduler) >> then(measure) >> concurrent >> then >> all
// 验证：concurrent 之前，即使经过 scheduler 调度，也不应该并发
// ============================================================================
void test_multicore_before_concurrent_serial() {
    TEST("multi-core: before concurrent should NOT be concurrent");

    const uint64_t TOTAL_TASKS = 500;
    const uint64_t MAX_CONCURRENT = 100000;

    std::atomic<uint64_t> before_current{0};
    std::atomic<uint64_t> before_max{0};
    std::atomic<uint64_t> total_processed{0};
    std::atomic<bool> completed{false};
    std::atomic<bool> all_result{false};

    auto context = make_root_context();

    just()
        >> for_each(std::views::iota(uint64_t(0), TOTAL_TASKS))
        // 先调度到随机 scheduler
        >> flat_map([](uint64_t id) {
            return random_scheduler()->as_task() >> then([id]() { return id; });
        })
        // concurrent 之前，不应该并发
        >> then([&](uint64_t id) {
            uint64_t cur = before_current.fetch_add(1, std::memory_order_relaxed) + 1;
            uint64_t old_max = before_max.load(std::memory_order_relaxed);
            while (cur > old_max && !before_max.compare_exchange_weak(old_max, cur, std::memory_order_relaxed)) {}

            volatile uint64_t dummy = 0;
            for (int i = 0; i < 100; ++i) dummy += i;

            before_current.fetch_sub(1, std::memory_order_relaxed);
            total_processed.fetch_add(1, std::memory_order_relaxed);
            return id;
        })
        >> concurrent(MAX_CONCURRENT)
        >> then([](uint64_t id) { return id; })
        >> all()
        >> then([&](bool succeed) {
            all_result.store(succeed);
            completed.store(true);
        })
        >> submit(context);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    bool before_ok = before_max.load() <= 1;
    bool completion_ok = total_processed.load() == TOTAL_TASKS;
    bool done_ok = completed.load() && all_result.load();

    if (before_ok && completion_ok && done_ok) {
        PASS();
    } else {
        FAIL("before concurrent should not be concurrent");
        std::cout << "    before_max_concurrent=" << before_max.load()
                  << " (expect<=1), processed=" << total_processed.load() << "/" << TOTAL_TASKS
                  << ", done=" << completed.load() << ", all=" << all_result.load() << std::endl;
    }
}

// ============================================================================
// Multi-core Test 17: after concurrent SHOULD be concurrent
// Pattern: for_each >> concurrent >> flat_map(scheduler) >> then(measure) >> all
// 验证：concurrent 之后，经过 scheduler 调度，应该并发执行
// ============================================================================
void test_multicore_after_concurrent_parallel() {
    TEST("multi-core: after concurrent SHOULD be concurrent");

    const uint64_t TOTAL_TASKS = 500;
    const uint64_t MAX_CONCURRENT = 100000;

    std::atomic<uint64_t> after_current{0};
    std::atomic<uint64_t> after_max{0};
    std::atomic<uint64_t> total_processed{0};
    std::atomic<bool> completed{false};
    std::atomic<bool> all_result{false};

    auto context = make_root_context();

    just()
        >> for_each(std::views::iota(uint64_t(0), TOTAL_TASKS))
        >> concurrent(MAX_CONCURRENT)
        // 调度到随机 scheduler，实现真正并发
        >> flat_map([](uint64_t id) {
            return random_scheduler()->as_task() >> then([id]() { return id; });
        })
        // concurrent 之后，应该并发执行
        >> then([&](uint64_t id) {
            uint64_t cur = after_current.fetch_add(1, std::memory_order_relaxed) + 1;
            uint64_t old_max = after_max.load(std::memory_order_relaxed);
            while (cur > old_max && !after_max.compare_exchange_weak(old_max, cur, std::memory_order_relaxed)) {}

            // 模拟工作负载，增加并发窗口
            volatile uint64_t dummy = 0;
            for (int i = 0; i < 100000; ++i) dummy += i * id;

            after_current.fetch_sub(1, std::memory_order_relaxed);
            total_processed.fetch_add(1, std::memory_order_relaxed);
            return id;
        })
        >> all()
        >> then([&](bool succeed) {
            all_result.store(succeed);
            completed.store(true);
        })
        >> submit(context);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    bool after_ok = after_max.load() > 1;
    bool completion_ok = total_processed.load() == TOTAL_TASKS;
    bool done_ok = completed.load() && all_result.load();

    if (after_ok && completion_ok && done_ok) {
        PASS();
    } else {
        FAIL("after concurrent should be concurrent");
        std::cout << "    after_max_concurrent=" << after_max.load()
                  << " (expect>1), processed=" << total_processed.load() << "/" << TOTAL_TASKS
                  << ", done=" << completed.load() << ", all=" << all_result.load() << std::endl;
    }
}

// ============================================================================
// Multi-core Test 18: concurrent(N) limits concurrency to N
// Pattern: for_each >> concurrent(5) >> flat_map(scheduler) >> then(slow work + measure) >> all
// 验证：concurrent(5) 限制最大并发数不超过 5
// ============================================================================
void test_multicore_concurrent_limit_small() {
    TEST("multi-core: concurrent(5) limits max concurrency to 5");

    const uint64_t TOTAL_TASKS = 200;
    const uint64_t MAX_CONCURRENT = 5;

    std::atomic<uint64_t> current{0};
    std::atomic<uint64_t> max_observed{0};
    std::atomic<uint64_t> total_processed{0};
    std::atomic<bool> completed{false};
    std::atomic<bool> all_result{false};

    auto context = make_root_context();

    just()
        >> for_each(std::views::iota(uint64_t(0), TOTAL_TASKS))
        >> concurrent(MAX_CONCURRENT)
        >> flat_map([](uint64_t id) {
            return random_scheduler()->as_task() >> then([id]() { return id; });
        })
        >> then([&](uint64_t id) {
            uint64_t cur = current.fetch_add(1, std::memory_order_relaxed) + 1;
            uint64_t old_max = max_observed.load(std::memory_order_relaxed);
            while (cur > old_max && !max_observed.compare_exchange_weak(old_max, cur, std::memory_order_relaxed)) {}

            // 模拟较长工作负载，让并发窗口充分展开
            volatile uint64_t dummy = 0;
            for (int i = 0; i < 500000; ++i) dummy += i * id;

            current.fetch_sub(1, std::memory_order_relaxed);
            total_processed.fetch_add(1, std::memory_order_relaxed);
            return id;
        })
        >> all()
        >> then([&](bool succeed) {
            all_result.store(succeed);
            completed.store(true);
        })
        >> submit(context);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    bool limit_ok = max_observed.load() <= MAX_CONCURRENT;
    bool completion_ok = total_processed.load() == TOTAL_TASKS;
    bool done_ok = completed.load() && all_result.load();

    if (limit_ok && completion_ok && done_ok) {
        PASS();
    } else {
        FAIL("concurrent limit not respected");
        std::cout << "    max_observed=" << max_observed.load()
                  << " (expect<=" << MAX_CONCURRENT << "), processed=" << total_processed.load() << "/" << TOTAL_TASKS
                  << ", done=" << completed.load() << ", all=" << all_result.load() << std::endl;
    }
}

// ============================================================================
// Multi-core Test 19: concurrent(1) degrades to serial execution
// Pattern: for_each >> concurrent(1) >> flat_map(scheduler) >> then(measure) >> all
// 验证：concurrent(1) 时最大并发数应 <= 1，即串行执行
// ============================================================================
void test_multicore_concurrent_limit_one() {
    TEST("multi-core: concurrent(1) degrades to serial");

    const uint64_t TOTAL_TASKS = 100;

    std::atomic<uint64_t> current{0};
    std::atomic<uint64_t> max_observed{0};
    std::atomic<uint64_t> total_processed{0};
    std::atomic<bool> completed{false};
    std::atomic<bool> all_result{false};

    auto context = make_root_context();

    just()
        >> for_each(std::views::iota(uint64_t(0), TOTAL_TASKS))
        >> concurrent(1)
        >> flat_map([](uint64_t id) {
            return random_scheduler()->as_task() >> then([id]() { return id; });
        })
        >> then([&](uint64_t id) {
            uint64_t cur = current.fetch_add(1, std::memory_order_relaxed) + 1;
            uint64_t old_max = max_observed.load(std::memory_order_relaxed);
            while (cur > old_max && !max_observed.compare_exchange_weak(old_max, cur, std::memory_order_relaxed)) {}

            volatile uint64_t dummy = 0;
            for (int i = 0; i < 100000; ++i) dummy += i * id;

            current.fetch_sub(1, std::memory_order_relaxed);
            total_processed.fetch_add(1, std::memory_order_relaxed);
            return id;
        })
        >> all()
        >> then([&](bool succeed) {
            all_result.store(succeed);
            completed.store(true);
        })
        >> submit(context);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    bool serial_ok = max_observed.load() <= 1;
    bool completion_ok = total_processed.load() == TOTAL_TASKS;
    bool done_ok = completed.load() && all_result.load();

    if (serial_ok && completion_ok && done_ok) {
        PASS();
    } else {
        FAIL("concurrent(1) not serial");
        std::cout << "    max_observed=" << max_observed.load()
                  << " (expect<=1), processed=" << total_processed.load() << "/" << TOTAL_TASKS
                  << ", done=" << completed.load() << ", all=" << all_result.load() << std::endl;
    }
}

// ============================================================================
// Multi-core Test 20: concurrent(10) vs concurrent(50) staircase test
// 验证：较大的 concurrent 限制应该产生更高的并发度
// ============================================================================
void test_multicore_concurrent_limit_staircase() {
    TEST("multi-core: concurrent(10) vs concurrent(50) staircase");

    const uint64_t TOTAL_TASKS = 500;

    // Helper lambda to measure max concurrency for a given limit
    auto measure_max_concurrent = [TOTAL_TASKS](uint64_t limit) -> std::tuple<uint64_t, uint64_t, bool> {
        std::atomic<uint64_t> current{0};
        std::atomic<uint64_t> max_observed{0};
        std::atomic<uint64_t> total_processed{0};
        std::atomic<bool> completed{false};
        std::atomic<bool> all_result{false};

        auto context = make_root_context();

        just()
            >> for_each(std::views::iota(uint64_t(0), TOTAL_TASKS))
            >> concurrent(limit)
            >> flat_map([](uint64_t id) {
                return random_scheduler()->as_task() >> then([id]() { return id; });
            })
            >> then([&](uint64_t id) {
                uint64_t cur = current.fetch_add(1, std::memory_order_relaxed) + 1;
                uint64_t old_max = max_observed.load(std::memory_order_relaxed);
                while (cur > old_max && !max_observed.compare_exchange_weak(old_max, cur, std::memory_order_relaxed)) {}

                volatile uint64_t dummy = 0;
                for (int i = 0; i < 500000; ++i) dummy += i * id;

                current.fetch_sub(1, std::memory_order_relaxed);
                total_processed.fetch_add(1, std::memory_order_relaxed);
                return id;
            })
            >> all()
            >> then([&](bool succeed) {
                all_result.store(succeed);
                completed.store(true);
            })
            >> submit(context);

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        return {max_observed.load(), total_processed.load(), completed.load() && all_result.load()};
    };

    auto [max10, proc10, ok10] = measure_max_concurrent(10);
    auto [max50, proc50, ok50] = measure_max_concurrent(50);

    bool limit10_ok = max10 <= 10;
    bool limit50_ok = max50 <= 50;
    bool completion_ok = proc10 == TOTAL_TASKS && proc50 == TOTAL_TASKS;
    bool done_ok = ok10 && ok50;
    // 较大的 concurrent 限制应产生更高或相等的并发度
    bool staircase_ok = max50 >= max10;

    if (limit10_ok && limit50_ok && completion_ok && done_ok && staircase_ok) {
        PASS();
    } else {
        FAIL("concurrent staircase test failed");
        std::cout << "    concurrent(10): max=" << max10 << " (expect<=10), processed=" << proc10
                  << ", ok=" << ok10 << std::endl;
        std::cout << "    concurrent(50): max=" << max50 << " (expect<=50), processed=" << proc50
                  << ", ok=" << ok50 << std::endl;
        std::cout << "    staircase: max50(" << max50 << ") >= max10(" << max10 << ") = "
                  << (staircase_ok ? "true" : "false") << std::endl;
    }
}

// ============================================================================
// Main
// ============================================================================
int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Sequential Module Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    // Sequential pipeline tests (single-thread)
    std::cout << "\n--- Sequential Pipeline Tests ---" << std::endl;
    test_transparent_sequential();
    test_empty_stream();
    test_exception_path();

    // Concurrent + Sequential tests (exercises source_cache, single-thread)
    std::cout << "\n--- Concurrent + Sequential Tests ---" << std::endl;
    test_concurrent_sequential_ordering();
    test_single_element();
    test_large_stream();

    // Lockfree queue tests
    std::cout << "\n--- Lockfree Queue Tests ---" << std::endl;
    test_lockfree_small_object();
    test_lockfree_large_object();
    test_lockfree_overflow();

    // Multi-core concurrent tests
    uint32_t num_cores = std::min(8u, std::thread::hardware_concurrency());
    this_core_id = num_cores;  // main thread gets unique core ID to avoid SPSC race with worker 0
    std::cout << "\n--- Multi-core Concurrent Tests (using " << num_cores << " cores) ---" << std::endl;
    start_workers(num_cores);

    test_multicore_before_concurrent_serial();
    test_multicore_after_concurrent_parallel();
    test_multicore_sequential_serialization();
    test_multicore_sequential_sync_path();
    test_multicore_empty_stream();
    test_multicore_exception_path();
    test_multicore_non_blocking();
    test_multicore_large_stream();

    // Concurrency limit control tests
    std::cout << "\n--- Concurrency Limit Control Tests ---" << std::endl;
    test_multicore_concurrent_limit_small();
    test_multicore_concurrent_limit_one();
    test_multicore_concurrent_limit_staircase();

    stop_workers();

    // Summary
    std::cout << "\n========================================" << std::endl;
    std::cout << "Results: " << pass_count << "/" << test_count << " passed";
    if (fail_count > 0) {
        std::cout << ", " << fail_count << " FAILED";
    }
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;

    return fail_count > 0 ? 1 : 0;
}
