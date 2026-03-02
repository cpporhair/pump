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
