/**
 * Concurrent 并发控制测试
 * 
 * 测试目标：在一个流里同时验证两种情况
 * 1. for_each 和 concurrent 之间的算子【不应该】并发执行（max_concurrent <= 1）
 * 2. concurrent 之后的算子【应该】并发执行（max_concurrent > 1）
 * 
 * 测试流程：
 *   for_each(items)
 *       >> flat_map(scheduler)           // 异步调度
 *       >> then(检测并发-不应该并发)      // concurrent 之前
 *       >> concurrent(N)                  // 声明允许的最大并发数
 *       >> flat_map(scheduler)           // 异步调度
 *       >> then(检测并发-应该并发)        // concurrent 之后
 *       >> all()
 */

#include <numeric>
#include <thread>
#include <atomic>
#include <chrono>
#include <iostream>
#include <ranges>
#include <stdexcept>

#include "env/runtime/runner.hh"
#include "env/scheduler/task/tasks_scheduler.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/for_each.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/just.hh"
#include "pump/sender/on.hh"
#include "pump/sender/then.hh"
#include "pump/sender/push_context.hh"
#include "pump/sender/pop_context.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/sequential.hh"

using namespace pump;
using namespace pump::sender;

struct
cpu_mask {
    std::vector<bool> mask;

    bool
    used_core(uint32_t id) const {
        return mask[id];
    }
};

std::vector<scheduler::task::scheduler*> glb_schedulers_by_core;
std::vector<scheduler::task::scheduler*> glb_schedulers_by_list;

// 全局测试统计 - concurrent 之前（不应该并发）
std::atomic<uint64_t> g_before_current_concurrent{0};
std::atomic<uint64_t> g_before_max_concurrent{0};
std::atomic<uint64_t> g_before_total_processed{0};

// 全局测试统计 - concurrent 之后（应该并发）
std::atomic<uint64_t> g_after_current_concurrent{0};
std::atomic<uint64_t> g_after_max_concurrent{0};
std::atomic<uint64_t> g_after_total_processed{0};

// 全局测试统计 - sequential 之后（不应该并发）
std::atomic<uint64_t> g_after_sequential_current_concurrent{0};
std::atomic<uint64_t> g_after_sequential_max_concurrent{0};
std::atomic<uint64_t> g_after_sequential_total_processed{0};

// sequential 同步路径测试统计
std::atomic<uint64_t> g_sync_after_sequential_current_concurrent{0};
std::atomic<uint64_t> g_sync_after_sequential_max_concurrent{0};
std::atomic<uint64_t> g_sync_after_sequential_total_processed{0};

// 空流 DONE 测试
std::atomic<bool> g_empty_range_done{false};

// 异常路径测试
std::atomic<bool> g_exception_path_completed{false};
std::atomic<bool> g_exception_all_false{false};

// 测试 sequential 的非阻塞特性
std::atomic<bool> g_fast_task_reached_downstream{false};
std::atomic<bool> g_slow_task_finished_upstream{false};

// 总计
std::atomic<uint64_t> g_total_sum{0};
uint64_t g_total_tasks{0};

void reset_stats(uint64_t tasks) {
    g_before_current_concurrent.store(0);
    g_before_max_concurrent.store(0);
    g_before_total_processed.store(0);
    
    g_after_current_concurrent.store(0);
    g_after_max_concurrent.store(0);
    g_after_total_processed.store(0);

    g_after_sequential_current_concurrent.store(0);
    g_after_sequential_max_concurrent.store(0);
    g_after_sequential_total_processed.store(0);

    g_sync_after_sequential_current_concurrent.store(0);
    g_sync_after_sequential_max_concurrent.store(0);
    g_sync_after_sequential_total_processed.store(0);

    g_empty_range_done.store(false);
    g_exception_path_completed.store(false);
    g_exception_all_false.store(false);
    
    g_fast_task_reached_downstream.store(false);
    g_slow_task_finished_upstream.store(false);
    
    g_total_sum.store(0);
    g_total_tasks = tasks;
}

// concurrent 之前的任务进入
void enter_before_concurrent() {
    uint64_t cur = g_before_current_concurrent.fetch_add(1, std::memory_order_relaxed) + 1;
    // 更新最大并发数
    uint64_t old_max = g_before_max_concurrent.load(std::memory_order_relaxed);
    while (cur > old_max && !g_before_max_concurrent.compare_exchange_weak(old_max, cur, std::memory_order_relaxed)) {}
}

