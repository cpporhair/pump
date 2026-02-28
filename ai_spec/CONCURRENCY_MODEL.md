# PUMP 并发模型

## 1. concurrent ≠ 并发

`concurrent(N)` 只声明允许N个并发，不产生真正并发。真正并发需要：
1. `concurrent(N)` 声明并发数
2. `on(scheduler)` 调度到scheduler执行

| 情况 | 结果 |
|------|------|
| 只有concurrent，无调度 | **串行**（同步函数调用） |
| 只有调度，无concurrent | **串行**（等逻辑返回才下一个） |
| concurrent + 调度 | **真正并发** |

## 2. 正确模式

```cpp
for_each(items)
    >> concurrent(N)                // 声明并发（必须在on之前）
    >> on(scheduler.as_task())      // 调度（真正并发开始）
    >> then(process)                // 业务逻辑
    >> all() / reduce()             // 收集结果
```

无限并发：`concurrent()` 不带参数。

参考：`apps/example/concurrent/main.cc`

## 3. 调度器是并发驱动力

每个核心运行主循环，轮询调用各scheduler的`advance()`推进任务。

```cpp
while (running) {
    task_scheduler->advance();
    batch_scheduler->advance();
    nvme_scheduler->advance();
}
```

调度方式：
- `>> on(scheduler->as_task())`
- `>> on(pump::scheduler::task::schedule_at(scheduler))`
- `>> on(pump::scheduler::task::schedule_at(pump::scheduler::task::any_scheduler(list)))`

## 4. 多级并发架构

```
第1层: generate_on >> concurrent(10000) >> as_batch(...)      ← 10000并发batch
  第2层: for_each(cache) >> concurrent() >> index::update      ← batch内并发
    第3层: for_each(spans) >> concurrent() >> nvme::put_span   ← span并发
      第4层: as_stream(pages) >> concurrent() >> put_page      ← page并发
        第5层: NVMe scheduler异步IO
```

嵌套并发：
```cpp
for_each(outer) >> concurrent(N) >> on(sched) >> then([](auto item) {
    return for_each(item.inner) >> concurrent() >> on(inner_sched) >> then(process) >> all();
}) >> flat() >> reduce();
```

## 5. Leader/Follower 合并

多个并发请求合并，leader统一执行，follower等待。scheduler返回`variant<leader_res*, follower_res, failed_res>`。

```cpp
// apply.hh 处理方式
>> visit()  // variant展开为具体类型
>> then([](auto&& res) {
    if constexpr (std::is_same_v<__typ__(res), leader_res*>)
        return just() >> write_data(res->span_list) >> notify_follower(res);
    else if constexpr (std::is_same_v<__typ__(res), follower_res>)
        return just(__fwd__(res));
}) >> flat()
```

## 6. visit 算子

将运行时值提升为编译期类型，使后续可用`if constexpr`分支。

| 输入 | 转换 |
|------|------|
| `bool` | `std::true_type` / `std::false_type` |
| `variant<A,B,C>` | 分别传递A、B、C |
| `T*` | 非空→传指针, 空→传`nullptr` |

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

原理：为每种类型分别实例化后续lambda，每个实例化只有一个返回类型。

## 7. 并发数选择

| 场景 | 建议 |
|------|------|
| CPU密集 | 核心数 |
| IO密集 | 1000-10000 |
| 内存受限 | 按内存计算 |
| 无限制 | `concurrent()` 无参数 |

**关键：concurrent必须在on(scheduler)之前。**
