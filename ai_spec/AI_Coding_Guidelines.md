# PUMP Framework AI Coding Guidelines

> **目标受众**：AI代码助手  
> **文档目的**：指导AI在开发应用时正确、高效、安全地使用PUMP框架

---

## 1. Framework Summary (框架摘要)

### 一句话定义
PUMP是一个**基于Sender/Operator语义的高性能可组合异步流程管道库**，通过声明式的方式组合复杂异步逻辑，并支持在不同执行域（计算线程、NVMe IO、网络事件循环）之间切换。

### 核心设计哲学
| 设计原则 | 说明 |
|---------|------|
| **Flat OpTuple** | 将整个流水线的Op映射到平铺的`std::tuple`，而非递归嵌套结构 |
| **Position-based Pushing** | 通过编译期位置索引推进执行，减少运行时虚函数寻址 |
| **Stack-based Context** | 采用栈式上下文管理，支持跨步骤状态传递而无需显式参数传递 |
| **Scheduler Isolation** | 强调执行域隔离与显式切换，支持Task/NVMe/Net多种调度器 |
| **Lock-Free** | PUMP框架本身是无锁的，使用PUMP框架的项目也应该保持无锁设计，避免引入锁机制 |
| **Single-Thread Scheduler** | 所有 scheduler 都被设计成只能单线程执行，每个 scheduler 实例只能被一个线程运行；自定义 scheduler 也必须遵循此原则 |

### 相关文档

| 文档 | 说明 | 职责 |
|------|------|------|
| [CONCURRENCY_MODEL.md](./CONCURRENCY_MODEL.md) | **并发模型设计指南** | `concurrent` 算子的正确使用、多级并发架构、Leader/Follower 合并模式、`visit` 算子详解 |
| [SCHEDULER_SPEC.md](./SCHEDULER_SPEC.md) | 调度器规范 | scheduler 的统一定义、类型列表、运行方式 |
| [SENDERS_SPEC.md](./SENDERS_SPEC.md) | Sender 算子规范 | 所有已实现 sender 的语义、输入输出、注意事项 |
| [PUMP_SPEC.md](./PUMP_SPEC.md) | PUMP 框架规范 | 核心抽象（sender/op/pipeline/scope/context）与运行模型 |
| [PROJECT_AI_SPEC.md](./PROJECT_AI_SPEC.md) | 项目 AI 规范 | 模块索引、编码关注点、推荐阅读顺序 |

> **文档职责分工**：
> - 本文档（AI_Coding_Guidelines.md）侧重于**编码范式、禁忌事项、场景示例**
> - 具体 sender 语义请查阅 [SENDERS_SPEC.md](./SENDERS_SPEC.md)
> - 并发模型与 `visit` 算子详解请查阅 [CONCURRENCY_MODEL.md](./CONCURRENCY_MODEL.md)
> - scheduler 定义与类型请查阅 [SCHEDULER_SPEC.md](./SCHEDULER_SPEC.md)

> **重要**：在使用 `concurrent` 算子前，务必阅读 [CONCURRENCY_MODEL.md](./CONCURRENCY_MODEL.md)，理解 `concurrent` 只是声明允许并发，真正的并发需要配合调度器调度。

---

## 2. AI 问题分析与方案设计指南

> **本节目的**：指导 AI 在面对技术问题时，如何通过正确的问题分析和信息收集，找到最优的解决方案。

### 2.1 问题描述的三个层次

问题描述有三个层次，AI 应该深入到本质层来理解问题：

| 层次 | 描述内容 | 示例 |
|------|---------|------|
| **现象层** | 观察到的症状 | "递归导致栈溢出" |
| **机制层** | 问题发生的原因 | "poll_next 被重复调用形成调用链" |
| **本质层** | 系统的不变量和约束 | "迭代器顺序访问"、"数据流的依赖关系" |

**关键原则**：不要停留在现象层和机制层寻找解决方案，要深入到本质层理解系统的约束。

### 2.2 方案评估优先级

优先考虑以下顺序的方案：

| 优先级 | 方案类型 | 说明 | 示例 |
|--------|---------|------|------|
| 1 | **领域特定方案** | 利用问题本身的约束和语义 | 利用"迭代器顺序访问"设计令牌机制 |
| 2 | **预防性方案** | 从根本上避免问题发生 | 在入口处控制，而非在调用链中间打断 |
| 3 | **通用技术方案** | 套用已知模式 | 蹦床模式、显式栈、深度检测等 |

**关键原则**：
- 问自己：这个问题有什么特殊性可以利用？
- 问自己：能否让问题根本不发生，而不是发生后处理？
- 通用技术方案通常不是最优解，应作为最后选择

### 2.3 方案设计前的必问清单

**强制规则：先问后答**

在提出任何技术方案之前，AI 必须：
1. 检查以下必问清单
2. 对不确定的项目主动询问用户
3. 等待用户回答后再提出方案

#### 2.3.1 数据/资源的访问模式

必须确认：
- [ ] 相关数据结构的访问是顺序的还是可并行的？
- [ ] 是否有独占资源（同一时刻只能一个访问者）？
- [ ] 数据的生产者和消费者关系是什么？

示例问题：
> "这个迭代器/数据结构是顺序访问还是可以并行访问？"
> "这个资源是否可以被多个线程同时访问？"

#### 2.3.2 调用关系和控制流

必须确认：
- [ ] 各组件之间的调用关系是什么？
- [ ] 是否存在回调或反向调用？
- [ ] 调用是同步的还是可能异步的？

示例问题：
> "组件 A 完成处理后会调用什么？是否会回调到组件 B？"
> "中间是否可能有异步操作打断调用链？"

#### 2.3.3 系统不变量（Invariants）

不变量是系统中**必须始终保持为真**的条件。必须确认：
- [ ] 有哪些条件必须始终保持为真？
- [ ] 什么操作不能同时发生？
- [ ] 什么顺序必须保证？

示例问题：
> "是否有任何操作必须互斥执行？"
> "元素的处理顺序是否重要？"
> "什么东西不能同时发生？"

#### 2.3.4 约束条件（非功能需求）

必须确认：
- [ ] 性能要求：是否需要无锁？是否允许额外内存分配？
- [ ] 依赖限制：是否可以依赖外部组件（如调度器）？
- [ ] 兼容性：是否需要兼容现有的异步操作？

### 2.4 重构/优化的语义等价原则

当用户要求“重构、优化、收敛、简化、合并”代码时，首要目标是**语义等价**。不改变行为，是优先级最高的约束。

**通用规则**：
- 保持可观察行为一致（输出、顺序、异常路径、回调/副作用的发生条件）。
- 失败路径/回退路径同样是语义的一部分，不能弱化或省略。
- 任何“必达”的副作用在新实现中仍必须必达。
- 若无法证明语义不变，应拆分为可验证的小步或先询问用户。

### 2.5 并发问题的思考框架

当遇到并发/递归问题时，按以下顺序思考：

**问题1：同一时刻需要多少个执行流？**
- 如果答案是"只需要一个" → 考虑令牌/互斥机制
- 如果答案是"可以多个" → 考虑无锁数据结构

