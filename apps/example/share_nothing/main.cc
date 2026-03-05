/**
 * Preemptive Task Stealing 测试
 *
 * 单条 pipeline 验证:
 *   for_each(0..N)
 *     >> concurrent(M)
 *     >> visit() 按奇偶分流
 *       - 奇数 → any_scheduler (preemptive, 多线程偷取)
 *       - 偶数 → get_by_core  (local, 单线程执行)
 *     >> flat()
 *     >> then(记录执行线程)
 *     >> all()
 */

#include <atomic>
#include <thread>
#include <chrono>
#include <iostream>
#include <ranges>
#include <unordered_map>
#include <mutex>
#include <variant>

#include "env/runtime/share_nothing.hh"
#include "env/scheduler/task/tasks_scheduler.hh"
#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/on.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/for_each.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/visit.hh"
#include "pump/core/context.hh"

using namespace pump;
using namespace pump::sender;

using task_scheduler = scheduler::task::scheduler;
using preemptive_scheduler = scheduler::task::preemptive_scheduler;
using runtime_t = env::runtime::global_runtime_t<task_scheduler>;

static constexpr uint32_t NUM_TASKS = 2000;
static constexpr uint32_t CONCURRENCY = 500;

struct odd_task { uint32_t i; };
struct even_task { uint32_t i; };

std::mutex g_mtx;
std::unordered_map<std::thread::id, uint32_t> g_odd_threads;   // preemptive
std::unordered_map<std::thread::id, uint32_t> g_even_threads;  // local
std::atomic<uint32_t> g_done{0};

void record_odd() {
    std::lock_guard lk(g_mtx);
    g_odd_threads[std::this_thread::get_id()]++;
}

void record_even() {
    std::lock_guard lk(g_mtx);
    g_even_threads[std::this_thread::get_id()]++;
}

void print_thread_map(const char* label,
                      const std::unordered_map<std::thread::id, uint32_t>& map) {
    std::cout << "\n--- " << label << " ---\n";
    uint32_t idx = 0;
    for (auto& [tid, cnt] : map)
        std::cout << "  thread #" << idx++ << ": " << cnt << " 个任务\n";
    std::cout << "  参与线程数: " << map.size() << "\n";
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::cout << "Preemptive Task Stealing Test\n";

    uint32_t cores_used = std::min(4u, std::thread::hardware_concurrency());
    if (cores_used < 2) {
        std::cout << "需要至少 2 个核心\n";
        return 1;
    }
    std::cout << "使用 " << cores_used << " 个调度器, "
              << NUM_TASKS << " 个任务, 并发 " << CONCURRENCY << "\n";

    auto* runtime = new runtime_t();
    for (uint32_t i = 0; i < cores_used; ++i)
        runtime->add_core_schedulers(i, new task_scheduler(i));

    auto* preemptive = runtime->any_scheduler<task_scheduler>();
    auto* local_sche = runtime->get_by_core<task_scheduler>(0);

    env::runtime::start(runtime);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto ctx = core::make_root_context();

    just()
        >> for_each(std::views::iota(0u, NUM_TASKS))
        >> concurrent(CONCURRENCY)
        // 奇偶分流: 产生 variant<odd_task, even_task>
        >> then([](uint32_t i) -> std::variant<odd_task, even_task> {
            if (i % 2 == 1)
                return odd_task{i};
            else
                return even_task{i};
        })
        >> visit()
        // visit 为每种 variant 类型分别实例化此 lambda
        >> then([preemptive, local_sche](auto&& task) {
            if constexpr (std::is_same_v<__typ__(task), odd_task>)
                return just(__fwd__(task).i) >> on(preemptive->as_task());
            else
                return just(__fwd__(task).i) >> on(local_sche->as_task());
        })
        >> flat()
        // 此时已在目标 scheduler 线程上执行
        >> then([](uint32_t i) {
            if (i % 2 == 1)
                record_odd();
            else
                record_even();
            volatile uint64_t dummy = 0;
            for (int j = 0; j < 3000; ++j) dummy += j;
            g_done.fetch_add(1, std::memory_order_relaxed);
            return i;
        })
        >> all()
        >> then([](bool ok) {
            std::cout << "\npipeline 完成, all() = " << (ok ? "true" : "false") << "\n";
            std::cout << "完成任务数: " << g_done.load() << "/" << NUM_TASKS << "\n";

            print_thread_map("奇数任务 → any_scheduler (preemptive, 应多线程)", g_odd_threads);
            print_thread_map("偶数任务 → get_by_core (local, 应单线程)", g_even_threads);

            bool odd_ok = g_odd_threads.size() > 1;
            bool even_ok = g_even_threads.size() == 1;
            bool count_ok = g_done.load() == NUM_TASKS;

            std::cout << "\n检查项:\n";
            std::cout << "  [" << (odd_ok ? "PASS" : "FAIL")
                      << "] 奇数(preemptive)多线程: " << g_odd_threads.size() << " (期望 > 1)\n";
            std::cout << "  [" << (even_ok ? "PASS" : "FAIL")
                      << "] 偶数(local)单线程: " << g_even_threads.size() << " (期望 == 1)\n";
            std::cout << "  [" << (count_ok ? "PASS" : "FAIL")
                      << "] 任务全部完成: " << g_done.load() << "/" << NUM_TASKS << "\n";

            if (odd_ok && even_ok && count_ok)
                std::cout << "\n[SUCCESS] 所有测试通过!\n";
            else
                std::cout << "\n[FAILED] 测试失败!\n";

            _exit(0);
        })
        >> submit(ctx);

    // 主线程等待
    std::this_thread::sleep_for(std::chrono::seconds(30));
    std::cout << "超时! 完成: " << g_done.load() << "/" << NUM_TASKS << "\n";
    _exit(1);
}
