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
| Scheduler Isolation | 执行域隔离，显式切换(Task/NVMe/TCP/UDP/KCP/RPC/DPDK) |
| Lock-Free | 框架和应用均无锁 |
| Single-Thread Scheduler | 每个scheduler实例单线程运行 |
| Per-Core SPSC Queue | 跨域消息使用per-core SPSC队列+bitmap，无CAS竞争 |

### 详细规范
- @ai_spec/RUNTIME_MODEL.md — 运行时模型、核心类型、模块目录
- @ai_spec/SENDERS_DETAIL.md — sender详细语义与注意事项
- @ai_spec/CODING_GUIDE.md — 编码指南、反模式、并发模型、复杂功能实现指南（§6）
- @ai_spec/RPC_DETAIL.md — RPC模块架构、协议、service定义模式

### 参考实现
- `apps/kv/` — 完整 KV 存储引擎，展示五类 scheduler 协作、Leader/Follower 合并、跨域 pipeline 编排（详细分析见 `ai_context/kv_analysis.md`）
- `apps/aisaq/` — NVMe 向量搜索引擎（DiskANN），展示 SPDK NVMe + pipeline beam search + GPU PQ 训练 + 多核 share-nothing 架构

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

### 陷阱
1. **flat_map滥用** → `flat_map`只用于lambda直接返回scheduler异步sender（如`nvme->get()`、`rpc::call()`）。同步逻辑用`then()`，跨步骤传值用`push_context`/`get_context`，需要流式变换用协程generator
2. **push_context循环move-from** → `push_context(value)`内部会move，循环中复用同一op第二次迭代起访问moved-from对象。循环中用`then([]{return new_obj();}) >> push_result_to_context()`替代
3. **不要轻易怀疑框架有bug** → 历史上所有"框架bug"最终都是应用代码错误，先审查应用层pipeline用法、参数、生命周期
4. **不要怀疑框架性能** → pipeline本身开销极低，性能问题在应用层找。排查方向：并发利用率、序列化瓶颈、编译优化选项（-O3）。NVMe事实：3×4K读和1×12K读性能一致（NVMe无顺序页概念）
5. **Release构建assert副作用** → `-O3 -DNDEBUG`会去掉`assert()`，内含副作用（赋值、函数调用）的assert在Release中逻辑丢失。永远先提取副作用到变量再assert

### 类型分支
Pipeline静态编译，不同类型分支：`variant` → `visit()` → `if constexpr` → `flat()`

### 重构原则
语义等价是最高约束：保持可观察行为（输出、顺序、异常路径、副作用）。失败路径也是语义。无法证明不变则拆小步或先询问。

### 开发方法论（实现复杂功能时必须遵循）

**核心认知：Pipeline 编排调度顺序，Scheduler 承载业务逻辑。** 复杂逻辑写在 scheduler 的 `handle_*()` 中（如 leader/follower 合并、waiter 协调），pipeline 只负责"什么时候做、做完去哪"。

**实现步骤（按顺序执行）：**

1. **确定数据归属与不变量** — 哪些状态必须被同一线程独占访问？哪些操作必须串行？这决定了需要哪些 scheduler
2. **设计 Scheduler** — 种类、实例数、路由策略（hash分片/随机/固定单点）、每个 scheduler 的 req/res 类型和 handle 逻辑
3. **画跨域数据流** — pipeline 在哪些 scheduler 之间跳转，每个跳转点的输入输出类型是什么
4. **编码** — 先写 scheduler 的 handle 逻辑，再写 sender pipeline 编排，最后写顶层组合

**何时自建 Scheduler：** 存在被多条并发 pipeline 共享的可变状态，或需要跨请求协调（合并/等待/排序），或绑定硬件资源。无共享状态 → `then()` + `on(task_scheduler)` 即可。Scheduler 设计的完整决策流程见 @ai_spec/CODING_GUIDE.md §6.2。

**异常处理分层：** 操作级 `catch_exception<T>` → 子流程级 `ignore_inner_exception` → 事务级 `ignore_all_exception` → 全局兜底 `any_exception`。资源释放代码（如 finish_batch）必须在异常屏蔽之后，确保一定执行。详见 §6.5。

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

// 抢占调度（任意核心均可消费，用于跨核心负载均衡）
auto* preemptive = runtime->any_scheduler<task_scheduler_t>();  // 返回 preemptive_scheduler*
just() >> on(preemptive->as_task()) >> then(work) >> submit(ctx);

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

// RPC 服务端（session-only，scheduler 类型自动推导）
rpc::serv<service::type::sub, service::type::add>(session)
    >> submit(ctx);

// RPC 客户端（session-only，call 是 bind_back，需 just() >> 前缀）
just() >> rpc::call<service::type::add>(session, 10, 20)
    >> then([](auto res) { use(res.v); })
    >> submit(ctx);

// RPC 并发调用（同一 session 上 pipelining）
for_each(requests) >> concurrent(N)
    >> flat_map([session](auto&& args) {
        return just() >> rpc::call<service::type::add>(session, args.a, args.b);
    }) >> reduce();

// UDP 收发（帧同步/实时流等场景，非 RPC）
udp::recv(sche) >> then([](udp::common::datagram&& dg, udp::common::endpoint ep) { ... });
udp::send(sche, ep, data, len) >> then([](bool ok) { ... });
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
| `rpc::serv<ids...>(session)` | RPC服务端（session-only） | `env/scheduler/net/rpc/rpc.hh` |
| `rpc::call<id>(session, args...)` | RPC客户端（bind_back，需`just() >>`） | `env/scheduler/net/rpc/rpc.hh` |
| `tcp::recv(session)` | TCP接收→net_frame | `env/scheduler/net/tcp/tcp.hh` |
| `tcp::send(session, data, len)` | TCP发送→bool | `env/scheduler/net/tcp/tcp.hh` |
| `kcp::recv(session)` | KCP接收→net_frame | `env/scheduler/net/kcp/kcp.hh` |
| `kcp::send(session, data, len)` | KCP发送→bool | `env/scheduler/net/kcp/kcp.hh` |
| `udp::recv(sche)` | UDP接收→(datagram, endpoint) | `env/scheduler/net/udp/udp.hh` |
| `udp::send(sche, ep, data, len)` | UDP发送→bool | `env/scheduler/net/udp/udp.hh` |