// concurrent 之前的任务退出
void exit_before_concurrent() {
    g_before_current_concurrent.fetch_sub(1, std::memory_order_relaxed);
    g_before_total_processed.fetch_add(1, std::memory_order_relaxed);
}

// concurrent 之后的任务进入
void enter_after_concurrent() {
    uint64_t cur = g_after_current_concurrent.fetch_add(1, std::memory_order_relaxed) + 1;
    // 更新最大并发数
    uint64_t old_max = g_after_max_concurrent.load(std::memory_order_relaxed);
    while (cur > old_max && !g_after_max_concurrent.compare_exchange_weak(old_max, cur, std::memory_order_relaxed)) {}
}

// concurrent 之后的任务退出
void exit_after_concurrent(uint64_t value) {
    g_after_current_concurrent.fetch_sub(1, std::memory_order_relaxed);
    g_after_total_processed.fetch_add(1, std::memory_order_relaxed);
    g_total_sum.fetch_add(value, std::memory_order_relaxed);
}

// sequential 之后的任务进入
void enter_after_sequential() {
    uint64_t cur = g_after_sequential_current_concurrent.fetch_add(1, std::memory_order_relaxed) + 1;
    // 更新最大并发数
    uint64_t old_max = g_after_sequential_max_concurrent.load(std::memory_order_relaxed);
    while (cur > old_max && !g_after_sequential_max_concurrent.compare_exchange_weak(old_max, cur, std::memory_order_relaxed)) {}
}

// sequential 之后的任务退出
void exit_after_sequential() {
    g_after_sequential_current_concurrent.fetch_sub(1, std::memory_order_relaxed);
    g_after_sequential_total_processed.fetch_add(1, std::memory_order_relaxed);
}

// sequential 同步路径的任务进入
void enter_sync_after_sequential() {
    uint64_t cur = g_sync_after_sequential_current_concurrent.fetch_add(1, std::memory_order_relaxed) + 1;
    uint64_t old_max = g_sync_after_sequential_max_concurrent.load(std::memory_order_relaxed);
    while (cur > old_max && !g_sync_after_sequential_max_concurrent.compare_exchange_weak(old_max, cur, std::memory_order_relaxed)) {}
}

// sequential 同步路径的任务退出
void exit_sync_after_sequential() {
    g_sync_after_sequential_current_concurrent.fetch_sub(1, std::memory_order_relaxed);
    g_sync_after_sequential_total_processed.fetch_add(1, std::memory_order_relaxed);
}