**问题2：其他执行流需要做什么？**
- 如果只需要"登记需求" → 用计数器
- 如果需要"等待结果" → 用 future/promise

**问题3：如何保证进展？**
- 令牌持有者负责处理所有 pending 请求
- 用循环替代递归

### 2.6 禁止行为

- ❌ **不确定时假设答案**：即使看起来"显而易见"，也要确认
- ❌ **跳过询问直接给方案**：必须先收集足够信息
- ❌ **停留在现象层**：要深入到本质层理解问题
- ❌ **优先使用通用方案**：先考虑领域特定方案

### 2.7 问题描述模板（供用户参考）

用户在描述问题时，可以参考以下模板提供信息：

```
## 问题描述

### 系统结构和数据流
- 涉及哪些组件/算子？
- 数据如何在它们之间流动？
- 调用关系是什么？

### 系统不变量
- 什么必须始终为真？
- 什么不能同时发生？

### 期望 vs 实际
- 期望：系统应该如何工作？
- 实际：系统实际如何工作？
- 差异：哪里出了问题？

### 约束条件
- 性能要求
- 依赖限制
- 兼容性要求

### 已知洞察（如有）
- 任何可能有助于解决问题的观察或想法
```

---

## 3. Core Concepts & Type Definitions (核心概念与类型)

### 3.1 Sender
```cpp
// Sender是延迟执行的操作描述，在被connect并启动前不执行任何操作
// 典型结构：
template <typename prev_t, typename func_t>
struct sender {
    prev_t prev;           // 前一个sender
    func_t func;           // 当前操作的函数
    
    // 生成操作对象
    auto make_op() { return op<func_t>(std::move(func)); }
    
    // 连接到操作列表
    template<typename context_t>
    auto connect() {
        return prev.template connect<context_t>().push_back(make_op());
    }
};
```

### 3.2 Context (上下文)
```cpp
// 生命周期：通过shared_ptr管理
// 创建方式：make_root_context() 返回 shared_ptr<root_context<...>>

// root_context - 根上下文
template <typename ...content_t>
struct root_context {
    std::tuple<content_t...> datas;  // 存储的数据
    // 通过 make_root_context(data1, data2, ...) 创建
};

// pushed_context - 推入的上下文，链接到基础上下文
template <uint64_t id, typename base_t, typename ...content_t>
struct pushed_context {
    base_t base_context;              // 基础上下文引用
    std::tuple<content_t...> datas;   // 当前层数据
};

// 从context中获取数据
auto& data = _get<need_type, 0, context_t>(context);
```

### 3.3 Scope (作用域)
```cpp
// 生命周期：通过shared_ptr管理
// 作用：管理Op列表的内存和层级关系

enum struct runtime_scope_type {
    root,            // 根作用域
    stream_starter,  // 流启动器作用域
    other            // 其他作用域
};

// root_scope - 根作用域
template <typename op_tuple_t>
struct root_scope {
    op_tuple_t op_tuple;  // 操作元组
};

// runtime_scope - 运行时作用域，链接到基础作用域
template <runtime_scope_type type, typename op_tuple_t, typename base_t>
struct runtime_scope {
    op_tuple_t op_tuple;
    base_t base_scope;    // 基础作用域引用
};

// 创建方式：make_runtime_scope<scope_type>(base_scope, op_tuple)
```

### 3.4 Op & OpPusher (操作与推进器)
```cpp
// Op - 操作对象，包含执行逻辑
template <typename func_t>
struct op {
    func_t func;
    constexpr static bool then_op = true;  // 类型标记
};

// OpPusher - 执行推进器，通过模板特化处理不同类型的Op
template<uint32_t pos, typename scope_t>
struct op_pusher {
    // 推送值到下一个操作
    template<typename context_t, typename ...value_t>
    static void push_value(context_t& context, scope_t& scope, value_t&& ...v);
    
    // 传播异常
    template<typename context_t>
    static void push_exception(context_t& context, scope_t& scope, std::exception_ptr e);
    
    // 跳过信号
    template<typename context_t>
    static void push_skip(context_t& context, scope_t& scope);
    
    // 完成信号
    template<typename context_t>
    static void push_done(context_t& context, scope_t& scope);
};
```

### 3.5 生命周期管理总结
| 类型 | 创建方式 | 管理方式 | 销毁时机 |
|------|---------|---------|---------|
| Context | `make_root_context(...)` | `shared_ptr` | 引用计数归零 |
| Scope | `make_runtime_scope<type>(...)` | `shared_ptr` | 引用计数归零 |
| Op | `sender.make_op()` | 值语义，存储在tuple中 | 随Scope销毁 |
| Sender | 直接构造 | 值语义 | connect后不再需要 |

---

## 4. Coding Patterns (必读：代码范式)

### 4.0 General Syntax Rules (通用语法规则)

在编写PUMP流水线代码之前，请牢记以下通用语法规则：

#### 4.0.1 Lambda参数优先使用泛型

在Sender/Receiver库中，Lambda参数应**优先使用泛型** `auto&&`，除非需要明确类型转换：

```cpp
// ✅ 推荐写法：使用泛型参数
>> then([](auto&& val) {
    // val的类型由编译器自动推导
    return process(std::forward<decltype(val)>(val));
})

// ✅ 多参数时同样使用泛型
>> then([](auto&& a, auto&& b) {
    return combine(std::forward<decltype(a)>(a), 
                   std::forward<decltype(b)>(b));
})

// ⚠️ 仅在需要明确类型转换时使用具体类型
>> then([](int val) {  // 明确需要int类型
    return val * 2;
})

// ⚠️ 或在需要类型约束时
>> then([](const std::string& s) {  // 明确需要string引用
    return s.size();
})
```

**原因**：PUMP的类型推导系统依赖模板，使用泛型参数可以：
- 避免不必要的类型转换和拷贝
- 保持完美转发语义
- 减少编译错误

#### 4.0.2 不可拷贝对象必须使用std::move

在Pipeline中传递不可拷贝对象（如`unique_ptr`、`unique_lock`等）时，**必须使用`std::move`**：

```cpp
// ✅ 正确写法：使用std::move传递unique_ptr
auto pipeline = just(std::make_unique<Resource>())
    >> then([](auto&& ptr) {
        ptr->use();
        return std::move(ptr);  // 必须move出去
    })
    >> then([](auto&& ptr) {
        ptr->finalize();
    });

// ❌ 错误写法：忘记move导致编译错误
auto bad_pipeline = just(std::make_unique<Resource>())
    >> then([](auto ptr) {  // 错误：unique_ptr不可拷贝
        ptr->use();
        return ptr;
    });

// ✅ 在lambda捕获中也要注意move
auto resource = std::make_unique<Data>();
auto pipeline = just()
    >> then([r = std::move(resource)]() mutable {  // 捕获时move
        r->process();
        return std::move(r);  // 返回时也要move
    });
```

**关键点**：
- `just(std::move(obj))` - 将对象移入pipeline起点
- Lambda返回不可拷贝对象时使用 `return std::move(obj);`
- Lambda捕获不可拷贝对象时使用 `[obj = std::move(obj)]` 并标记 `mutable`

