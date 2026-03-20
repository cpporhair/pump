
#include <numeric>
#include <thread>

#include "env/runtime/runner.hh"
#include "env/scheduler/nvme/scheduler.hh"
#include "env/scheduler/task/tasks_scheduler.hh"
#include "pump/core/random.hh"
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

struct
cpu_mask {
    std::vector<bool> mask;

    bool
    used_core(uint32_t id) const {
        return mask[id];
    }
};

std::vector<pump::scheduler::task::scheduler*> glb_schedulers_by_core;
std::vector<pump::scheduler::task::scheduler*> glb_schedulers_by_list;

// 极致高效的随机数生成器 (xorshift64*)
inline uint64_t
fast_random() {
    static thread_local uint64_t state = []() {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&state) ^ 
               static_cast<uint64_t>(__builtin_ia32_rdtsc()));
    }();
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545F4914F6CDD1DULL;
}

inline pump::scheduler::task::scheduler*
random_scheduler() {
    //auto i = pump::core::fast_random_uint32(glb_schedulers_by_list.size());
    auto i = fast_random() ;
    auto b = i % glb_schedulers_by_list.size();
    return glb_schedulers_by_list[b];
}

inline auto
on_any_task_scheduler() {
    return pump::sender::on(random_scheduler()->as_task());
}

inline auto
init_schedulers() {
    return pump::sender::get_context<cpu_mask>()
        >> pump::sender::then([](cpu_mask& mask) {
            glb_schedulers_by_core.resize(mask.mask.size());
            for (uint32_t i = 0; i < mask.mask.size(); ++i) {
                if (mask.mask[i]) {
                    auto sche = new pump::scheduler::task::scheduler(i);
                    glb_schedulers_by_core[i] = sche;
                    glb_schedulers_by_list.push_back(sche);
                }
            }
        });
}


inline auto
run_proc(uint32_t core, pump::scheduler::task::scheduler* sche) {
    pthread_t this_thread = pthread_self();
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    sched_setaffinity(this_thread, sizeof(cpuset), &cpuset);

    std::cout << "start at core " << core << std::endl;

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
    return pump::sender::get_context<cpu_mask>()
        >> pump::sender::then([](cpu_mask& mask) {
            uint32_t this_core = sched_getcpu();
            for (uint32_t i = 0; i < mask.mask.size(); ++i) {
                if (i != this_core && mask.mask[i]) {
                    std::jthread([i]() mutable { run_proc(i, glb_schedulers_by_core[i]); }).detach();
                }
            }

            if (mask.mask[this_core])
                run_proc(this_core, glb_schedulers_by_core[this_core]);
            else
                run_sleep(this_core);
        });
}

inline auto
start_env(cpu_mask&& mask) {
    return [mask=__fwd__(mask)](auto &&user_func) mutable  {
        pump::sender::just()
            >> pump::sender::push_context(__fwd__(mask))
            // init_schedulers根据传入的核心掩码.创建对应的task_scheduler
            >> init_schedulers()
            >> pump::sender::then([user_func = __fwd__(user_func)]() mutable {
                return pump::sender::just()
                    // on_any_task_scheduler调度到任意一个task_scheduler上
                    // 此时系统还没真的advance调度器,所以是暂时挂在调度器的任务队列里,
                    >> on_any_task_scheduler()
                    // user_func返回并执行用户的具体的业务逻辑
                    >> pump::sender::ignore_inner_exception(user_func())
                    >> pump::sender::submit(pump::core::make_root_context());
            })
            // run_schedule_proc根据传入的mask掩码,启动线程,绑核,实现share_nothing的能力
            >> run_schedule_proc()
            >> pump::sender::pop_context()
            >> pump::sender::submit(pump::core::make_root_context());
    };
}