void print_result() {
    std::cout << "\n========== 测试结果 ==========" << std::endl;
    std::cout << "总任务数: " << g_total_tasks << std::endl;
    
    std::cout << "\n--- concurrent 之前 (不应该并发) ---" << std::endl;
    std::cout << "处理数量: " << g_before_total_processed.load() << std::endl;
    std::cout << "最大并发数: " << g_before_max_concurrent.load() << std::endl;
    
    std::cout << "\n--- concurrent 之后 (应该并发) ---" << std::endl;
    std::cout << "处理数量: " << g_after_total_processed.load() << std::endl;
    std::cout << "最大并发数: " << g_after_max_concurrent.load() << std::endl;

    std::cout << "\n--- sequential 之后 (不应该并发) ---" << std::endl;
    std::cout << "处理数量: " << g_after_sequential_total_processed.load() << std::endl;
    std::cout << "最大并发数: " << g_after_sequential_max_concurrent.load() << std::endl;

    std::cout << "\n--- sequential 同步路径 (不应该并发) ---" << std::endl;
    std::cout << "处理数量: " << g_sync_after_sequential_total_processed.load() << std::endl;
    std::cout << "最大并发数: " << g_sync_after_sequential_max_concurrent.load() << std::endl;

    std::cout << "\n--- 空流 DONE ---" << std::endl;
    std::cout << "已完成: " << (g_empty_range_done.load() ? "true" : "false") << std::endl;

    std::cout << "\n--- 异常路径 ---" << std::endl;
    std::cout << "已完成: " << (g_exception_path_completed.load() ? "true" : "false") << std::endl;
    std::cout << "all() 返回 false: " << (g_exception_all_false.load() ? "true" : "false") << std::endl;
    
    std::cout << "\n--- 业务逻辑 ---" << std::endl;
    std::cout << "累加结果: " << g_total_sum.load() << std::endl;
    uint64_t expected_sum = g_total_tasks * (g_total_tasks - 1) / 2;
    std::cout << "期望结果: " << expected_sum << std::endl;
    
    // 检查结果
    bool before_ok = g_before_max_concurrent.load() <= 1;
    bool after_ok = g_after_max_concurrent.load() > 1;
    bool sequential_ok = g_after_sequential_max_concurrent.load() <= 1;
    bool sequential_sync_ok = g_sync_after_sequential_max_concurrent.load() <= 1;
    bool completion_ok = g_after_sequential_total_processed.load() == g_total_tasks;
    bool sum_ok = g_total_sum.load() == expected_sum;
    bool non_blocking_ok = g_fast_task_reached_downstream.load();
    bool empty_range_ok = g_empty_range_done.load();
    bool exception_path_ok = g_exception_path_completed.load() && g_exception_all_false.load();
    
    std::cout << "\n检查项:" << std::endl;
    std::cout << "  [" << (before_ok ? "PASS" : "FAIL") << "] concurrent 之前不应该并发: "
              << "max_concurrent = " << g_before_max_concurrent.load() << " (期望 <= 1)" << std::endl;
    std::cout << "  [" << (after_ok ? "PASS" : "FAIL") << "] concurrent 之后应该并发: "
              << "max_concurrent = " << g_after_max_concurrent.load() << " (期望 > 1)" << std::endl;
    std::cout << "  [" << (sequential_ok ? "PASS" : "FAIL") << "] sequential 之后不应该并发: "
              << "max_concurrent = " << g_after_sequential_max_concurrent.load() << " (期望 <= 1)" << std::endl;
    std::cout << "  [" << (sequential_sync_ok ? "PASS" : "FAIL") << "] sequential 同步路径不应该并发: "
              << "max_concurrent = " << g_sync_after_sequential_max_concurrent.load() << " (期望 <= 1)" << std::endl;
    std::cout << "  [" << (completion_ok ? "PASS" : "FAIL") << "] 任务完成: "
              << g_after_sequential_total_processed.load() << " / " << g_total_tasks << std::endl;
    std::cout << "  [" << (sum_ok ? "PASS" : "FAIL") << "] 业务逻辑: "
              << "累加结果 " << g_total_sum.load() << " == 期望 " << expected_sum << std::endl;
    std::cout << "  [" << (non_blocking_ok ? "PASS" : "FAIL") << "] Sequential 非阻塞测试: "
              << (non_blocking_ok ? "快速任务已在慢速任务完成前到达下游" : "快速任务未能在慢速任务完成前到达下游") << std::endl;
    std::cout << "  [" << (empty_range_ok ? "PASS" : "FAIL") << "] 空流 DONE: "
              << (empty_range_ok ? "已完成" : "未完成") << std::endl;
    std::cout << "  [" << (exception_path_ok ? "PASS" : "FAIL") << "] 异常路径: "
              << (exception_path_ok ? "已完成且 all() 返回 false" : "未完成或 all() 返回非 false") << std::endl;
    
    if (before_ok && after_ok && sequential_ok && sequential_sync_ok && completion_ok && sum_ok && non_blocking_ok && empty_range_ok && exception_path_ok) {
        std::cout << "\n[SUCCESS] 测试通过!" << std::endl;
    } else {
        std::cout << "\n[FAILED] 测试失败!" << std::endl;
    }
    std::cout << "==============================\n" << std::endl;
}

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

inline scheduler::task::scheduler*
random_scheduler() {
    return glb_schedulers_by_list[fast_random() % glb_schedulers_by_list.size()];
}

inline auto
init_schedulers() {
    return get_context<cpu_mask>()
        >> then([](cpu_mask& mask) {
            glb_schedulers_by_core.resize(mask.mask.size());
            for (uint32_t i = 0; i < mask.mask.size(); ++i) {
                if (mask.mask[i]) {
                    auto sche = new scheduler::task::scheduler(i);
                    glb_schedulers_by_core[i] = sche;
                    glb_schedulers_by_list.push_back(sche);
                }
            }
            std::cout << "初始化了 " << glb_schedulers_by_list.size() << " 个调度器" << std::endl;
        });
}


