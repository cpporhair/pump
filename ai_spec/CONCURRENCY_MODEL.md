# PUMP Framework 并发模型设计指南

> **目标受众**：AI代码助手  
> **文档目的**：指导AI正确理解和使用PUMP框架的并发模型

---

## 1. 核心概念：concurrent 不等于并发

### 1.1 关键理解

**`concurrent` 算子本身不带来真正的并发，它只是声明允许并发。**

真正的并发需要满足两个条件：
1. 使用 `concurrent(N)` 声明允许的最大并发数
2. 使用 `on(scheduler)` 将任务调度到调度器执行

### 1.2 三种情况对比

| 情况 | 代码模式 | 结果 |
|------|---------|------|
| 只有 `concurrent`，没有调度 | `for_each >> concurrent() >> then(sync_func)` | **串行执行**，因为业务逻辑是同步函数调用 |
| 只有调度，没有 `concurrent` | `for_each >> on(scheduler) >> then(...)` | **串行执行**，会等整个业务逻辑返回才处理下一个 |
| `concurrent` + 调度 | `for_each >> concurrent() >> on(scheduler) >> then(...)` | **真正并发**，concurrent 允许并发，scheduler 实现调度 |

---

## 2. 正确的使用模式

### 2.1 基本模式

```cpp
// 正确的并发模式
pump::sender::for_each(items)
    >> pump::sender::concurrent(N)      // 1. 声明允许最大 N 个并发
    >> on_any_task_scheduler()          // 2. 调度到 task_scheduler（这里开始真正并发）
    >> pump::sender::then([](auto item) {
        // 3. 业务逻辑
        return process(item);
    })
    >> pump::sender::all()              // 4. 等待所有完成
```

### 2.2 参考示例（来自 apps/example/concurrent/main.cc）

```cpp
return pump::sender::for_each(std::views::iota(0, 1000000))
    // concurrent 的作用是允许之后的代码，一直到 pump::sender::all()，可以以最大 1000 的数量并发
    >> pump::sender::concurrent(1000)
    // 调度到任意一个 task_scheduler 上，on_any_task_scheduler 这里才会开始真的并发
    // 如果不加 concurrent，那么即使阻塞在 task_scheduler 上，也会等整个业务逻辑返回才并发
    // 如果不调度到任何一个 scheduler 上，哪怕加了 concurrent，因为之后的业务逻辑是同步的函数调用，所以实际还是串行，没有任何并发
    >> on_any_task_scheduler()
    >> pump::sender::then([](uint64_t id) {
        return id * 2;
    })
    >> pump::sender::all()
    >> pump::sender::then([](bool succeed) {
        std::cout << succeed << std::endl;
    });
```

### 2.3 关键顺序

```
for_each(items)           // 产生流
    >> concurrent(N)      // 声明允许并发
    >> on(scheduler)      // 调度到 scheduler（真正并发开始）
    >> then(...)          // 业务逻辑
    >> all() / reduce()   // 收集结果
```

**注意**：`concurrent` 必须在 `on(scheduler)` 之前，否则无法控制并发数。

---

## 3. 调度器是并发的驱动力

### 3.1 调度器的作用

调度器（scheduler）是真正执行任务的地方。每个核心运行一个线程，调用调度器的 `advance()` 方法推进任务执行。

```cpp
// 每个核心的主循环
while (running) {
    task_scheduler->advance();    // 推进任务调度
    batch_scheduler->advance();   // 推进 batch 发布
    fs_scheduler->advance();      // 推进页面分配
    nvme_scheduler->advance();    // 推进 NVMe IO
}
```

### 3.2 常用调度方式

```cpp
// 方式1：使用 on() 算子
>> pump::sender::on(scheduler->as_task())

// 方式2：使用 schedule_at()
>> pump::sender::on(pump::scheduler::task::schedule_at(scheduler))

// 方式3：随机选择调度器
>> pump::sender::on(pump::scheduler::task::schedule_at(
       pump::scheduler::task::any_scheduler(schedulers)))

// 方式4：KV 项目中的封装
>> any_task_scheduler()  // 随机选择一个 task_scheduler
```

