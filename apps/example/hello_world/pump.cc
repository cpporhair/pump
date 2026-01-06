/**
 * PUMP Framework 综合示例
 * 
 * 本示例演示 AI_Coding_Guidelines.md 文档中描述的各类 Sender 用法：
 * 1. 基础操作：just, then, submit
 * 2. 值变换：transform, flat_map
 * 3. 并发控制：when_all, for_each + concurrent
 * 4. 上下文管理：with_context, get_context, push_context, pop_context
 * 5. 异常处理：any_exception
 * 6. 流式处理：for_each, reduce
 * 7. 协程桥接：await_able
 */

#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>

// 核心头文件
#include "pump/core/context.hh"
#include "pump/core/meta.hh"

// Sender API 头文件
#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/when_all.hh"
#include "pump/sender/for_each.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/sequential.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/push_context.hh"
#include "pump/sender/pop_context.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/any_exception.hh"

// 协程支持
#include "pump/coro/coro.hh"
#include "pump/sender/await_sender.hh"

using namespace pump::sender;
using namespace pump::core;

// ============================================================================
// 示例1：基础操作 - just, then, submit
// ============================================================================
void demo_basic_operations() {
    std::cout << "\n=== 示例1：基础操作 (just, then, submit) ===" << std::endl;
    
    // 创建根上下文
    auto context = make_root_context();
    
    // 构建并提交 pipeline（链式调用，避免 must_rvalue 断言）
    just(10)
        >> then([](int x) {
            std::cout << "  Step 1: 输入值 = " << x << std::endl;
            return x * 2;
        })
        >> then([](int x) {
            std::cout << "  Step 2: 乘以2 = " << x << std::endl;
            return x + 5;
        })
        >> then([](int x) {
            std::cout << "  Step 3: 加5 = " << x << std::endl;
            std::cout << "  最终结果: " << x << std::endl;
        })
        >> submit(context);
}

// ============================================================================
// 示例2：值变换 - transform, flat_map
// ============================================================================
void demo_transform_and_flat_map() {
    std::cout << "\n=== 示例2：值变换 (transform, flat_map) ===" << std::endl;
    
    auto context = make_root_context();
    
    // transform: 对值进行变换
    // flat_map: 返回一个新的 sender 并展开执行
    just(5)
        >> transform([](int x) {
            std::cout << "  transform: " << x << " -> " << x * x << std::endl;
            return x * x;
        })
        >> flat_map([](int x) {
            std::cout << "  flat_map: 创建新的 sender，值 = " << x << std::endl;
            return just(x + 100)
                >> then([](int y) {
                    std::cout << "  flat_map 内部: " << y << std::endl;
                    return y;
                });
        })
        >> then([](int x) {
            std::cout << "  最终结果: " << x << std::endl;
        })
        >> submit(context);
}

// ============================================================================
// 示例3：并发控制说明
// ============================================================================
void demo_when_all() {
    std::cout << "\n=== 示例3：并发控制 (when_all) ===" << std::endl;
    
    // when_all 用于并行执行多个 sender，等待全部完成
    // 典型用法（在 KV 项目中）：
    //   just()
    //       >> when_all(
    //           sender1 >> then([](auto x) { return result1; }),
    //           sender2 >> then([](auto x) { return result2; }),
    //           sender3 >> then([](auto x) { return result3; })
    //       )
    //       >> then([](auto&& results) {
    //           // results 是 tuple<variant<monostate, exception_ptr, T>, ...>
    //           // 处理聚合结果
    //       });
    //
    // 注意：when_all 通常与调度器配合使用，在纯同步环境下
    // 建议使用 for_each + concurrent 模式

    just()
        >> when_all(
            just() >> then([](...) { return 1; }),
            just(2) >> then([](auto &&a) { return a; }),
            just(3) >> then([](auto &&a) { return a; })
        )
        >> then([](...) {
        })
        >> submit(make_root_context());

}

// ============================================================================
// 示例4：流式处理 - for_each + concurrent + reduce
// ============================================================================
void demo_stream_processing() {
    std::cout << "\n=== 示例4：流式处理 (for_each, concurrent, reduce) ===" << std::endl;
    
    auto context = make_root_context();
    
    // 创建一个数据集
    std::vector<int> items = {1, 2, 3, 4, 5};
    
    // for_each: 将 vector 转为流
    // concurrent: 并发处理（最大并发数）
    // reduce: 收集所有结果
    // 注意：使用 just(items) >> for_each_by_args() 作为起点
    just(std::move(items))
        >> for_each_by_args()
        >> concurrent(2)  // 最大并发数为 2
        >> then([](int item) {
            int result = item * item;
            std::cout << "  处理元素: " << item << " -> " << result << std::endl;
            return result;
        })
        >> reduce()
        >> then([](auto&& results) {
            std::cout << "  流式处理完成" << std::endl;
        })
        >> submit(context);
}

// ============================================================================
// 示例5：上下文管理 - push_context, get_context, pop_context
// ============================================================================

// 定义一个共享状态结构
struct SharedState {
    int counter = 0;
    std::string name = "SharedState";
    
    void increment() { ++counter; }
    int get() const { return counter; }
};