---

### 模式1：发起异步任务

**场景**：创建并启动一个异步操作流水线

```cpp
#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/submit.hh"

using namespace pump::sender;
using namespace pump::core;

// 正确示例：创建context，构建pipeline，提交执行
void start_async_task() {
    // 1. 创建根上下文（可携带共享数据）
    auto context = make_root_context();
    
    // 2. 构建sender pipeline
    auto pipeline = just(42)                          // 起始值
        >> then([](int x) { return x * 2; })          // 同步变换
        >> then([](int x) { 
            std::cout << "Result: " << x << std::endl; 
        });
    
    // 3. 提交执行
    pipeline >> submit(context);
}

// 带返回值的异步任务（使用协程）
pump::coro::task<int> async_compute() {
    auto result = co_await (
        just(10, 20)
        >> then([](int a, int b) { return a + b; })
        >> await_able()  // 转换为awaitable
    );
    co_return result;  // 返回30
}
```

### 模式2：并发控制

**场景**：并发处理多个任务，等待全部完成

```cpp
#include "pump/sender/when_all.hh"
#include "pump/sender/for_each.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/reduce.hh"

using namespace pump::sender;

// 模式2a：when_all - 等待多个独立sender完成
auto wait_multiple_tasks() {
    return just()
        >> when_all(
            just(1) >> then([](int x) { return x * 10; }),   // task1
            just(2) >> then([](int x) { return x * 20; }),   // task2
            just(3) >> then([](int x) { return x * 30; })    // task3
        )
        >> then([](auto&& results) {
            // results是tuple<variant<monostate, exception_ptr, int>, ...>
            // 处理聚合结果
        });
}

// 模式2b：for_each + concurrent - 并发处理流式数据
auto process_items_concurrently(std::vector<int>& items) {
    return for_each(items)                    // 将vector转为流
        >> concurrent(4)                       // 最大并发数4
        >> then([](int item) {                 // 并发处理每个元素
            return expensive_compute(item);
        })
        >> reduce();                           // 收集所有结果
}

// 模式2c：with_concurrent - 并发处理后回到顺序流
auto process_with_order_preserved(std::vector<int>& items) {
    return for_each(items)
        >> with_concurrent(
            then([](int item) { return async_process(item); })
            >> flat()
        );  // 内部并发，外部保持顺序
}
```

### 模式3：错误处理

**场景**：捕获和处理异步操作中的异常

```cpp
#include "pump/sender/any_exception.hh"
#include "pump/sender/then.hh"

using namespace pump::sender;

// 模式3a：any_exception - 捕获所有异常
auto with_error_recovery() {
    return just()
        >> then([]() {
            // 可能抛出异常的操作
            if (some_condition) {
                throw std::runtime_error("Operation failed");
            }
            return 42;
        })
        >> any_exception([](std::exception_ptr e) {
            // 异常恢复：返回一个新的sender继续执行
            return just(-1);  // 返回默认值
        });
}

// 模式3b：catch_exception<T> - 捕获特定类型异常
auto with_specific_error_handling() {
    return just()
        >> then([]() -> int {
            throw custom_error("specific error");
        })
        >> catch_exception<custom_error>([](custom_error& e) {
            log_error(e);
            return just(default_value);
        })
        >> any_exception([](std::exception_ptr) {
            // 处理其他未捕获的异常
            return just(fallback_value);
        });
}

// 模式3c：在then中抛出异常（异常会自动传播）
auto propagate_exception() {
    return just(value)
        >> then([](auto v) {
            if (!validate(v)) {
                throw validation_error("Invalid value");
            }
            return process(v);
        })
        // 异常会通过push_exception传播到后续的异常处理器
        >> any_exception([](std::exception_ptr e) {
            return just() >> forward_value(error_result);
        });
}
```

### 模式4：资源清理与RAII

**场景**：管理异步操作中的资源生命周期

```cpp
#include "pump/sender/push_context.hh"
#include "pump/sender/pop_context.hh"
#include "pump/sender/get_context.hh"

using namespace pump::sender;

// 模式4a：使用Context传递资源（推荐）
auto with_resource_context() {
    return just()
        >> push_context(std::make_unique<Resource>())  // 资源入栈
        >> then([]() {
            // 操作...
        })
        >> get_context<std::unique_ptr<Resource>>()    // 获取资源
        >> then([](std::unique_ptr<Resource>& res) {
            res->do_something();
        })
        >> pop_context();  // 资源出栈（自动销毁）
}

// 模式4b：使用with_context简化资源管理
auto with_scoped_resource() {
    auto resource = std::make_shared<Resource>();
    
    return just()
        >> with_context(resource)(
            // 在此作用域内可通过get_context访问resource
            get_context<std::shared_ptr<Resource>>()
            >> then([](std::shared_ptr<Resource>& r) {
                r->process();
            })
        );
    // with_context结束后自动pop
}

// 模式4c：RAII包装器用于异步清理
struct AsyncResourceGuard {
    Resource* res;
    ~AsyncResourceGuard() {
        // 注意：析构函数中不能执行异步操作
        // 同步清理
        res->sync_cleanup();
    }
};

auto with_raii_guard() {
    return just()
        >> then([]() {
            auto guard = std::make_unique<AsyncResourceGuard>(acquire_resource());
            return guard;
        })
        >> then([](auto guard) {
            guard->res->use();
            // guard在lambda结束时自动销毁
        });
}
```

### 模式5：调度器切换

**场景**：在不同执行域之间切换

```cpp
#include "pump/sender/on.hh"
#include "pump/sender/flat.hh"

using namespace pump::sender;

// 模式5a：使用on切换到指定调度器
auto switch_scheduler() {
    return just(data)
        >> then([](auto d) {
            // 当前在调用者线程
            return prepare(d);
        })
        >> on(io_scheduler.as_task())  // 切换到IO调度器
        >> then([](auto d) {
            // 现在在IO线程执行
            return do_io_operation(d);
        })
        >> on(compute_scheduler.as_task())  // 切换到计算调度器
        >> then([](auto result) {
            // 在计算线程处理结果
            return process_result(result);
        });
}

// 模式5b：flat_map中切换调度器
auto async_with_scheduler_switch() {
    return just(request)
        >> then([](auto req) {
            // 返回一个在特定调度器上执行的sender
            return nvme_scheduler.read_page(req.page_id);
        })
        >> flat()  // 展开内部sender
        >> then([](auto page_data) {
            return process_page(page_data);
        });
}
```

---

## 5. Anti-Patterns (禁忌事项)

### ❌ 禁忌1：在异步回调中执行阻塞操作

```cpp
// ❌ 错误写法：在then中执行阻塞IO
auto bad_blocking() {
    return just()
        >> then([]() {
            std::this_thread::sleep_for(std::chrono::seconds(1));  // 阻塞！
            auto data = blocking_read_file("data.txt");            // 阻塞！
            return data;
        });
}

// ✅ 修正写法：使用异步操作或切换到专用调度器
auto good_async() {
    return just()
        >> then([](){ return file_path; })
        >> on(io_scheduler.as_task())      // 切换到IO调度器
        >> then([](auto path) {
            return async_read_file(path);  // 使用异步API
        })
        >> flat();
}
```