---

## 4. 多级并发架构（以 KV 项目为例）

KV 项目展示了复杂的多级并发嵌套架构：

### 4.1 完整的并发层次图

```
main.cc: load(max_key) >> updt(max_key) >> read(max_key)
                ↓
┌─────────────────────────────────────────────────────────────────┐
│ 第1层: 顶层并发 (ycsb.hh)                                        │
│   generate_on(any_task_scheduler(), 0..10000000)                │
│       >> concurrent(10000)  ← 允许 10000 个并发 batch            │
│       >> as_batch(...)                                          │
└─────────────────────────────────────────────────────────────────┘
                ↓ 每个并发单元
┌─────────────────────────────────────────────────────────────────┐
│ 第2层: Batch 内部并发 (apply.hh)                                 │
│   update_index(b):                                              │
│       for_each(b->cache) >> concurrent() >> then(index::update) │
│                                                                 │
│   merge_batch_and_allocate_page():                              │
│       → fs::scheduler 的 Leader/Follower 合并                   │
└─────────────────────────────────────────────────────────────────┘
                ↓ Leader 执行写入
┌─────────────────────────────────────────────────────────────────┐
│ 第3层: Span 级别并发 (apply.hh - write_data)                     │
│   for_each(list.spans)                                          │
│       >> concurrent()  ← 多个 span 并发                          │
│       >> nvme::then_put_span()                                  │
└─────────────────────────────────────────────────────────────────┘
                ↓ 每个 span
┌─────────────────────────────────────────────────────────────────┐
│ 第4层: Page 级别并发 (nvme/sender.hh - put_pages)                │
│   as_stream(pages)                                              │
│       >> concurrent()  ← 多个 page 并发                          │
│       >> flat_map([s](auto *p) { return put_page(p, s); })      │
└─────────────────────────────────────────────────────────────────┘
                ↓ 每个 page
┌─────────────────────────────────────────────────────────────────┐
│ 第5层: NVMe 调度器异步 IO                                        │
│   scheduler->put(page) → SPDK 异步写入                          │
└─────────────────────────────────────────────────────────────────┘
```

### 4.2 各层详解

#### 第1层：顶层并发（10000 并发 batch）

```cpp
// ycsb.hh - load()
return start_statistic()
    >> generate_on(any_task_scheduler(), std::views::iota(uint64_t(0), max))
    >> concurrent(10000)  // 允许 10000 个并发
    >> as_batch(make_kv() >> put() >> apply() >> statistic_put())
    >> count();
```

- `generate_on(any_task_scheduler(), ...)` 生成流，每个元素调度到随机 task_scheduler
- `concurrent(10000)` 声明允许最多 10000 个 batch 同时执行
- 真正的并发发生在 `any_task_scheduler()` 将任务调度到不同核心

#### 第2层：Batch 内部并发

```cpp
// apply.hh - update_index()
return pump::sender::for_each(b->cache)
    >> pump::sender::concurrent()  // 无限并发
    >> pump::sender::then([](data::key_value& kv){ return index::update(kv.file); })
    >> pump::sender::flat()
    >> pump::sender::reduce();
```

- 一个 batch 中的多个 key-value 可以并发更新索引
- `concurrent()` 不带参数表示无限并发

#### 第3层：Span 级别并发

```cpp
// apply.hh - write_data()
return pump::sender::for_each(list.spans)
    >> pump::sender::concurrent()  // 多个 span 并发
    >> nvme::then_put_span()
    >> pump::sender::all(...);
```

- 合并后的 `span_list` 包含多个 `write_span`
- 多个 span 可以并发写入不同的 NVMe 位置

#### 第4层：Page 级别并发

```cpp
// env/scheduler/nvme/sender.hh - put_pages()
return sender::as_stream(__fwd__(pl))
    >> sender::concurrent()  // 多个 page 并发
    >> sender::flat_map([s](auto *p) { return put_page(p, s); })
    >> sender::all();
```

