# 编码指南、反模式与并发模型

> 本文档补充 CLAUDE.md 中核心规则/禁止项未覆盖的深层内容。基础规则和代码模式见 CLAUDE.md。

## 1. 问题分析方法论

### 1.1 三层次分析

| 层次 | 描述 | 示例 |
|------|------|------|
| **现象层** | 观察到的症状 | "递归导致栈溢出" |
| **机制层** | 发生的原因 | "poll_next 被重复调用形成调用链" |
| **本质层** | 系统不变量和约束 | "迭代器顺序访问"、"数据流依赖关系" |

**关键**: 不要停留在现象层找方案，深入本质层理解约束。

### 1.2 方案评估优先级

1. **领域特定方案** — 利用问题本身的约束和语义
2. **预防性方案** — 从根本上避免问题发生
3. **通用技术方案** — 蹦床模式、深度检测等（最后选择）

### 1.3 方案设计前必问清单

提出技术方案前必须确认：

**数据/资源访问模式**:
- 顺序还是可并行？有独占资源吗？生产者/消费者关系？

**调用关系和控制流**:
- 组件间调用关系？是否有回调/反向调用？同步还是异步？

**系统不变量**:
- 什么条件必须始终为真？什么操作不能同时发生？什么顺序必须保证？

**约束条件**:
- 是否需要无锁？允许额外内存分配吗？可以依赖外部组件吗？

### 1.4 并发问题思考框架

1. **同一时刻需要多少个执行流？** → 一个: 令牌/互斥；多个: 无锁结构
2. **其他执行流需要做什么？** → 登记需求: 计数器；等待结果: future/promise
3. **如何保证进展？** → 令牌持有者处理所有 pending；用循环替代递归

## 2. 反模式详解

CLAUDE.md 列出了 6 条禁止项。以下补充关键代码示例：

### 2.1 then 返回 sender 忘记 flat

```cpp
// ❌ 结果类型是 sender 而非实际值
just(key) >> then([](auto k) { return async_lookup(k); });

// ✅
just(key) >> then([](auto k) { return async_lookup(k); }) >> flat();
// ✅ 或用 flat_map
just(key) >> flat_map([](auto k) { return async_lookup(k); });
```

### 2.2 并发中共享可变状态

```cpp
// ❌ 数据竞争
int counter = 0;
for_each(items) >> concurrent(4) >> then([&counter](auto item) {
    counter++;  // 竞争！
    return process(item);
}) >> reduce();

// ✅ 用 reduce 聚合
for_each(items) >> concurrent(4) >> then([](auto item) {
    return process(item);
}) >> reduce(0, [](int acc, auto) { return acc + 1; });

// ✅ 或用 atomic
auto counter = std::make_shared<std::atomic<int>>(0);
for_each(items) >> concurrent(4) >> then([counter](auto item) {
    counter->fetch_add(1);
    return process(item);
}) >> reduce();
```

### 2.3 submit 的 Context 生命周期

```cpp
// ❌ 局部 ctx 可能在异步任务完成前销毁
void danger() {
    auto ctx = make_root_context(data);
    just(42) >> then([](int x) { return x * 2; }) >> submit(ctx);
}  // ctx 销毁，异步任务访问悬空指针

// ✅ 在 lambda 中捕获 ctx 延长生命周期
void safe() {
    auto ctx = make_root_context(data);
    just(42) >> then([ctx](int x) { return x * 2; }) >> submit(ctx);
}
```

### 2.4 给 src/env 或 src/pump 加锁

```cpp
// ❌ 禁止给框架代码加锁
struct op { std::mutex mtx; /* ... */ };           // 禁止
struct scheduler { std::mutex mtx; /* ... */ };    // 禁止

// ✅ 正确做法：追溯调用链找根因
// Step 1: 确认错误位置
// Step 2: 追溯是谁调用了这段代码
// Step 3: 检查是否 scheduler 被多线程运行（最常见根因）
// Step 4: 在应用层解决，而非修改框架
```