### ❌ 禁忌2：捕获局部变量的引用

```cpp
// ❌ 错误写法：lambda捕获局部变量引用
auto bad_capture() {
    int local_value = 42;
    return just()
        >> then([&local_value]() {  // 危险！local_value可能已销毁
            return local_value * 2;
        });
}

// ✅ 修正写法：值捕获或使用Context
auto good_capture_by_value() {
    int local_value = 42;
    return just()
        >> then([local_value]() {  // 值捕获，安全
            return local_value * 2;
        });
}

// ✅ 修正写法：使用Context传递
auto good_capture_with_context() {
    return just()
        >> push_context(42)
        >> get_context<int>()
        >> then([](int& value) {  // 从context获取，生命周期由context管理
            return value * 2;
        })
        >> pop_context();
}
```

### ❌ 禁忌3：在then中吞掉异常

```cpp
// ❌ 错误写法：在then中catch异常但不传播
auto bad_exception_handling() {
    return just()
        >> then([]() {
            try {
                risky_operation();
            } catch (...) {
                // 异常被吞掉，后续操作不知道发生了错误
                return default_value;
            }
            return result;
        });
}

// ✅ 修正写法：让异常传播，使用any_exception处理
auto good_exception_handling() {
    return just()
        >> then([]() {
            risky_operation();  // 异常会自动通过push_exception传播
            return result;
        })
        >> any_exception([](std::exception_ptr e) {
            log_exception(e);
            return just(default_value);  // 统一的错误恢复点
        });
}
```

### ❌ 禁忌4：忘记flat展开返回的sender

```cpp
// ❌ 错误写法：then返回sender但没有flat
auto bad_no_flat() {
    return just(key)
        >> then([](auto k) {
            return async_lookup(k);  // 返回sender，但没有展开！
        });
    // 结果类型是sender，而不是lookup的结果
}

// ✅ 修正写法：使用flat或flat_map
auto good_with_flat() {
    return just(key)
        >> then([](auto k) {
            return async_lookup(k);
        })
        >> flat();  // 展开内部sender
}

// ✅ 更简洁：使用flat_map
auto good_with_flat_map() {
    return just(key)
        >> flat_map([](auto k) {
            return async_lookup(k);
        });
}
```

### ❌ 禁忌5：在并发操作中共享可变状态

```cpp
// ❌ 错误写法：并发操作共享可变状态
auto bad_shared_state() {
    int counter = 0;  // 共享可变状态
    return for_each(items)
        >> concurrent(4)
        >> then([&counter](auto item) {  // 数据竞争！
            counter++;
            return process(item);
        })
        >> reduce();
}

// ✅ 修正写法：使用reduce进行聚合
auto good_reduce() {
    return for_each(items)
        >> concurrent(4)
        >> then([](auto item) {
            return process(item);  // 无共享状态
        })
        >> reduce(0, [](int acc, auto result) {
            return acc + 1;  // 在reduce中安全聚合
        });
}

// ✅ 修正写法：使用原子操作（如果必须共享）
auto good_atomic() {
    auto counter = std::make_shared<std::atomic<int>>(0);
    return for_each(items)
        >> concurrent(4)
        >> then([counter](auto item) {
            counter->fetch_add(1);  // 原子操作
            return process(item);
        })
        >> reduce();
}
```

### ❌ 禁忌6：混淆submit和await_able

```cpp
// ❌ 错误写法：在协程中使用submit（无法获取结果）
pump::coro::task<int> bad_submit_in_coro() {
    auto ctx = make_root_context();
    just(42) >> then([](int x) { return x * 2; }) >> submit(ctx);
    // submit是fire-and-forget，无法获取结果！
    co_return 0;  // 错误的返回值
}

// ✅ 修正写法：使用await_able获取结果
pump::coro::task<int> good_await_in_coro() {
    auto result = co_await (
        just(42)
        >> then([](int x) { return x * 2; })
        >> await_able()
    );
    co_return result;  // 正确返回84
}
```

### ❌ 禁忌7：submit的Context生命周期不安全

**核心警告**：`submit` 是**非阻塞的（Fire-and-forget）**，调用后立即返回，异步任务在后台执行。

```cpp
// ❌ 错误写法：传递局部栈变量Context的引用
void dangerous_local_context() {
    auto local_ctx = make_root_context(some_data);  // 局部变量
    
    just(42)
        >> then([](int x) { return x * 2; })
        >> submit(local_ctx);  // 危险！
    
    // 函数返回后local_ctx被销毁
    // 但异步任务可能还在运行，访问已销毁的context！
}

// ❌ 错误写法：在作用域结束前context可能被销毁
void dangerous_scope() {
    {
        auto ctx = make_root_context();
        just() >> long_running_task() >> submit(ctx);
    }  // ctx在这里销毁，但任务可能还在运行！
    
    do_other_work();  // 异步任务可能正在访问已销毁的ctx
}

// ✅ 修正写法1：使用shared_ptr确保生命周期
void safe_shared_context() {
    auto ctx = make_root_context(some_data);  // 已经是shared_ptr
    
    just(42)
        >> then([](int x) { return x * 2; })
        >> then([ctx](int result) {  // 捕获ctx延长生命周期
            // 使用result
        })
        >> submit(ctx);
    
    // 即使函数返回，ctx的引用计数>0，不会被销毁
}

// ✅ 修正写法2：确保调用者作用域足够长
void safe_long_lived_scope() {
    auto ctx = make_root_context();
    
    just() >> some_task() >> submit(ctx);
    
    // 阻塞等待或确保作用域持续到任务完成
    wait_for_completion();  // 或使用其他同步机制
}

// ✅ 修正写法3：在长生命周期对象中持有context
class Service {
    std::shared_ptr<root_context<Config>> ctx_;
public:
    void start() {
        ctx_ = make_root_context(config_);
        just() >> run_service() >> submit(ctx_);
        // ctx_的生命周期与Service对象绑定
    }
};
```

**关键规则**：
1. **永远不要**传递即将销毁的局部Context给`submit`
2. **推荐**在lambda中捕获context以延长其生命周期
3. **推荐**将context存储在长生命周期的对象中
4. 如果需要等待结果，使用`await_able()`而非`submit`

### ❌ 禁忌8：通过给 env/pump 库代码加锁来解决任何错误

**核心警告**：遇到**任何错误**（包括但不限于内存错误、数据竞争、逻辑错误等）时，**禁止**通过给 `env` 或 `pump` 库下的代码加锁来"修复"问题。

**适用范围**：
- `src/env/` 目录下的所有代码（scheduler、runtime 等）
- `src/pump/` 目录下的所有代码（sender、core、coro 等）

#### 错误的调试思路

