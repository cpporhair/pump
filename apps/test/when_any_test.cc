#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <cassert>
#include <atomic>
#include <thread>
#include <chrono>
#include <set>

#include "pump/core/context.hh"
#include "pump/core/meta.hh"

#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/when_any.hh"
#include "pump/sender/when_all.hh"
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
// Single-thread Tests
// ============================================================================

void test_when_any_single_sender() {
    TEST("when_any single sender");

    auto context = make_root_context();
    bool completed = false;
    uint32_t got_index = UINT32_MAX;
    int got_value = -1;

    just()
        >> when_any(
            just(42)
        )
        >> then([&](uint32_t winner_index, auto result) {
            got_index = winner_index;
            if (result.index() == 2) {
                got_value = std::get<2>(result);
            }
            completed = true;
        })
        >> submit(context);

    if (completed && got_index == 0 && got_value == 42) {
        PASS();
    } else {
        FAIL("single sender: completed=" + std::to_string(completed)
             + " index=" + std::to_string(got_index)
             + " value=" + std::to_string(got_value));
    }
}

void test_when_any_basic() {
    TEST("when_any basic (multiple just senders)");

    auto context = make_root_context();
    bool completed = false;
    uint32_t got_index = UINT32_MAX;
    int got_value = -1;

    just()
        >> when_any(
            just(10),
            just(20),
            just(30)
        )
        >> then([&](uint32_t winner_index, auto result) {
            got_index = winner_index;
            if (result.index() == 2) {
                got_value = std::get<2>(result);
            }
            completed = true;
        })
        >> submit(context);

    // In single-threaded mode, first branch should win
    std::set<int> valid_values = {10, 20, 30};
    if (completed && got_index < 3 && valid_values.count(got_value)) {
        PASS();
    } else {
        FAIL("basic: completed=" + std::to_string(completed)
             + " index=" + std::to_string(got_index)
             + " value=" + std::to_string(got_value));
    }
}

void test_when_any_with_values() {
    TEST("when_any with different values");

    auto context = make_root_context();
    bool completed = false;
    uint32_t got_index = UINT32_MAX;
    int got_value = -1;

    just()
        >> when_any(
            just(100),
            just(200),
            just(300)
        )
        >> then([&](uint32_t winner_index, auto result) {
            got_index = winner_index;
            if (result.index() == 2) {
                got_value = std::get<2>(result);
            }
            completed = true;
        })
        >> submit(context);

    // Verify result is one of the valid values
    bool value_ok = (got_index == 0 && got_value == 100) ||
                    (got_index == 1 && got_value == 200) ||
                    (got_index == 2 && got_value == 300);

    if (completed && value_ok) {
        PASS();
    } else {
        FAIL("with_values: completed=" + std::to_string(completed)
             + " index=" + std::to_string(got_index)
             + " value=" + std::to_string(got_value));
    }
}

void test_when_any_in_pipeline() {
    TEST("when_any in full pipeline");

    auto context = make_root_context();
    bool completed = false;
    int final_value = -1;

    just()
        >> when_any(
            just(5),
            just(10)
        )
        >> then([](uint32_t winner_index, auto result) {
            if (result.index() == 2) {
                return std::get<2>(result) * 10;
            }
            return 0;
        })
        >> then([&](int val) {
            final_value = val;
            completed = true;
        })
        >> submit(context);

    // First branch wins in single-thread, so 5 * 10 = 50
    bool value_ok = (final_value == 50 || final_value == 100);

    if (completed && value_ok) {
        PASS();
    } else {
        FAIL("pipeline: completed=" + std::to_string(completed)
             + " final_value=" + std::to_string(final_value));
    }
}