// 测试1: for_each 和 concurrent 紧挨着的场景
inline auto
test_direct_concurrent() {
    std::cout << "=== Test 1: for_each >> concurrent (直接连接) ===" << std::endl;
    return pump::sender::for_each(std::views::iota(static_cast<uint64_t>(0), static_cast<uint64_t>(1000000)))
        // concurrent的作用是允许之后的代码,一直到pump::sender::all(),可以以最大100的数量并发
        >> pump::sender::concurrent(100)
        // 调度到任意一个task_scheduler上.on_any_task_scheduler这里才会开始真的并发
        >> pump::sender::flat_map([](uint64_t id) {
            return random_scheduler()->as_task() >> pump::sender::then([id]() { return id; });
        })
        >> pump::sender::then([](uint64_t id) {
            return id * 2;
        })
        >> pump::sender::all()
        >> pump::sender::then([](bool succeed) {
            std::cout << "Test 1 result: " << (succeed ? "PASS" : "FAIL") << std::endl;
            return succeed;
        });
}

// 测试2: for_each 和 concurrent 之间有异步调度算子的场景
// 关键点: for_each 和 concurrent 之间的算子不应该并发执行
inline auto
test_async_between_foreach_concurrent() {
    // 用于检测并发的原子计数器
    // 如果 for_each 和 concurrent 之间的算子并发执行，max_concurrent 会大于 1
    static std::atomic<uint64_t> current_concurrent{0};
    static std::atomic<uint64_t> max_concurrent{0};
    static std::atomic<uint64_t> total_processed{0};
    
    // 重置计数器
    current_concurrent.store(0);
    max_concurrent.store(0);
    total_processed.store(0);
    
    std::cout << "=== Test 2: for_each >> scheduler >> then >> concurrent (异步调度) ===" << std::endl;
    std::cout << "验证: for_each 和 concurrent 之间的算子不应该并发执行" << std::endl;
    
    return pump::sender::for_each(std::views::iota(static_cast<uint64_t>(0), static_cast<uint64_t>(10000)))
        // 异步调度到某个 scheduler 上
        >> pump::sender::flat_map([](uint64_t id) {
            return random_scheduler()->as_task() >> pump::sender::then([id]() { return id; });
        })
        // 这个 then 在 concurrent 之前，不应该并发执行
        >> pump::sender::then([](uint64_t id) {
            // 增加当前并发计数
            uint64_t cur = current_concurrent.fetch_add(1, std::memory_order_relaxed) + 1;
            // 更新最大并发数
            uint64_t old_max = max_concurrent.load(std::memory_order_relaxed);
            while (cur > old_max && !max_concurrent.compare_exchange_weak(old_max, cur, std::memory_order_relaxed)) {}
            
            // 模拟一些工作
            volatile int dummy = 0;
            for (int i = 0; i < 100; ++i) dummy += i;
            
            // 减少当前并发计数
            current_concurrent.fetch_sub(1, std::memory_order_relaxed);
            total_processed.fetch_add(1, std::memory_order_relaxed);
            
            return id;
        })
        // concurrent 之后才允许并发
        >> pump::sender::concurrent(100)
        >> pump::sender::flat_map([](uint64_t id) {
            return random_scheduler()->as_task() >> pump::sender::then([id]() { return id; });
        })
        >> pump::sender::then([](uint64_t id) {
            return id * 2;
        })
        >> pump::sender::all()
        >> pump::sender::then([](bool succeed) {
            uint64_t max_conc = max_concurrent.load();
            uint64_t total = total_processed.load();
            std::cout << "Total processed: " << total << std::endl;
            std::cout << "Max concurrent (between for_each and concurrent): " << max_conc << std::endl;
            
            // 如果 max_concurrent > 1，说明 for_each 和 concurrent 之间的算子并发执行了
            // 这是不期望的行为
            bool no_concurrent_before = (max_conc <= 1);
            std::cout << "Test 2 result: " << (succeed && no_concurrent_before ? "PASS" : "FAIL");
            if (!no_concurrent_before) {
                std::cout << " (ERROR: concurrent execution detected before concurrent operator!)";
            }
            std::cout << std::endl;
            return succeed && no_concurrent_before;
        });
}

int
main(int argc, char **argv) {
    cpu_mask mask;
    mask.mask.resize(std::thread::hardware_concurrency());
    mask.mask[0] = true;
    mask.mask[2] = true;
    mask.mask[4] = true;
    start_env(__mov__(mask))([]() {
        // 运行测试2: for_each 和 concurrent 之间有异步调度算子
        return test_async_between_foreach_concurrent();
    });
    return 0;
}