```cpp
// ❌ 错误做法：看到任何并发相关的错误，就给 env/pump 库代码加锁
// 这违反了 PUMP 框架的无锁设计原则！

// 错误示例1：给 sender 算子加互斥锁
template<typename func_t>
struct op {
    std::mutex mtx;  // ❌ 禁止！
    
    void execute() {
        std::lock_guard<std::mutex> lock(mtx);  // ❌ 禁止！
        // ...
    }
};

// 错误示例2：给 scheduler 加锁
class scheduler {
    std::mutex mtx;  // ❌ 禁止！
    
    void advance() {
        std::lock_guard<std::mutex> lock(mtx);  // ❌ 禁止！
        // ...
    }
};
```

#### 正确的调试思路

错误通常是**使用方式**的问题，而非框架本身的问题。正确的调试步骤：

1. **Step 1**：确认错误的具体位置（哪个函数、哪行代码）
2. **Step 2**：追溯调用链，找到是谁调用了这段代码
3. **Step 3**：检查调用方是否违反了以下约束：
   - 是否有多个线程在操作同一个 scheduler？（违反 Single-Thread Scheduler 原则）
   - 是否有共享状态被多线程访问？
   - scheduler 的 `advance()` 是否被多线程调用？
4. **Step 4**：只有在确认框架本身有 bug 时，才修改框架代码

#### 常见根因示例

```cpp
// ❌ 错误：同一个 scheduler 被多个线程运行
void bad_scheduler_usage() {
    auto* scheduler = new task_scheduler(0);
    
    // 线程1运行scheduler
    std::thread([scheduler]() {
        while (true) scheduler->advance();
    }).detach();
    
    // 线程2也运行同一个scheduler - 这会导致内存错误！
    while (true) scheduler->advance();
}

// ✅ 正确：每个 scheduler 只被一个线程运行
void good_scheduler_usage() {
    auto* scheduler1 = new task_scheduler(0);
    auto* scheduler2 = new task_scheduler(1);
    
    // 线程1运行scheduler1
    std::thread([scheduler1]() {
        while (true) scheduler1->advance();
    }).detach();
    
    // 主线程运行scheduler2
    while (true) scheduler2->advance();
}
```

#### 红线规则

| 禁止操作 | 说明 |
|---------|------|
| ❌ 给 `src/pump/` 下的任何代码加锁 | sender、core、coro 等都是无锁设计 |
| ❌ 给 `src/env/` 下的任何代码加锁 | scheduler、runtime 等都是单线程执行设计 |
| ❌ 通过加锁来"修复"任何错误 | 无论是内存错误、数据竞争还是逻辑错误 |
| ❌ 不追溯根因就修改框架核心代码 | 可能掩盖真正的问题 |

| 正确做法 | 说明 |
|---------|------|
| ✅ 首先检查 scheduler 是否被多线程运行 | 最常见的根因 |
| ✅ 检查是否有共享状态被并发访问 | 应使用原子操作或无锁队列 |
| ✅ 追溯调用链找到真正的根因 | 在修改代码前理解问题本质 |
| ✅ 在应用层代码中解决问题 | 而非修改 env/pump 库代码 |

---

## 6. KV Store Scenario Application (场景模拟：KV存储)

### 6.0 Advanced Branching (高级分支处理)

#### 静态流中的动态逻辑

PUMP的Pipeline是**静态编译**的，这意味着整个流水线的类型在编译期就已确定。当Sender可能返回不同类型的结果时（例如`IndexHit` vs `IndexMiss`），不能使用普通的运行时`if/else`来处理类型差异。

**问题场景**：
```cpp
// ❌ 错误写法：运行时if/else无法处理不同类型
auto bad_branching() {
    return index_lookup(key)
        >> then([](auto result) {
            if (result.found) {
                return read_data(result.location);  // 返回类型A
            } else {
                return just(empty_value);           // 返回类型B - 编译错误！
            }
        });
}
```

#### 解决方案：std::variant + if constexpr

使用`std::variant`包装可能的返回类型，配合**`if constexpr`**在下游的`then`中进行编译期类型分发：

```cpp
// ✅ 正确写法：使用variant统一返回类型
struct IndexHit { data_location loc; };
struct IndexMiss { };
using LookupResult = std::variant<IndexHit, IndexMiss>;

auto good_branching() {
    return index_lookup(key)  // 返回 LookupResult
        >> then([](LookupResult&& result) {
            // 使用std::visit或在后续then中用if constexpr处理
            return std::visit([](auto&& r) {
                using T = std::decay_t<decltype(r)>;
                if constexpr (std::is_same_v<T, IndexHit>) {
                    return just() >> read_data(r.loc);
                } else {
                    return just(empty_value);
                }
            }, result);
        })
        >> flat();  // 展开内部sender
}
```

#### 使用visit算子进行类型提升

PUMP提供了`visit`算子，可以将运行时的条件值（bool/指针/variant）提升为编译期的类型分支：

```cpp
// ✅ 使用visit算子：将运行时值提升为编译期类型
auto with_visit() {
    return index_lookup(key)
        >> visit()  // 将variant的运行时选择提升为编译期类型
        >> then([](auto&& result) {
            // 这里可以使用if constexpr，因为result的类型在编译期已知
            using T = std::decay_t<decltype(result)>;
            if constexpr (std::is_same_v<T, IndexHit>) {
                return just() >> read_data(result.loc);
            } else if constexpr (std::is_same_v<T, IndexMiss>) {
                return just(empty_value);
            }
        })
        >> flat();
}

// visit也支持bool类型
auto bool_visit() {
    return check_condition()
        >> visit()  // bool -> std::true_type 或 std::false_type
        >> then([](auto flag) {
            if constexpr (std::is_same_v<decltype(flag), std::true_type>) {
                return handle_true_case();
            } else {
                return handle_false_case();
            }
        })
        >> flat();
}
```

#### 关键要点

| 技术 | 适用场景 | 说明 |
|-----|---------|------|
| `std::variant` | 多种可能的返回类型 | 统一包装不同类型，延迟到下游处理 |
| `if constexpr` | 编译期类型分支 | 在`then`的lambda中根据类型选择不同逻辑 |
| `visit`算子 | 运行时值→编译期类型 | 将variant/bool/指针提升为类型标签 |
| `flat()` | 展开分支返回的sender | 每个分支返回sender时必须flat |

---

### 综合示例：实现异步Get操作

以下代码展示如何组合PUMP API实现一个完整的KV存储Get操作：