### Core / Coro 头文件
| API | 路径 |
|-----|------|
| `make_root_context(...)` / `root_context` / `pushed_context` | `pump/core/context.hh` |
| `root_scope` / `runtime_scope` / `make_runtime_scope` | `pump/core/scope.hh` |
| `op_pusher<pos, scope_t>` | `pump/core/op_pusher.hh` |
| `compute_sender_type` | `pump/core/compute_sender_type.hh` |
| `op_list_builder` | `pump/core/op_tuple_builder.hh` |
| `per_core::queue` / `spsc::queue` / `mpmc::queue` / `this_core_id` | `pump/core/lock_free_queue.hh` |
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
    core::per_core::queue<_my_op::req*> req_q;
    void schedule(_my_op::req* r) { req_q.try_enqueue(r); }
    bool advance() {
        return req_q.drain([](_my_op::req* r) { r->cb(result); delete r; });
    }
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
| Task Scheduler（含preemptive_scheduler） | `env/scheduler/task/tasks_scheduler.hh` |
| NVMe Scheduler | `env/scheduler/nvme/scheduler.hh` |
| TCP Scheduler | `env/scheduler/net/tcp/*` |
| TCP Layers（tcp_bind, tcp_ring_buffer） | `env/scheduler/net/tcp/common/layers.hh` |
| UDP Scheduler（io_uring / epoll / dpdk） | `env/scheduler/net/udp/io_uring/scheduler.hh`, `env/scheduler/net/udp/epoll/scheduler.hh`, `env/scheduler/net/udp/dpdk/scheduler.hh` |
| KCP Scheduler（io_uring / epoll / dpdk） | `env/scheduler/net/kcp/io_uring/scheduler.hh`, `env/scheduler/net/kcp/epoll/scheduler.hh`, `env/scheduler/net/kcp/dpdk/scheduler.hh` |
| dgram Transport（io_uring / epoll / dpdk） | `env/scheduler/net/dgram/io_uring.hh`, `env/scheduler/net/dgram/epoll.hh`, `env/scheduler/net/dgram/dpdk.hh` |
| RPC | `env/scheduler/net/rpc/*` |
| Session 组合 | `env/scheduler/net/common/session.hh`（`pump::scheduler::net::session_t`） |
| 共享帧类型 | `env/scheduler/net/common/frame.hh`（`pump::scheduler::net::net_frame`） |
| 共享 send/recv sender | `env/scheduler/net/common/send_sender.hh`, `env/scheduler/net/common/recv_sender.hh` |
| Frame Receiver Layer | `env/scheduler/net/common/frame_receiver.hh` |
| Runtime（runtime_schedulers, any_scheduler） | `env/runtime/runner.hh` |
| Share-Nothing Runner | `env/runtime/share_nothing.hh` |
| op_pusher基类 | `pump/core/op_pusher.hh` |
| compute_sender_type | `pump/core/compute_sender_type.hh` |
| op_list_builder | `pump/core/op_tuple_builder.hh` |

参考实现：`src/env/scheduler/task/tasks_scheduler.hh`, `apps/kv/batch/scheduler.hh`, `src/env/scheduler/net/rpc/client/trigger.hh`（轻量自定义scheduler示例）

---

## 队列类型

框架提供四种队列（`pump/core/lock_free_queue.hh`）：

| 队列 | 用途 | 原子操作 |
|------|------|---------|
| `per_core::queue<T, CAP>` | **跨域消息传递（默认选择）**。每个源核心一个SPSC队列+bitmap | SPSC: relaxed load + release store; bitmap: `fetch_or`/`exchange` |
| `spsc::queue<T, CAP>` | 单生产者单消费者 | relaxed load + acquire/release store |
| `mpmc::queue<T, CAP>` | 多生产者多消费者（如preemptive_scheduler） | CAS (`compare_exchange_weak`) |
| `local::queue<T, CAP>` | 同线程内部缓冲 | 无原子操作 |

**per_core::queue 要点**：
- 生产者通过 `this_core_id`（thread_local）自动路由到对应SPSC队列
- `drain(handler)` 用bitmap快速定位有数据的队列，批量消费
- `try_enqueue()` 后自动 `fetch_or` 设置bitmap位
- 支持最多128核心（`MAX_CORES=128`，2个bitmap word）
- `this_core_id` 在 `share_nothing.hh` 的 `run()` 中自动初始化

## 抢占调度

`preemptive_scheduler` 使用MPMC队列，允许任意核心投递、任意核心消费，用于跨核心负载均衡。

```cpp
// scheduler 声明 preemptive 支持
struct scheduler {
    using preemptive_scheduler_t = preemptive_scheduler;  // 关联类型
    // ...
    template<typename Runtime>
    bool advance(Runtime& rt) {
        if (!advance())
            return handle_preemptive(rt);  // 空闲时消费抢占队列
        return true;
    }
};

// 运行时获取抢占调度器
auto* preemptive = runtime->any_scheduler<task_scheduler_t>();
// 如果T有preemptive_scheduler_t → 返回全局preemptive_scheduler*（任意核心可消费）
// 否则 → 随机选一个普通scheduler实例
```