- 每个 span 内部包含多个 page
- 多个 page 可以并发提交到 NVMe 调度器

---

## 5. Leader/Follower 合并模式

### 5.1 概念

多个并发请求可以被合并，由一个 leader 统一执行，其他请求作为 follower 等待结果。

### 5.2 实现示例（fs/scheduler.hh）

```cpp
void handle_allocate() {
    _allocate::leader_res *res = nullptr;
    _allocate::req* ldr = nullptr;

    // 不断从队列取请求，直到达到 max_merge_pages 限制
    while ((res == nullptr || res->span_list.page_count() <= runtime::max_merge_pages) &&
           (1 == spdk_ring_dequeue(allocate_req_queue, (void **) tmp, 1))) {
        
        if (res == nullptr)
            res = new _allocate::leader_res();
        
        auto* req = (_allocate::req*)tmp[0];
        if (base_ssd->allocator->allocate(res->span_list, req->batch)) {
            if (ldr)
                res->followers_req.push_back(req);  // 后续请求成为 follower
            else
                ldr = req;  // 第一个请求成为 leader
        }
    }
    
    ldr->cb(res);  // 只有 leader 收到合并后的结果
}
```

### 5.3 使用场景

```cpp
// apply.hh - 处理 leader/follower 结果
>> pump::sender::then([b](auto &&res) {
    if constexpr (std::is_same_v<std::decay_t<decltype(res)>, fs::_allocate::leader_res*>) {
        // Leader: 执行写入并通知 followers
        return pump::sender::just()
            >> write_data(res->span_list)
            >> write_meta(res)
            >> notify_follower(res);
    }
    else if constexpr (std::is_same_v<std::decay_t<decltype(res)>, fs::_allocate::follower_res>) {
        // Follower: 直接返回，等待 leader 完成
        return pump::sender::just(__fwd__(res));
    }
})
```

### 5.4 visit 算子：处理 variant 返回值

Leader/Follower 模式中，调度器返回 `std::variant<leader_res*, follower_res, failed_res>`，需要根据不同类型执行不同的业务逻辑。

#### 问题：运行时分支 vs 编译期分支

在 PUMP 框架中，`then` 算子的 lambda 返回的 sender 类型必须在**编译期确定**。如果直接使用运行时 `if-else`：

```cpp
// ❌ 错误：两个分支返回不同类型的 sender，无法编译
>> then([](auto res) {
    if (std::holds_alternative<leader_res*>(res))
        return write_data_sender{};  // 类型 A
    else
        return just_sender{};        // 类型 B，与 A 不同！
})
```

#### 解决方案：visit 算子

`visit` 算子将**运行时的值**（bool/variant/指针）转换为**编译期的类型**，使得后续可以使用 `if constexpr` 进行编译期分支选择：

```cpp
// ✅ 正确：使用 visit 将 variant 展开为具体类型
>> pump::sender::visit()
>> pump::sender::then([](auto&& res) {
    if constexpr (std::is_same_v<__typ__(res), leader_res*>) {
        // Leader 路径：执行写入
        return pump::sender::just() >> write_data(res->span_list);
    }
    else if constexpr (std::is_same_v<__typ__(res), follower_res>) {
        // Follower 路径：等待
        return pump::sender::just(__fwd__(res));
    }
    else {
        // 失败路径
        return pump::sender::just();
    }
})
>> pump::sender::flat()
```

#### visit 支持的类型转换

| 输入类型 | 转换结果 |
|---------|---------|
| `bool` | `true` → `std::true_type`，`false` → `std::false_type` |
| `std::variant<A, B, C>` | 使用 `std::visit` 展开，分别传递 `A`、`B`、`C` 类型的值 |
| 指针类型 `T*` | 非空 → 传递指针值，空 → 传递 `nullptr` |

#### 实际使用示例

**示例1：bool 条件分支（ycsb.hh 的 updt 函数）**