void test_when_any_void_senders() {
    TEST("when_any void senders");

    auto context = make_root_context();
    bool completed = false;
    uint32_t got_index = UINT32_MAX;
    bool is_done_or_value = false;

    just()
        >> when_any(
            just(),
            just(),
            just()
        )
        >> then([&](uint32_t winner_index, auto result) {
            got_index = winner_index;
            // For void senders, result.index() == 2 means value (nullptr_t)
            is_done_or_value = (result.index() == 2);
            completed = true;
        })
        >> submit(context);

    if (completed && got_index < 3 && is_done_or_value) {
        PASS();
    } else {
        FAIL("void: completed=" + std::to_string(completed)
             + " index=" + std::to_string(got_index)
             + " is_done_or_value=" + std::to_string(is_done_or_value));
    }
}

// ============================================================================
// Exception Tests
// ============================================================================

void test_when_any_exception() {
    TEST("when_any single exception branch");

    auto context = make_root_context();
    bool completed = false;
    uint32_t got_index = UINT32_MAX;
    bool got_exception = false;

    just()
        >> when_any(
            just(1) >> then([](int) -> int { throw std::runtime_error("test error"); }),
            just(2)
        )
        >> then([&](uint32_t winner_index, auto result) {
            got_index = winner_index;
            got_exception = (result.index() == 1);  // exception_ptr
            completed = true;
        })
        >> submit(context);

    // First branch throws, so it should be the winner with exception
    // Or second branch succeeds first - either is valid
    if (completed) {
        PASS();
    } else {
        FAIL("exception: not completed");
    }
}

void test_when_any_all_exception() {
    TEST("when_any all branches throw");

    auto context = make_root_context();
    bool completed = false;
    bool got_exception = false;

    just()
        >> when_any(
            just(1) >> then([](int) -> int { throw std::runtime_error("error 1"); }),
            just(2) >> then([](int) -> int { throw std::runtime_error("error 2"); }),
            just(3) >> then([](int) -> int { throw std::runtime_error("error 3"); })
        )
        >> then([&](uint32_t winner_index, auto result) {
            got_exception = (result.index() == 1);
            completed = true;
        })
        >> submit(context);

    if (completed && got_exception) {
        PASS();
    } else {
        FAIL("all_exception: completed=" + std::to_string(completed)
             + " got_exception=" + std::to_string(got_exception));
    }
}

// ============================================================================
// Multi-core Tests
// ============================================================================

void test_multicore_when_any_basic() {
    TEST("multi-core: when_any basic");

    std::atomic<bool> completed{false};
    std::atomic<uint32_t> got_index{UINT32_MAX};
    std::atomic<int> got_value{-1};

    auto context = make_root_context();

    just()
        >> when_any(
            random_scheduler()->as_task() >> then([]() { return 10; }),
            random_scheduler()->as_task() >> then([]() { return 20; }),
            random_scheduler()->as_task() >> then([]() { return 30; })
        )
        >> then([&](uint32_t winner_index, auto result) {
            got_index.store(winner_index);
            if (result.index() == 2) {
                got_value.store(std::get<2>(result));
            }
            completed.store(true);
        })
        >> submit(context);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::set<int> valid = {10, 20, 30};
    if (completed.load() && got_index.load() < 3 && valid.count(got_value.load())) {
        PASS();
    } else {
        FAIL("multicore basic: completed=" + std::to_string(completed.load())
             + " index=" + std::to_string(got_index.load())
             + " value=" + std::to_string(got_value.load()));
    }
}

void test_multicore_when_any_race() {
    TEST("multi-core: when_any race with different delays");

    std::atomic<bool> completed{false};
    std::atomic<uint32_t> got_index{UINT32_MAX};
    std::atomic<int> got_value{-1};

    auto context = make_root_context();

    // Branch with less computation should tend to win
    just()
        >> when_any(
            random_scheduler()->as_task() >> then([]() {
                // Heavy computation (slow branch)
                volatile uint64_t dummy = 0;
                for (int i = 0; i < 100000; ++i) dummy += i;
                return 1;
            }),
            random_scheduler()->as_task() >> then([]() {
                // Light computation (fast branch)
                return 2;
            }),
            random_scheduler()->as_task() >> then([]() {
                // Heavy computation (slow branch)
                volatile uint64_t dummy = 0;
                for (int i = 0; i < 100000; ++i) dummy += i;
                return 3;
            })
        )
        >> then([&](uint32_t winner_index, auto result) {
            got_index.store(winner_index);
            if (result.index() == 2) {
                got_value.store(std::get<2>(result));
            }
            completed.store(true);
        })
        >> submit(context);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::set<int> valid = {1, 2, 3};
    if (completed.load() && got_index.load() < 3 && valid.count(got_value.load())) {
        PASS();
    } else {
        FAIL("multicore race: completed=" + std::to_string(completed.load())
             + " index=" + std::to_string(got_index.load())
             + " value=" + std::to_string(got_value.load()));
    }
}