inline auto
run_proc(uint32_t core, scheduler::task::scheduler* sche) {
    pthread_t this_thread = pthread_self();
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    sched_setaffinity(this_thread, sizeof(cpuset), &cpuset);

    while (true) {
        sche->advance();
    }
}

inline auto
run_sleep(uint32_t core) {
    pthread_t this_thread = pthread_self();
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    sched_setaffinity(static_cast<int>(this_thread), sizeof(cpuset), &cpuset);

    while (true) {
        sleep(1);
    }
}

inline auto
run_schedule_proc() {
    return get_context<cpu_mask>()
        >> then([](cpu_mask& mask) {
            uint32_t this_core = sched_getcpu();
            for (uint32_t i = 0; i < mask.mask.size(); ++i) {
                if (i != this_core && mask.mask[i]) {
                    std::thread([i]() mutable {
                        run_proc(i, glb_schedulers_by_core[i]);
                    }).detach();
                }
            }
            // 当前线程运行当前核心的调度器
            if (mask.mask[this_core])
                run_proc(this_core, glb_schedulers_by_core[this_core]);
            else
                run_sleep(this_core);
        });
}

// 测试函数：在一个流里同时测试应该并发和不应该并发两种情况
inline auto
user_test_func() {
    // 测试参数
    const uint64_t TOTAL_TASKS = 100000;
    const uint64_t MAX_CONCURRENT = 1000000;
    
    std::cout << "\n>>> 并发控制测试" << std::endl;
    std::cout << "总任务数: " << TOTAL_TASKS << std::endl;
    std::cout << "concurrent 最大并发数: " << MAX_CONCURRENT << std::endl;
    std::cout << "\n测试流程: for_each >> scheduler >> then(检测不并发) >> concurrent >> scheduler >> then(检测并发) >> sequential >> scheduler >> then(检测不并发) >> all" << std::endl;
    std::cout << "附加测试: sequential 同步路径、空流 DONE、异常路径" << std::endl;
    
    reset_stats(TOTAL_TASKS);
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    auto main_pipeline =
        just()
        >> for_each(std::views::iota(static_cast<uint64_t>(0), TOTAL_TASKS))
        // 第一次异步调度 - 在 concurrent 之前
        >> flat_map([](uint64_t id) {
            return random_scheduler()->as_task() >> then([id]() { return id; });
        })
        // 这个 then 在 concurrent 之前，【不应该】并发执行
        >> then([](uint64_t id) {
            enter_before_concurrent();
            
            // 模拟一些工作
            volatile int dummy = 0;
            for (int i = 0; i < 100; ++i) dummy += i;
            
            exit_before_concurrent();
            return id;
        })
        // concurrent 算子 - 声明允许的最大并发数
        >> concurrent(MAX_CONCURRENT)
        // 第二次异步调度 - 在 concurrent 之后
        >> flat_map([](uint64_t id) {
            return random_scheduler()->as_task() >> then([id]() { return id; });
        })
        // 这个 then 在 concurrent 之后，【应该】并发执行
        >> then([](uint64_t id) {
            enter_after_concurrent();
            
            // 模拟一些工作负载
            if (id == 0) {
                // 快速任务
                volatile uint64_t dummy = 0;
                for (int i = 0; i < 100; ++i) dummy += i;
            } else if (id == 1) {
                // 慢速任务
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                g_slow_task_finished_upstream.store(true);
            } else {
                volatile uint64_t dummy = 0;
                for (int i = 0; i < 100000; ++i) {
                    dummy += i * id;
                }
            }
            
            exit_after_concurrent(id);
            return id;
        })
        // sequential 算子 - 将并发流转回串行流
        >> sequential()
        // 第三次异步调度 - 在 sequential 之后
        >> flat_map([](uint64_t id) {
            return random_scheduler()->as_task() >> then([id]() { return id; });
        })
        // 这个 then 在 sequential 之后，【不应该】并发执行
        >> then([](uint64_t id) {
            enter_after_sequential();
            
            if (id != 1) {
                if (!g_slow_task_finished_upstream.load()) {
                    g_fast_task_reached_downstream.store(true);
                }
            }

            // 模拟一些工作负载
            volatile uint64_t dummy = 0;
            for (int i = 0; i < 1000; ++i) {
                dummy += i;
            }
            
            exit_after_sequential();
            return id;
        })
        >> all();

    auto sync_after_sequential =
        just()
        >> for_each(std::views::iota(static_cast<uint64_t>(0), TOTAL_TASKS))
        >> concurrent(MAX_CONCURRENT)
        >> then([](uint64_t id) {
            return id;
        })
        >> sequential()
        // 同步路径：不经过 flat_map 直接 then
        >> then([](uint64_t id) {
            enter_sync_after_sequential();
            volatile uint64_t dummy = 0;
            for (int i = 0; i < 1000; ++i) dummy += i + id;
            exit_sync_after_sequential();
            return id;
        })
        >> all();

    auto empty_range_done =
        just()
        >> for_each(std::views::iota(static_cast<uint64_t>(0), static_cast<uint64_t>(0)))
        >> sequential()
        >> all()
        >> then([](bool succeed) {
            g_empty_range_done.store(succeed);
            return succeed;
        });

    auto exception_path =
        just()
        >> for_each(std::views::iota(static_cast<uint64_t>(0), static_cast<uint64_t>(1000)))
        >> then([](uint64_t id) -> uint64_t {
            if (id == 999) {
                throw std::runtime_error("test exception");
            }
            return id;
        })
        >> sequential()
        >> all()
        >> then([](bool succeed) {
            g_exception_path_completed.store(true);
            g_exception_all_false.store(!succeed);
            return succeed;
        });

    return __mov__(main_pipeline)
        >> flat_map([sync_after_sequential = __mov__(sync_after_sequential)](bool) mutable {
            return __mov__(sync_after_sequential);
        })
        >> flat_map([empty_range_done = __mov__(empty_range_done)](bool) mutable {
            return __mov__(empty_range_done);
        })
        >> flat_map([exception_path = __mov__(exception_path)](bool) mutable {
            return __mov__(exception_path);
        })
        >> then([start_time](bool succeed) {
            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            std::cout << "\n所有任务完成，耗时: " << duration.count() << " ms" << std::endl;
            std::cout << "all() 返回: " << (succeed ? "true" : "false") << std::endl;
            print_result();
            
            std::cout << "\n========================================" << std::endl;
            std::cout << "测试完成!" << std::endl;
            std::cout << "========================================" << std::endl;
            std::exit(0);
        });
}