```cpp
#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/for_each.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/forward_value.hh"
#include "pump/sender/on.hh"

using namespace pump::sender;

namespace kv {

// 数据结构定义
struct key_value {
    std::string key;
    std::string value;
    bool found = false;
};

struct data_page {
    uint64_t page_id;
    // ... 页面数据
};

struct batch {
    uint64_t snapshot_id;
    // ... 批处理上下文
};

// ============================================
// 步骤1：从NVMe并发读取数据页
// ============================================
inline auto read_pages(std::vector<data_page*>& pages) {
    return for_each(pages)                    // 将页面列表转为流
        >> concurrent(4)                       // 最大4个并发IO
        >> then([](data_page* page) {
            // 返回NVMe读取sender
            return nvme_scheduler.read_page(page->page_id);
        })
        >> flat()                              // 展开NVMe sender
        >> reduce();                           // 等待所有页面读取完成
}

// ============================================
// 步骤2：在索引调度器上执行查找
// ============================================
inline auto on_index_scheduler() {
    return then([](auto&& ...args) {
        // 切换到索引调度器，并携带当前参数
        return index_scheduler.as_task() 
            >> forward_value(std::forward<decltype(args)>(args)...);
    })
    >> flat();
}

// ============================================
// 步骤3：主Get逻辑 - 组合所有步骤
// ============================================
inline auto get_impl(batch* b, const char* key) {
    // 第一步：在索引中查找key
    return index_lookup(key, b->snapshot_id)
        // 第二步：根据查找结果类型分支处理
        >> then([b](auto&& result) {
            // 使用if constexpr根据结果类型选择不同的处理路径
            
            if constexpr (std::is_same_v<decltype(result), index_hit*>) {
                // 情况A：索引命中，需要读取数据页
                return just()
                    >> then([result]() { 
                        return result->pages;  // 获取需要读取的页面列表
                    })
                    >> flat_map([](auto& pages) {
                        return read_pages(pages);  // 并发读取页面
                    })
                    >> on_index_scheduler()        // 切回索引调度器
                    >> then([result]() {
                        // 从页面数据构造key_value
                        auto kv = key_value{
                            .key = result->key,
                            .value = extract_value(result->pages),
                            .found = true
                        };
                        delete result;  // 清理临时结果
                        return kv;
                    });
            }
            else if constexpr (std::is_same_v<decltype(result), index_miss>) {
                // 情况B：索引未命中
                return just()
                    >> forward_value(key_value{.found = false});
            }
            else {
                // 情况C：其他错误
                return just(std::make_exception_ptr(
                    std::runtime_error("Unexpected result type")
                ));
            }
        })
        >> flat()  // 展开分支返回的sender
        // 第三步：统一的异常处理
        >> any_exception([](std::exception_ptr e) {
            // 记录错误日志
            try {
                std::rethrow_exception(e);
            } catch (const std::exception& ex) {
                log_error("Get failed: ", ex.what());
            }
            // 返回空结果而不是传播异常
            return just() >> forward_value(key_value{.found = false});
        });
}

// ============================================
// 步骤4：公开API - 从Context获取batch
// ============================================
inline auto get() {
    return get_context<batch*>()              // 从context获取batch指针
        >> then([](batch* b, std::string&& key) {
            return get_impl(b, key.c_str());
        })
        >> flat();
}

// 便捷重载：直接传入key
inline auto get(std::string&& key) {
    return forward_value(std::move(key)) >> get();
}

// ============================================
// 使用示例
// ============================================
void example_usage() {
    auto context = make_root_context();
    
    // 创建batch并推入context
    auto batch_ptr = create_batch();
    
    just()
        >> push_context(batch_ptr)
        // 执行Get操作
        >> forward_value(std::string("my_key"))
        >> get()
        >> then([](key_value&& kv) {
            if (kv.found) {
                std::cout << "Found: " << kv.value << std::endl;
            } else {
                std::cout << "Key not found" << std::endl;
            }
        })
        >> pop_context()
        >> submit(context);
}

// 协程版本
pump::coro::task<key_value> coro_get(const std::string& key) {
    auto ctx = make_root_context();
    auto batch_ptr = create_batch();
    
    auto result = co_await (
        just()
        >> push_context(batch_ptr)
        >> forward_value(std::string(key))
        >> get()
        >> await_able(ctx)
    );
    
    co_return result;
}

} // namespace kv
```

### 关键设计要点总结

| 要点 | 说明 |
|-----|------|
| **流水线组合** | 使用 `>>` 将多个sender串联，保持逻辑线性 |
| **并发IO** | `for_each >> concurrent >> flat >> reduce` 模式处理批量IO |
| **调度器切换** | 使用 `on()` 或返回特定调度器的sender + `flat()` |
| **类型分支** | 在 `then` 中使用 `if constexpr` 根据类型选择不同路径 |
| **异常处理** | 使用 `any_exception` 在流水线末端统一处理错误 |
| **上下文传递** | 使用 `push_context/get_context` 传递跨步骤状态 |
| **资源清理** | 在 `then` 中手动delete或使用智能指针 |

---

## 附录：常用Sender速查表

| Sender | 用途 | 输入 | 输出 |
|--------|------|------|------|
| `just(...)` | 起始值 | 任意值 | 传入的值 |
| `then(f)` | 同步变换 | 上一步值 | f的返回值 |
| `flat()` | 展开sender | sender | sender的输出 |
| `flat_map(f)` | transform+flat | 上一步值 | f返回sender的输出 |
| `for_each(range)` | 流式化 | 可迭代范围 | 流式元素 |
| `concurrent(n)` | 并发处理 | 流式元素 | 并发结果 |
| `reduce()` | 聚合 | 流式结果 | 聚合值 |
| `when_all(...)` | 并行等待 | 多个sender | 结果元组 |
| `any_exception(f)` | 异常处理 | exception_ptr | f返回sender的输出 |
| `push_context(...)` | 入栈数据 | 数据 | 不变 |
| `get_context<T>()` | 获取数据 | 无 | T& + 当前值 |
| `pop_context()` | 出栈 | 无 | 不变 |
| `on(sender)` | 调度切换 | 当前值 | 目标sender输出 |
| `await_able()` | 协程桥接 | sender | awaitable |
| `submit(ctx)` | 启动执行 | sender | 无（副作用） |

---

## 7. Header File Reference (头文件速查表)

> **重要**：以下映射基于实际代码库扫描，请严格按照此表引用头文件，避免猜测路径。

### 7.1 Sender API 头文件

#### 基础与提交
| API | 头文件路径 |
|-----|-----------|
| `just(...)` | `pump/sender/just.hh` |
| `submit(context)` | `pump/sender/submit.hh` |

#### 变换与展开
| API | 头文件路径 |
|-----|-----------|
| `then(f)` | `pump/sender/then.hh` |
| `transform(f)` | `pump/sender/then.hh` |
| `forward_value(...)` | `pump/sender/then.hh` |
| `ignore_args()` | `pump/sender/then.hh` |
| `ignore_results()` | `pump/sender/then.hh` |
| `just_exception(e)` | `pump/sender/then.hh` |
| `then_exception(e)` | `pump/sender/then.hh` |
| `false_to_exception(exp)` | `pump/sender/then.hh` |
| `flat()` | `pump/sender/flat.hh` |
| `flat_map(f)` | `pump/sender/flat.hh` |

#### 流式生产
| API | 头文件路径 |
|-----|-----------|
| `for_each(range)` | `pump/sender/for_each.hh` |
| `generate(range)` | `pump/sender/generate.hh` |
| `stream(range)` | `pump/sender/generate.hh` |
| `range(range)` | `pump/sender/generate.hh` |
| `loop(n)` | `pump/sender/generate.hh` |
| `repeat(n)` | `pump/sender/repeat.hh` |
| `forever()` | `pump/sender/repeat.hh` |

