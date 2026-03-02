# PUMP Framework AI Guidelines

## 角色
C++语言专家，低延迟/并发/无锁系统开发。

## 框架概述
PUMP：基于Sender/Operator语义的高性能可组合异步管道库，声明式组合异步逻辑，支持跨执行域切换。

### 设计原则
| 原则 | 说明 |
|------|------|
| Flat OpTuple | Op平铺到`std::tuple`，非递归嵌套 |
| Position-based Pushing | 编译期位置索引推进 |
| Stack-based Context | 栈式上下文跨步骤传递 |
| Scheduler Isolation | 执行域隔离，显式切换(Task/NVMe/Net) |
| Lock-Free | 框架和应用均无锁 |
| Single-Thread Scheduler | 每个scheduler实例单线程运行 |

### 详细规范
- @ai_spec/RUNTIME_MODEL.md — 运行时模型、核心类型、模块目录
- @ai_spec/SENDERS_DETAIL.md — sender详细语义与注意事项
- @ai_spec/CODING_GUIDE.md — 编码指南、反模式、并发模型

---

## 核心规则

### 语法
1. Lambda参数优先`auto&&`，保持完美转发
2. 不可拷贝对象必须`std::move`（just传入/lambda返回/lambda捕获+mutable）
3. then返回sender → 必须`>> flat()`或用`flat_map`
4. 并发中禁止共享可变状态，用`reduce`聚合或`std::atomic`

### 禁止
1. **then中阻塞** → 用`on(scheduler)`切换后异步操作
2. **捕获局部引用** → 值捕获或Context传递
3. **then中吞异常** → 让异常传播，`any_exception`处理
4. **给src/env或src/pump加锁** → 错误是使用方式问题，追溯调用链找根因（最常见：scheduler被多线程运行）
5. **submit后Context提前销毁** → 在lambda中捕获ctx或存储到长生命周期对象
6. **协程中用submit取结果** → 用`await_able()`

### 类型分支
Pipeline静态编译，不同类型分支：`variant` → `visit()` → `if constexpr` → `flat()`

### 重构原则
语义等价是最高约束：保持可观察行为（输出、顺序、异常路径、副作用）。失败路径也是语义。无法证明不变则拆小步或先询问。

---

## 代码模式

```cpp
// 基础 pipeline
just(42) >> then([](int x){ return x*2; }) >> submit(ctx);

// 协程
auto r = co_await (just(10,20) >> then([](int a, int b){ return a+b; }) >> await_able());

// 并发（三要素：concurrent + on(scheduler) + reduce/all）
for_each(items) >> concurrent(N) >> on(sched.as_task()) >> then(process) >> reduce();

// 调度切换
just(d) >> on(io_sched.as_task()) >> then(do_io) >> on(compute.as_task()) >> then(process);

// 异常
just() >> then(risky) >> any_exception([](auto e){ return just(fallback); });

// Context
just() >> push_context(data) >> get_context<T>() >> then([](T& d){...}) >> pop_context();

// when_any（竞争，取最先完成的分支）
just() >> when_any(
    sched1->as_task() >> then([]() { return do_fast(); }),
    sched2->as_task() >> then([]() { return do_slow(); })
) >> then([](uint32_t winner_index, auto result) {
    if (result.index() == 2) use(std::get<2>(result));  // 成功值
});

// visit 分支
check() >> visit() >> then([](auto flag){
    if constexpr (std::is_same_v<decltype(flag), std::true_type>)
        return handle_true();
    else
        return handle_false();
}) >> flat();
```

---

## Sender 速查（含头文件）

| Sender | 用途 | 头文件 |
|--------|------|--------|
| `just(...)` | 起始值 | `pump/sender/just.hh` |
| `submit(ctx)` | 启动执行(fire-and-forget) | `pump/sender/submit.hh` |
| `then(f)` | 同步变换 | `pump/sender/then.hh` |
| `transform(f)` | then的语义包装 | `pump/sender/then.hh` |
| `forward_value(...)` | 显式传值(忽略前值) | `pump/sender/then.hh` |
| `ignore_args()` | 忽略所有参数 | `pump/sender/then.hh` |
| `ignore_results()` | 忽略结果 | `pump/sender/then.hh` |
| `just_exception(e)` | 抛异常 | `pump/sender/then.hh` |
| `then_exception(e)` | 传播异常 | `pump/sender/then.hh` |
| `false_to_exception(e)` | false时抛异常 | `pump/sender/then.hh` |
| `flat()` | 展开sender | `pump/sender/flat.hh` |
| `flat_map(f)` | then+flat | `pump/sender/flat.hh` |
| `on(sender)` | 调度切换 | `pump/sender/on.hh` |
| `for_each(range)` | 流式化 | `pump/sender/for_each.hh` |
| `generate(r)` / `stream(r)` / `range(r)` | for_each别名 | `pump/sender/generate.hh` |
| `loop(n)` | 0..n-1序列 | `pump/sender/generate.hh` |
| `repeat(n)` / `forever()` | 重复 | `pump/sender/repeat.hh` |
| `concurrent(n)` | 并发(必须在on之前) | `pump/sender/concurrent.hh` |
| `sequential()` | 顺序处理 | `pump/sender/sequential.hh` |
| `with_concurrent(bb)` | 并发子步骤后回顺序 | `pump/sender/sequential.hh` |
| `when_all(...)` | 并行等待多sender | `pump/sender/when_all.hh` |
| `when_any(...)` | 竞争取最先完成的sender | `pump/sender/when_any.hh` |
| `reduce(init,f)` | 聚合 | `pump/sender/reduce.hh` |
| `to_container<T>()` / `to_vector<T>()` | 收集到容器 | `pump/sender/reduce.hh` |
| `any_exception(f)` | 捕获所有异常 | `pump/sender/any_exception.hh` |
| `catch_exception<T>(f)` | 捕获特定异常 | `pump/sender/any_exception.hh` |
| `ignore_all_exception()` | 忽略异常 | `pump/sender/any_exception.hh` |
| `ignore_inner_exception(bb)` | 内部子流程忽略异常 | `pump/sender/any_exception.hh` |
| `maybe()` | optional有值→传值, 无→skip | `pump/sender/maybe.hh` |
| `maybe_not()` | optional有值→skip, 无→传空 | `pump/sender/maybe.hh` |
| `visit()` | 运行时值→编译期类型 | `pump/sender/visit.hh` |
| `when_skipped(f)` | skip时执行f | `pump/sender/when_skipped.hh` |
| `push_context(...)` | 数据入栈 | `pump/sender/push_context.hh` |
| `push_result_to_context()` | 当前值入栈 | `pump/sender/push_context.hh` |
| `get_context<T...>()` | 按类型读context | `pump/sender/get_context.hh` |
| `get_full_context_object()` | 获取完整context对象 | `pump/sender/get_context.hh` |
| `pop_context()` | 出栈 | `pump/sender/pop_context.hh` |
| `with_context(v)(bb)` | 作用域context | `pump/sender/pop_context.hh` |
| `await_able()` / `await_able(ctx)` | 协程桥接 | `pump/sender/await_sender.hh` |