## 3. 并发模型

### 3.1 concurrent 三要素

| 情况 | 结果 |
|------|------|
| 只有 concurrent，无调度 | **串行** |
| 只有调度，无 concurrent | **串行** |
| concurrent + on(scheduler) | **真正并发** |

```cpp
for_each(items)
    >> concurrent(N)                // 1. 声明并发（必须在 on 之前）
    >> on(scheduler.as_task())      // 2. 调度（真正并发开始）
    >> then(process)                // 3. 业务逻辑
    >> reduce()                     // 4. 收集结果
```

### 3.2 多级并发架构

```
第1层: generate_on >> concurrent(10000) >> as_batch(...)
  第2层: for_each(cache) >> concurrent() >> index::update
    第3层: for_each(spans) >> concurrent() >> nvme::put_span
      第4层: as_stream(pages) >> concurrent() >> put_page
        第5层: NVMe scheduler 异步 IO
```

嵌套并发：
```cpp
for_each(outer) >> concurrent(N) >> on(sched) >> then([](auto item) {
    return for_each(item.inner)
        >> concurrent() >> on(inner_sched)
        >> then(process) >> all();
}) >> flat() >> reduce();
```

### 3.3 Leader/Follower 合并模式

多个并发请求合并，leader 统一执行，follower 等待结果。Scheduler 返回 `variant<leader_res*, follower_res, failed_res>`。

```cpp
>> visit()  // variant 展开
>> then([](auto&& res) {
    if constexpr (std::is_same_v<__typ__(res), leader_res*>)
        return just() >> write_data(res->span_list) >> notify_follower(res);
    else if constexpr (std::is_same_v<__typ__(res), follower_res>)
        return just(__fwd__(res));
}) >> flat()
```

## 4. visit 算子深入

### 4.1 转换规则

| 输入 | 输出 |
|------|------|
| `bool` true | `std::true_type` |
| `bool` false | `std::false_type` |
| `variant<A,B,C>` | 分别传递 A、B、C（各自实例化后续 lambda） |
| `T*` 非空 | `T*`（原指针） |
| `T*` 空 | `std::nullptr_t` |

### 4.2 原理

`visit` 为每种可能类型**分别实例化**后续 lambda。每个实例化版本只有一个返回类型，因此即使各分支返回不同类型的 sender 也能编译通过。

### 4.3 典型用法

```cpp
// bool 分支
>> then([](uint64_t i) { return (spdk_get_ticks() % 100) < 50; })
>> visit()
>> then([](auto&& flag) {
    if constexpr (std::is_same_v<__typ__(flag), std::true_type>)
        return as_batch(make_kv() >> put() >> apply());
    else
        return as_batch(random_key(max) >> get());
}) >> flat()
```

```cpp
// variant 分支（index lookup 返回 variant<hit, miss>）
>> visit()
>> then([](auto&& result) {
    using T = std::decay_t<decltype(result)>;
    if constexpr (std::is_same_v<T, IndexHit>)
        return just() >> read_data(result.loc);
    else
        return just(empty_value);
}) >> flat()
```

### 4.4 使用要点

- `visit()` 后的 `then` lambda 参数必须用 `auto&&`
- 各分支返回不同 sender 时，后面必须 `>> flat()`
- `visit(value)` 变体会把额外 value 一起传递

## 5. 并发数选择参考

| 场景 | 建议 |
|------|------|
| CPU 密集 | 核心数 |
| IO 密集 | 1000-10000 |
| 内存受限 | 按内存计算 |
| 无限制 | `concurrent()` 无参数 |

## 6. 复杂功能实现指南

> 基于 KV 项目（`apps/kv/`）的实践总结。实现跨多个 scheduler 的复杂业务时，遵循以下指南。

### 6.1 Pipeline 与 Scheduler 的职责分工