#### 并发与顺序
| API | 头文件路径 |
|-----|-----------|
| `concurrent(max)` | `pump/sender/concurrent.hh` |
| `sequential()` | `pump/sender/sequential.hh` |
| `with_concurrent(bind_back)` | `pump/sender/sequential.hh` |

#### 聚合与归约
| API | 头文件路径 |
|-----|-----------|
| `when_all(...)` | `pump/sender/when_all.hh` |
| `reduce(...)` | `pump/sender/reduce.hh` |
| `to_container<T>()` | `pump/sender/reduce.hh` |
| `to_vector<T>()` | `pump/sender/reduce.hh` |

#### 异常处理
| API | 头文件路径 |
|-----|-----------|
| `any_exception(f)` | `pump/sender/any_exception.hh` |
| `catch_exception<T>(f)` | `pump/sender/any_exception.hh` |
| `ignore_all_exception()` | `pump/sender/any_exception.hh` |
| `ignore_inner_exception(bb)` | `pump/sender/any_exception.hh` |

#### 控制与条件
| API | 头文件路径 |
|-----|-----------|
| `maybe()` | `pump/sender/maybe.hh` |
| `maybe_not()` | `pump/sender/maybe.hh` |
| `when_skipped(f)` | `pump/sender/when_skipped.hh` |
| `visit()` | `pump/sender/visit.hh` |

#### 调度切换
| API | 头文件路径 |
|-----|-----------|
| `on(sender)` | `pump/sender/on.hh` |

#### 上下文操作
| API | 头文件路径 |
|-----|-----------|
| `push_context(...)` | `pump/sender/push_context.hh` |
| `push_result_to_context()` | `pump/sender/push_context.hh` |
| `pop_context()` | `pump/sender/pop_context.hh` |
| `with_context(...)` | `pump/sender/pop_context.hh` |
| `get_context<T...>()` | `pump/sender/get_context.hh` |
| `get_full_context_object()` | `pump/sender/get_context.hh` |

#### 协程桥接
| API | 头文件路径 |
|-----|-----------|
| `await_able()` | `pump/sender/await_sender.hh` |
| `await_able(context)` | `pump/sender/await_sender.hh` |

### 7.2 Core API 头文件

| API | 头文件路径 |
|-----|-----------|
| `make_root_context(...)` | `pump/core/context.hh` |
| `root_context<...>` | `pump/core/context.hh` |
| `pushed_context<...>` | `pump/core/context.hh` |
| `root_scope<...>` | `pump/core/scope.hh` |
| `runtime_scope<...>` | `pump/core/scope.hh` |
| `make_runtime_scope<...>(...)` | `pump/core/scope.hh` |
| `op_pusher<pos, scope_t>` | `pump/core/op_pusher.hh` |

### 7.3 Coro API 头文件

| API | 头文件路径 |
|-----|-----------|
| `task<T>` | `pump/coro/coro.hh` |
| `generator<T>` | `pump/coro/generator.hh` |

### 7.4 常用Include组合示例

```cpp
// 基础Pipeline
#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/submit.hh"
#include "pump/core/context.hh"

// 并发处理
#include "pump/sender/for_each.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/reduce.hh"

// 异常处理
#include "pump/sender/any_exception.hh"

// 上下文管理
#include "pump/sender/push_context.hh"
#include "pump/sender/pop_context.hh"
#include "pump/sender/get_context.hh"

// 协程支持
#include "pump/sender/await_sender.hh"
#include "pump/coro/coro.hh"

// 调度切换
#include "pump/sender/on.hh"
#include "pump/sender/flat.hh"
```

---

## 8. Scheduler Deep Dive (调度器详解)

### 8.1 Scheduler的作用

在PUMP框架中，**Scheduler（调度器）** 是执行域隔离与切换的核心机制。它负责：

1. **执行域隔离**：不同类型的操作（计算、IO、网络）在独立的调度器上执行，避免相互阻塞
2. **CPU核心绑定**：每个scheduler可以绑定到特定的CPU核心，实现Share-Nothing架构
3. **异步任务调度**：通过请求队列实现生产者/消费者模型，支持跨线程任务投递
4. **轮询驱动**：通过`advance()`方法被主循环轮询，处理待执行的任务

### 8.2 框架提供的Scheduler

PUMP框架在`src/env/scheduler/`目录下提供了基础scheduler实现：

#### 8.2.1 Task Scheduler（通用任务调度器）

```cpp
#include "env/scheduler/task/tasks_scheduler.hh"

// 创建task scheduler，绑定到CPU核心0
auto* task_schd = new pump::scheduler::task::scheduler(0);

// 切换到task scheduler执行
auto pipeline = just(data)
    >> on(task_schd->as_task())  // 切换执行域
    >> then([](auto d) {
        // 现在在task scheduler的核心上执行
        return process(d);
    });

// 延时执行（定时器）
auto delayed = just()
    >> task_schd->delay(1000)  // 延时1000毫秒
    >> then([]() {
        std::cout << "1秒后执行" << std::endl;
    });
```

**Task Scheduler核心API**：
| 方法 | 说明 |
|------|------|
| `as_task()` | 返回sender，用于切换到此scheduler执行 |
| `delay(ms)` | 返回定时器sender，延时指定毫秒后继续 |
| `advance()` | 处理待执行任务，由主循环调用 |

#### 8.2.2 NVMe Scheduler（存储IO调度器）

```cpp
#include "env/scheduler/nvme/nvme_scheduler.hh"

// NVMe scheduler用于高性能存储IO
// 通常与SPDK集成，提供零拷贝的磁盘读写
```

### 8.3 自建Scheduler模式（重点）

当框架提供的scheduler不能满足需求时，可以自建scheduler。以下是基于KV项目的完整模式：

#### 8.3.1 Scheduler结构模板