void demo_context_management() {
    std::cout << "\n=== 示例5：上下文管理 (push_context, get_context, pop_context) ===" << std::endl;
    
    auto context = make_root_context();
    
    // 创建共享状态
    auto shared_state = std::make_shared<SharedState>();
    // 保存一个副本用于最后的输出（因为 push_context 需要 move）
    auto shared_state_copy = shared_state;
    
    // 使用 push_context 和 pop_context 手动管理上下文
    // 注意：push_context 需要右值，使用 std::move 传递
    just()
        >> push_context(std::move(shared_state))
        >> then([]() {
            std::cout << "  进入上下文作用域" << std::endl;
        })
        >> get_context<std::shared_ptr<SharedState>>()
        >> then([](std::shared_ptr<SharedState>& state) {
            state->increment();
            std::cout << "  第一次递增，counter = " << state->get() << std::endl;
        })
        >> get_context<std::shared_ptr<SharedState>>()
        >> then([](std::shared_ptr<SharedState>& state) {
            state->increment();
            std::cout << "  第二次递增，counter = " << state->get() << std::endl;
        })
        >> pop_context()
        >> then([shared_state_copy]() {
            std::cout << "  离开上下文作用域，最终 counter = " 
                      << shared_state_copy->get() << std::endl;
        })
        >> submit(context);
}

// ============================================================================
// 示例6：异常处理 - any_exception
// ============================================================================
void demo_exception_handling() {
    std::cout << "\n=== 示例6：异常处理 (any_exception) ===" << std::endl;
    
    auto context = make_root_context();
    
    // any_exception: 捕获所有异常并恢复
    just(42)
        >> then([](int x) -> int {
            std::cout << "  准备抛出异常..." << std::endl;
            throw std::runtime_error("模拟错误");
            return x;  // 不会执行到这里
        })
        >> any_exception([](std::exception_ptr e) {
            try {
                std::rethrow_exception(e);
            } catch (const std::exception& ex) {
                std::cout << "  捕获异常: " << ex.what() << std::endl;
            }
            // 返回一个新的 sender 继续执行（错误恢复）
            return just(-1);
        })
        >> then([](int x) {
            std::cout << "  异常恢复后的值: " << x << std::endl;
        })
        >> submit(context);
}

// ============================================================================
// 示例7：协程桥接 - await_able
// ============================================================================
auto demo_coroutine_impl() -> pump::coro::return_yields<int> {
    std::cout << "  协程开始执行" << std::endl;
    
    for (int i = 1; i <= 3; ++i) {
        // 使用 await_able 将 sender 转换为可 co_await 的对象
        auto result = co_await (
            just(__mov__(i))
            >> then([](int x) {
                return x * 10;
            })
            >> await_able(make_root_context())
        );
        std::cout << "  协程计算结果: " << result << std::endl;
        co_yield result;
    }
    
    std::cout << "  协程结束" << std::endl;
    co_return 0;
}

void demo_coroutine_bridge() {
    std::cout << "\n=== 示例7：协程桥接 (await_able) ===" << std::endl;
    
    auto coro = demo_coroutine_impl();
    while (!coro.done()) {
        auto& promise = coro.resume();
        std::cout << "  主函数收到值: " << promise.take() << std::endl;
    }
}

// ============================================================================
// 示例8：综合示例 - 组合多种 Sender
// ============================================================================
void demo_comprehensive() {
    std::cout << "\n=== 示例8：综合示例 ===" << std::endl;
    
    auto context = make_root_context();
    
    // 模拟一个数据处理流程：
    // 1. 初始化数据
    // 2. 并发处理多个数据源
    // 3. 使用上下文传递状态
    // 4. 处理可能的异常
    
    auto shared_counter = std::make_shared<int>(0);
    
    just()
        >> then([]() {
            std::cout << "  [综合] 开始数据处理流程" << std::endl;
            return std::vector<int>{10, 20, 30};
        })
        >> push_context(std::move(shared_counter))
        >> flat_map([](std::vector<int> data) {
            // 注意：for_each 需要一个 sender 作为起点
            return just(std::move(data))
                >> for_each_by_args()
                >> concurrent(2)
                >> then([](int item) {
                    std::cout << "  [综合] 处理数据项: " << item << std::endl;
                    return item * 2;
                })
                >> reduce();
        })
        >> get_context<std::shared_ptr<int>>()
        >> then([](std::shared_ptr<int>& counter, auto&& results) {
            (*counter)++;
            std::cout << "  [综合] 处理完成，计数器 = " << *counter << std::endl;
        })
        >> pop_context()
        >> any_exception([](std::exception_ptr e) {
            std::cout << "  [综合] 捕获到异常，进行恢复" << std::endl;
            return just();
        })
        >> then([]() {
            std::cout << "  [综合] 流程结束" << std::endl;
        })
        >> submit(context);
}

// ============================================================================
// 主函数
// ============================================================================
int main(int argc, char **argv) {
    std::cout << "========================================" << std::endl;
    std::cout << "PUMP Framework 综合示例" << std::endl;
    std::cout << "验证 AI_Coding_Guidelines.md 文档" << std::endl;
    std::cout << "========================================" << std::endl;
    
    // 运行所有示例
    //demo_basic_operations();
    //demo_transform_and_flat_map();
    //demo_when_all();
    demo_stream_processing();
    //demo_context_management();
    //demo_exception_handling();
    //demo_coroutine_bridge();
    demo_comprehensive();
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "所有示例执行完成！" << std::endl;
    std::cout << "========================================" << std::endl;
    
    return 0;
}