### Core / Coro 头文件
| API | 路径 |
|-----|------|
| `make_root_context(...)` / `root_context` / `pushed_context` | `pump/core/context.hh` |
| `root_scope` / `runtime_scope` / `make_runtime_scope` | `pump/core/scope.hh` |
| `op_pusher<pos, scope_t>` | `pump/core/op_pusher.hh` |
| `compute_sender_type` | `pump/core/compute_sender_type.hh` |
| `op_list_builder` | `pump/core/op_tuple_builder.hh` |
| `task<T>` | `pump/coro/coro.hh` |
| `generator<T>` | `pump/coro/generator.hh` |

---

## 自建 Scheduler

需要6个组件：
1. **req** — 业务数据 + `std::move_only_function<void(Result&&)> cb`
2. **op** — 类型标记(`constexpr static bool xxx_op = true`) + `start<pos>(ctx, scope)` 构造req，cb中调`op_pusher<pos+1>::push_value`
3. **sender** — `make_op()` + `connect<context_t>()`
4. **scheduler** — lock-free请求队列 + `schedule(req*)` + `handle_*()` + `advance()`
5. **特化op_pusher** — `pump::core`命名空间，requires类型标记，调用`op.start<pos>(ctx, scope)`
6. **特化compute_sender_type** — 定义sender的value_type

### 模板

```cpp
namespace my_app {
// 1. req
namespace _my_op {
    struct req {
        MyData* data;
        std::move_only_function<void(Result&&)> cb;
    };
    // 2. op
    template <typename sched_t>
    struct op {
        constexpr static bool my_op_tag = true;
        sched_t* sched;
        MyData* data;
        template<uint32_t pos, typename ctx_t, typename scope_t>
        auto start(ctx_t& ctx, scope_t& scope) {
            sched->schedule(new req{data,
                [ctx=ctx, scope=scope](Result&& r) mutable {
                    pump::core::op_pusher<pos+1, scope_t>::push_value(ctx, scope, std::move(r));
                }});
        }
    };
    // 3. sender
    template<typename sched_t>
    struct sender {
        sched_t* sched; MyData* data;
        auto make_op() { return op<sched_t>(sched, data); }
        template<typename ctx_t>
        auto connect() { return pump::core::builder::op_list_builder<0>().push_back(make_op()); }
    };
}
// 4. scheduler
struct scheduler {
    spdk_ring* queue;
    void schedule(_my_op::req* r) { spdk_ring_enqueue(queue, (void**)&r, 1, nullptr); }
    void advance() { /* dequeue + req->cb(result) + delete req */ }
    auto my_op(MyData* d) { return _my_op::sender<scheduler>(this, d); }
};
}
// 5. 特化 op_pusher
namespace pump::core {
template<uint32_t pos, typename scope_t>
requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
    && (get_current_op_type_t<pos, scope_t>::my_op_tag)
struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
    template<typename ctx_t>
    static inline void push_value(ctx_t& ctx, scope_t& scope) {
        std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
    }
};
// 6. 特化 compute_sender_type
template <typename ctx_t, typename sched_t>
struct compute_sender_type<ctx_t, my_app::_my_op::sender<sched_t>> {
    consteval static uint32_t count_value() { return 1; }
    consteval static auto _impl_get_value_type() { return std::type_identity<Result>{}; }
    using value_type = Result;
};
}
```

### Scheduler 头文件
| 组件 | 路径 |
|------|------|
| Task Scheduler | `env/scheduler/task/tasks_scheduler.hh` |
| NVMe Scheduler | `env/scheduler/nvme/nvme_scheduler.hh` |
| Net Scheduler | `env/scheduler/net/*` |
| op_pusher基类 | `pump/core/op_pusher.hh` |
| compute_sender_type | `pump/core/compute_sender_type.hh` |
| op_list_builder | `pump/core/op_tuple_builder.hh` |

参考实现：`src/env/scheduler/task/tasks_scheduler.hh`, `apps/kv/batch/scheduler.hh`