**核心原则：Pipeline 是编排层，Scheduler 是逻辑层。**

| 职责 | 归属 | 示例 |
|------|------|------|
| 决定执行顺序和并发度 | Pipeline（sender 组合） | `concurrent(N) >> on(sched) >> reduce()` |
| 决定跨域跳转路径 | Pipeline（`on()` / 返回其他 scheduler 的 sender） | `then([](){ return index::update(f); }) >> flat()` |
| 实际的业务判断和数据操作 | Scheduler 的 `handle_*()` | leader/follower 合并、B-tree 查找、waiter 协调 |
| 运行时多路分支的决策 | Scheduler（通过 variant 返回） | `req->cb(variant<leader*, follower, failed>{})` |
| 编译期类型分支的处理 | Pipeline（`visit` + `if constexpr`） | 根据 scheduler 返回的 variant 走不同子 pipeline |

**错误做法：** 把复杂业务逻辑全写在 `then` lambda 中。lambda 应该只做简单的数据变换或返回另一个 scheduler 的 sender。

```cpp
// ❌ 把业务逻辑塞进 then
>> then([](auto&& req) {
    // 50 行复杂逻辑：合并请求、分配资源、协调等待者 ...
    return result;
})

// ✅ 业务逻辑在 scheduler 的 handle 中，pipeline 只做编排
>> then([](auto&& batch) { return fs::allocate_data_page(batch); })  // 进入 fs scheduler
>> flat()
>> then([](auto&& res) {  // variant 分支处理
    if constexpr (is_leader<res>) return just() >> write_data(res) >> notify_follower(res);
    else return just(__fwd__(res));
}) >> flat()
```

### 6.2 Scheduler 设计决策

#### 何时需要自建 Scheduler

核心判断：**是否存在需要被多条并发 pipeline 共享访问的可变状态？**

| 情况 | 方案 | 理由 |
|------|------|------|
| 无状态计算 | `on(task_scheduler) >> then(f)` | 不需要保护任何状态 |
| 状态是 pipeline 私有的 | 值传递或 context | 不存在共享，不需要 scheduler |
| 状态已被现有 scheduler 管理 | 用现有 scheduler 的 sender | 不需要新建 |
| **共享可变状态** | **自建 scheduler** | 单线程 `handle_*()` 天然互斥，无需锁 |
| **跨请求协调**（合并/等待/排序） | **自建 scheduler** | 协调逻辑必须在同一线程中看到多个请求 |
| **硬件资源绑定**（NVMe qpair、fd） | **自建 scheduler** | 硬件资源必须从固定线程操作 |

简记：`then()` 解决"做计算"，`on(task_scheduler)` 解决"换线程"，自建 scheduler 解决"管状态"。

#### 确定需要自建后，回答以下设计问题

**Q1: 需要多少个实例？如何路由？**
- 全局唯一（如快照序号） → 单实例，固定路由 `list[0]`
- 可按 key 分片（如索引） → 多实例，hash 路由 `key_seed % N`
- 无状态 / 可任意分发 → 多实例，随机路由 `ticks % N`

**Q2: handle 有几种结果路径？**
- 单一结果 → `cb()` 或 `cb(result)`
- 多种结果（leader/follower/failed） → `cb(variant<A,B,C>)`，op 中 `std::visit` 展开

**Q3: 是否需要跨请求协调？**
- 不需要 → 简单 dequeue + process + cb
- 需要合并（group commit） → Leader/Follower 模式：连续 dequeue 多个请求，第一个为 leader，后续为 follower
- 需要等待（按需加载） → Waiter 链表：首次访问者成为 reader-leader 执行 IO，后续访问者挂 waiter 等通知

### 6.3 variant 多路返回值模式

Scheduler 需要根据运行时状态返回不同结果时的标准做法：