// 用于启动环境的辅助函数
inline auto
on_any_task_scheduler() {
    return on(random_scheduler()->as_task());
}

inline auto
start_env(cpu_mask&& mask) {
    return [mask=__fwd__(mask)](auto &&user_func) mutable  {
        just()
            >> push_context(__fwd__(mask))
            >> init_schedulers()
            >> then([user_func = __fwd__(user_func)]() mutable {
                auto user_sender = user_func();
                return just()
                    >> on_any_task_scheduler()
                    >> flat_map([s = __mov__(user_sender)](auto&& ...) mutable {
                        return __mov__(s);
                    })
                    >> ignore_all_exception()
                    >> submit(core::make_root_context());
            })
            >> run_schedule_proc()
            >> pop_context()
            >> submit(core::make_root_context());
    };
}

int
main(int argc, char **argv) {
    std::cout << "========================================" << std::endl;
    std::cout << "Share-Nothing Concurrent Control Test" << std::endl;
    std::cout << "并发控制测试 - 同时验证应该并发和不应该并发" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "\n##############################################################" << std::endl;
    std::cout << "# 测试目标:                                                    #" << std::endl;
    std::cout << "# 1. for_each 和 concurrent 之间的算子【不应该】并发          #" << std::endl;
    std::cout << "# 2. concurrent 之后的算子【应该】并发                        #" << std::endl;
    std::cout << "# 3. sequential 之后的算子【不应该】并发                      #" << std::endl;
    std::cout << "##############################################################\n" << std::endl;
    
    // 使用多个核心
    cpu_mask mask;
    mask.mask.resize(std::thread::hardware_concurrency());
    
    uint32_t cores_to_use = std::min(10u, static_cast<uint32_t>(mask.mask.size()));
    for (uint32_t i = 0; i < cores_to_use; ++i) {
        mask.mask[i] = true;
    }
    
    std::cout << "使用 " << cores_to_use << " 个核心" << std::endl;
    
    start_env(__mov__(mask))([]() {
        return user_test_func();
    });
    
    return 0;
}