```cpp
>> then([](uint64_t i) { return (spdk_get_ticks() % 100) < 50; })  // 运行时 bool
>> visit()                                                          // 转为编译期类型
>> then([max](auto &&res) {
    if constexpr (std::is_same_v<__typ__(res), std::true_type>)
        return as_batch(random_kv(max) >> put() >> apply());  // put 操作
    else
        return as_batch(random_key(max) >> get());            // get 操作
})
```

**示例2：检查操作是否成功（apply.hh 的 cache_data_if_succeed）**

```cpp
return check_batch(b)           // 返回 bool：batch 是否有错误
    >> pump::sender::visit()    // 转为 std::true_type 或 std::false_type
    >> pump::sender::then([b](auto&& v){
        if constexpr (std::is_same_v<__typ__(v), std::true_type>)
            // 成功：缓存数据
            return pump::sender::just() >> pump::sender::generate(b->cache) >> ...;
        else
            // 失败：什么都不做
            return pump::sender::just();
    })
    >> pump::sender::flat();
```

#### 原理说明

`visit` 会为每种可能的类型分别实例化后续的 `then` lambda，每个实例化版本只有一个返回类型，因此可以编译通过。这是利用 C++ 模板实例化机制实现的**类型安全的运行时分支**。

---

## 6. 并发控制最佳实践

### 6.1 选择合适的并发数

| 场景 | 建议并发数 | 原因 |
|------|-----------|------|
| CPU 密集型 | 核心数 | 避免过多上下文切换 |
| IO 密集型 | 较大值（如 1000-10000） | 充分利用 IO 等待时间 |
| 内存受限 | 根据内存计算 | 避免 OOM |
| 无限制 | `concurrent()` 不带参数 | 由调度器自然控制 |

### 6.2 常见错误

```cpp
// ❌ 错误：concurrent 在 on(scheduler) 之后，无法控制并发
for_each(items)
    >> on(scheduler)
    >> concurrent(100)  // 此时已经调度，concurrent 无效
    >> then(...)

// ❌ 错误：没有调度，concurrent 无效
for_each(items)
    >> concurrent(100)
    >> then([](auto item) {
        return sync_process(item);  // 同步函数，实际串行执行
    })

// ✅ 正确：concurrent 在 on(scheduler) 之前
for_each(items)
    >> concurrent(100)
    >> on(scheduler)
    >> then(...)
```

### 6.3 嵌套并发

并发可以嵌套，形成复杂的并发拓扑：

```cpp
// 外层并发
for_each(batches)
    >> concurrent(1000)
    >> on(task_scheduler)
    >> then([](auto batch) {
        // 内层并发
        return for_each(batch.items)
            >> concurrent()
            >> on(nvme_scheduler)
            >> then([](auto item) { return write(item); })
            >> all();
    })
    >> flat()
    >> reduce();
```

---

## 7. 总结

### 7.1 核心要点

1. **`concurrent(N)` 只是声明**：声明允许最大 N 个并发，不产生真正并发
2. **调度器是驱动力**：`on(scheduler)` 将任务调度到执行，`advance()` 推进执行
3. **顺序很重要**：`for_each >> concurrent >> on(scheduler) >> then >> all/reduce`
4. **可以嵌套**：多级并发可以嵌套，形成复杂的并发拓扑
5. **合并优化**：Leader/Follower 模式可以合并请求，减少 IO 次数

### 7.2 快速参考

```cpp
// 基本并发模式
for_each(items)
    >> concurrent(N)           // 允许 N 个并发
    >> on(scheduler)           // 调度到 scheduler
    >> then(process)           // 业务逻辑
    >> all() / reduce()        // 收集结果

// 无限并发
for_each(items)
    >> concurrent()            // 无参数 = 无限并发
    >> on(scheduler)
    >> then(process)
    >> all()

// 嵌套并发
for_each(outer)
    >> concurrent(N)
    >> on(scheduler)
    >> then([](auto item) {
        return for_each(item.inner)
            >> concurrent()
            >> on(inner_scheduler)
            >> then(inner_process)
            >> all();
    })
    >> flat()
    >> reduce()
```