```
Scheduler handle_*()
  │ 判断业务状态
  ├─ 情况A → req->cb(variant<A*, B, C>{A*指针})
  ├─ 情况B → req->cb(variant<A*, B, C>{B{}})
  └─ 情况C → req->cb(variant<A*, B, C>{C{}})
       │
       ▼ op::start 中的 callback
  std::visit([&ctx, &scope](auto&& v) {
      op_pusher<pos+1>::push_value(ctx, scope, std::forward<decltype(v)>(v));
  }, std::move(v));
       │
       ▼ Pipeline 中
  >> then([](auto&& res) {
      if constexpr (std::is_same_v<__typ__(res), A*>)
          return /* A 的子 pipeline */;
      else if constexpr (std::is_same_v<__typ__(res), B>)
          return /* B 的子 pipeline */;
      else
          return /* C 的子 pipeline */;
  }) >> flat()
```

**关键：** variant 展开不用 `visit()` sender（因为值已在 op 的 callback 中通过 `std::visit` 展开了），后续 then 直接用 `if constexpr` 分支。只有当 pipeline 中间产生的值是 variant/bool 时才用 `visit()` sender。

### 6.4 Context 栈使用策略

| 场景 | 手段 | 示例 |
|------|------|------|
| 事务级隐式参数 | `push_context(ptr)` + `get_context<T>()` | batch 指针在整个事务中传递 |
| 临时资源生命周期保护 | `with_context(value)(pipeline)` | string key 跨异步调用存活 |
| 同类型多值消歧 | `push_context_with_id<__COUNTER__>` | 嵌套事务中区分不同 batch |
| 子流程独立数据 | `with_context(data)(子pipeline)` | 每次循环迭代独立的 req/res 帧 |

**原则：** 需要跨多个 `then` / 跨 scheduler 传递的数据放 context，单个 then 内用完的数据直接做参数传递。

### 6.5 异常处理分层策略

从内到外四层，每层有不同职责：

```
操作级 ─── catch_exception<具体异常>(recovery_sender)
  │          精确捕获，执行恢复逻辑（如释放已分配的页面）
  │
子流程级 ─ ignore_inner_exception(子pipeline)
  │          隔离子流程异常，不影响外层控制流
  │
事务级 ─── ignore_all_exception()  [在资源释放代码之前]
  │          确保 finish/cleanup 代码一定执行
  │
全局级 ─── any_exception([](auto e){ log + just() })
             顶层兜底，打印/记录后继续
```

**资源释放的异常安全模式（类 RAII）：**
```cpp
// start_xxx() 获取资源
// >> user_pipeline
// >> ignore_all_exception()   ← 屏蔽用户异常
// >> finish_xxx()             ← 确保释放（publish/delete/pop_context）
```

### 6.6 跨域数据流设计

复杂功能通常涉及多个 scheduler 之间的数据流转。设计时画出完整的跳转路径：

```
apply() 写路径的跨域流：

  [调用方 thread]
       │ get_context<batch*>()
       ▼
  [batch scheduler]  ← allocate_put_id：分配版本号
       │ cb()
       ▼
  [index scheduler × N]  ← update：并发更新索引（for_each + concurrent）
       │ cb() × N → reduce()
       ▼
  [fs scheduler]  ← allocate_data_page：Leader/Follower 合并
       │ cb(variant<leader*, follower, failed>)
       ▼ if constexpr 分支
  [nvme scheduler × M]  ← write_data：并发 DMA 写（for_each + concurrent）
       │ cb() × M → reduce()
       ▼
  [index scheduler × N]  ← cache：并发缓存数据
       │ cb() × N → reduce()
       ▼ done
```

**要点：**
- 每次进入新 scheduler = 一次异步跳转（当前线程让出，scheduler 的 advance 线程接管）
- 从 scheduler 回来 = callback 触发 `op_pusher<pos+1>::push_value`
- 并发扇出（for_each + concurrent）后必须扇入（reduce/all）
- 类型在每个跳转点都是编译期确定的