```cpp
namespace my_app {
    // 1. 定义请求结构
    namespace _my_operation {
        struct req {
            // 业务数据
            MyData* data;
            // 回调函数（关键！）
            std::move_only_function<void(ResultType&&)> cb;
        };

        // 2. 定义Op（操作对象）
        template <typename scheduler_t>
        struct op {
            // 类型标记，用于op_pusher特化
            constexpr static bool my_operation_op = true;
            
            scheduler_t* scheduler;
            MyData* data;

            op(scheduler_t* s, MyData* d)
                : scheduler(s), data(d) {}

            op(op&& rhs)
                : scheduler(rhs.scheduler), data(rhs.data) {}

            // 核心方法：启动异步操作
            template<uint32_t pos, typename context_t, typename scope_t>
            auto start(context_t& context, scope_t& scope) {
                return scheduler->schedule(
                    new req{
                        data,
                        // 回调中继续推进pipeline
                        [context = context, scope = scope](ResultType&& result) mutable {
                            pump::core::op_pusher<pos + 1, scope_t>::push_value(
                                context, scope, std::forward<ResultType>(result)
                            );
                        }
                    }
                );
            }
        };

        // 3. 定义Sender
        template<typename scheduler_t>
        struct sender {
            scheduler_t* scheduler;
            MyData* data;

            sender(scheduler_t* s, MyData* d)
                : scheduler(s), data(d) {}

            auto make_op() {
                return op<scheduler_t>(scheduler, data);
            }

            template<typename context_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };
    }

    // 4. 定义Scheduler类
    struct scheduler {
    private:
        friend struct _my_operation::op<scheduler>;
        
        uint32_t core;                    // 绑定的CPU核心
        spdk_ring* request_queue;         // 请求队列（SPDK ring或其他lock-free queue）
        void* tmp[4096];                  // 临时缓冲区

        // 入队方法
        void schedule(_my_operation::req* req) {
            spdk_ring_enqueue(request_queue, (void**)&req, 1, nullptr);
        }

        // 处理方法
        void handle_my_operation() {
            uint32_t count = spdk_ring_dequeue(request_queue, tmp, 1);
            for (uint32_t i = 0; i < count; ++i) {
                auto* req = (_my_operation::req*)tmp[i];
                // 执行业务逻辑
                ResultType result = do_work(req->data);
                // 调用回调，继续pipeline
                req->cb(std::move(result));
                delete req;
            }
        }

    public:
        scheduler(uint32_t c)
            : core(c)
            , request_queue(spdk_ring_create(SPDK_RING_TYPE_MP_SC, 4096, -1)) {}

        // 公开API：返回sender
        auto my_operation(MyData* data) {
            return _my_operation::sender<scheduler>(this, data);
        }

        uint32_t get_core() { return core; }

        // 主循环调用此方法
        void advance() {
            handle_my_operation();
            // 处理其他操作类型...
        }
    };
}

// 5. 特化op_pusher（在pump::core命名空间）
namespace pump::core {
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::my_operation_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename context_t>
        static inline void push_value(context_t& context, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(context, scope);
        }
    };

    // 6. 特化compute_sender_type（定义返回值类型）
    template <typename context_t, typename scheduler_t>
    struct compute_sender_type<context_t, my_app::_my_operation::sender<scheduler_t>> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto _impl_get_value_type() {
            return std::type_identity<ResultType>{};
        }
        using value_type = ResultType;
    };
}
```

#### 8.3.2 Scheduler运行机制

```cpp
// 主循环示例（参考KV项目的task_proc）
int task_proc(void* arg) {
    uint32_t this_core = get_current_core();
    
    // 收集当前核心上所有scheduler的advance方法
    std::vector<std::function<void()>> advance_list;
    
    if (task_schedulers.by_core[this_core])
        advance_list.push_back([=]() { 
            task_schedulers.by_core[this_core]->advance(); 
        });
    
    if (my_schedulers.by_core[this_core])
        advance_list.push_back([=]() { 
            my_schedulers.by_core[this_core]->advance(); 
        });
    
    // 轮询循环
    while (running) {
        for (auto& advance : advance_list) {
            advance();
        }
    }
    return 0;
}
```

### 8.4 KV项目Scheduler实战示例

KV项目展示了多种自建scheduler的实际应用：

#### 8.4.1 Scheduler类型一览

| Scheduler | 文件位置 | 职责 |
|-----------|---------|------|
| `task::scheduler` | 框架提供 | 通用任务调度、定时器 |
| `batch::scheduler` | `apps/kv/batch/scheduler.hh` | 批次snapshot分配、发布 |
| `index::scheduler` | `apps/kv/index/scheduler.hh` | 索引更新、缓存、读取 |
| `fs::scheduler` | `apps/kv/fs/scheduler.hh` | 数据页分配/回收 |
| `nvme::scheduler` | 框架提供 | NVMe读写调度 |

#### 8.4.2 Scheduler对象管理

```cpp
// apps/kv/runtime/scheduler_objects.hh
namespace apps::kv::runtime {
    // Scheduler列表结构
    struct my_scheduler_list {
        my::scheduler** by_core;  // 按核心索引
        std::vector<my::scheduler*> list;  // 所有实例
        
        my_scheduler_list()
            : by_core(new my::scheduler*[std::thread::hardware_concurrency()]{nullptr}) {}
    };

    // 全局scheduler对象
    inline task_scheduler_list task_schedulers;
    inline batch_scheduler_list batch_schedulers;
    inline index_scheduler_list index_schedulers;
    inline fs_scheduler_list fs_schedulers;
}
```

#### 8.4.3 在Pipeline中使用自建Scheduler

```cpp
// KV项目的写入流程示例
auto apply_write() {
    return get_context<data::batch*>()
        >> then([](data::batch* b) {
            // 1. 在batch scheduler上分配put ID
            return batch::allocate_put_id(b);
        })
        >> flat()
        >> then([](auto put_id) {
            // 2. 在index scheduler上更新索引
            return index::update(put_id);
        })
        >> flat()
        >> then([](auto* res) {
            // 3. 在fs scheduler上分配数据页
            return fs::allocate_data_page(res->batch);
        })
        >> flat()
        >> then([](auto* alloc_res) {
            // 4. 切换到task scheduler执行NVMe写入
            return just()
                >> as_task()  // 切换到task scheduler
                >> nvme::put_span(alloc_res->span_list);
        })
        >> flat();
}
```

#### 8.4.4 跨Scheduler切换模式

```cpp
// 使用on()切换scheduler
auto cross_scheduler_operation() {
    return just(data)
        >> on(index_scheduler->as_task())  // 切换到index scheduler
        >> then([](auto d) {
            return lookup_index(d);
        })
        >> on(fs_scheduler->as_task())     // 切换到fs scheduler
        >> then([](auto result) {
            return allocate_page(result);
        })
        >> on(task_scheduler->as_task())   // 切换到task scheduler
        >> then([](auto page) {
            return write_to_nvme(page);
        });
}
```

### 8.5 自建Scheduler检查清单

创建自定义scheduler时，确保完成以下步骤：

- [ ] **定义req结构**：包含业务数据和`std::move_only_function`回调
- [ ] **定义op结构**：包含类型标记（如`constexpr static bool xxx_op = true`）和`start()`方法
- [ ] **定义sender结构**：包含`make_op()`和`connect()`方法
- [ ] **定义scheduler类**：包含请求队列、`schedule()`、`handle_*()`和`advance()`方法
- [ ] **特化op_pusher**：在`pump::core`命名空间中，根据类型标记特化
- [ ] **特化compute_sender_type**：定义sender的返回值类型
- [ ] **注册到运行循环**：确保`advance()`被主循环调用
- [ ] **管理scheduler生命周期**：使用scheduler列表结构管理实例

### 8.6 Scheduler头文件参考

| 组件 | 头文件路径 |
|------|-----------|
| Task Scheduler | `env/scheduler/task/tasks_scheduler.hh` |
| NVMe Scheduler | `env/scheduler/nvme/nvme_scheduler.hh` |
| op_pusher基类 | `pump/core/op_pusher.hh` |
| compute_sender_type | `pump/core/compute_sender_type.hh` |
| op_list_builder | `pump/core/op_tuple_builder.hh` |
| Lock-free Queue | `pump/core/lockfree.hh` |

---

> **文档版本**：1.2  
> **最后更新**：2026-02-10  
> **适用PUMP版本**：当前代码库