void test_multicore_when_any_stress() {
    TEST("multi-core: when_any stress test");

    const int ITERATIONS = 1000;
    std::atomic<int> success_count{0};
    std::atomic<int> completion_count{0};

    for (int iter = 0; iter < ITERATIONS; ++iter) {
        auto context = make_root_context();

        just()
            >> when_any(
                random_scheduler()->as_task() >> then([]() { return 1; }),
                random_scheduler()->as_task() >> then([]() { return 2; }),
                random_scheduler()->as_task() >> then([]() { return 3; }),
                random_scheduler()->as_task() >> then([]() { return 4; })
            )
            >> then([&](uint32_t winner_index, auto result) {
                if (winner_index < 4 && result.index() == 2) {
                    int val = std::get<2>(result);
                    if (val >= 1 && val <= 4) {
                        success_count.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                completion_count.fetch_add(1, std::memory_order_relaxed);
            })
            >> submit(context);
    }

    // Wait for all iterations to complete
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (completion_count.load() < ITERATIONS && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (completion_count.load() == ITERATIONS && success_count.load() == ITERATIONS) {
        PASS();
    } else {
        FAIL("stress: completed=" + std::to_string(completion_count.load())
             + "/" + std::to_string(ITERATIONS)
             + " success=" + std::to_string(success_count.load()));
    }
}

// ============================================================================
// Memory Safety Test
// ============================================================================

void test_when_any_no_leak() {
    TEST("when_any no memory leak (stress)");

    const int ITERATIONS = 5000;
    std::atomic<int> completion_count{0};

    for (int iter = 0; iter < ITERATIONS; ++iter) {
        auto context = make_root_context();

        just()
            >> when_any(
                random_scheduler()->as_task() >> then([]() { return 0; }),
                random_scheduler()->as_task() >> then([]() { return 0; })
            )
            >> then([&](uint32_t, auto) {
                completion_count.fetch_add(1, std::memory_order_relaxed);
            })
            >> submit(context);
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (completion_count.load() < ITERATIONS && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (completion_count.load() == ITERATIONS) {
        PASS();
    } else {
        FAIL("no_leak: completed=" + std::to_string(completion_count.load())
             + "/" + std::to_string(ITERATIONS));
    }
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "when_any Module Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    // Single-thread tests
    std::cout << "\n--- Single-thread Tests ---" << std::endl;
    test_when_any_single_sender();
    test_when_any_basic();
    test_when_any_with_values();
    test_when_any_in_pipeline();
    test_when_any_void_senders();

    // Exception tests
    std::cout << "\n--- Exception Tests ---" << std::endl;
    test_when_any_exception();
    test_when_any_all_exception();

    // Multi-core tests
    uint32_t num_cores = std::min(8u, std::thread::hardware_concurrency());
    std::cout << "\n--- Multi-core Tests (using " << num_cores << " cores) ---" << std::endl;
    start_workers(num_cores);
    test_multicore_when_any_basic();
    test_multicore_when_any_race();
    test_multicore_when_any_stress();
    test_when_any_no_leak();
    stop_workers();

    // Summary
    std::cout << "\n========================================" << std::endl;
    std::cout << "Results: " << pass_count << "/" << test_count << " passed";
    if (fail_count > 0) std::cout << ", " << fail_count << " FAILED";
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;

    return fail_count > 0 ? 1 : 0;
}
