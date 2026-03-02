# Sender 详细语义

> 本文档补充 CLAUDE.md 速查表未充分说明的 sender。简单 sender（just, then, transform 等）仅看 CLAUDE.md 即可。

## 变换与展开

### flat()
- **输入**: `sender` 或 `variant<sender...>`
- **输出**: 被展开 sender 的输出值
- **机制**: 创建 `runtime_scope(type=other)`，子 sender 执行完后通过 `push_value` 回接父 scope
- **注意**: variant 输入时每个分支分别展开

### flat_map(f)
- 等价于 `transform(f) >> flat()`
- f 必须返回 sender

### forward_value(...)
- 忽略上游值，显式传递给定值到下游
- 常用于分支处理和错误恢复中重新组织输出

### on(sender)
- **输入**: 上游值（被丢弃）
- **输出**: 目标 sender 的输出
- **机制**: 类似 flat，但目标 sender 来自参数而非上游值
- 常用模式: `>> on(sched.as_task())` 切换执行域

## 控制与条件

### maybe()
- **输入**: `std::optional<T>`
- **输出**: 有值 → `T`（push_value）；无值 → push_skip
- skip 会跳过后续 op 直到遇到 `when_skipped` 或流结束

### maybe_not()
- 与 maybe 相反：有值 → push_skip；无值 → push_value（空值）

### when_skipped(f)
- **输入**: skip 信号
- **输出**: `f(context)` 返回的 sender 的输出
- f 接收 context 引用，必须返回 sender
- 机制: 创建新 runtime_scope 执行 f 返回的 sender，结果回接原流

### visit()
- **输入/输出转换**:

| 输入类型 | 输出 | 说明 |
|---------|------|------|
| `bool` | `std::true_type` 或 `std::false_type` | 编译期分支 |
| `variant<A,B,...>` | 分别传递 A、B、... | 每种类型分别实例化后续 lambda |
| `T*` | 非空 → `T*`；空 → `std::nullptr_t` | 指针有效性分支 |

- **原理**: 为每种可能类型分别实例化后续 lambda，使 `if constexpr` 可用
- `visit(value)` 变体会把额外的 value 一起传递到下游
- 后续必须 `>> flat()` 因为各分支可能返回不同 sender

## 并发与顺序

### concurrent(max)
- **前置要求**: 必须在 `on(scheduler)` 之前
- **单独使用时串行**: 只声明允许并发，不产生真正并发
- **真正并发** = `concurrent(N)` + `on(scheduler)`
- `concurrent()` 无参数 = 无限并发
- 内部用 `concurrent_counter` 追踪 pending/completed

### sequential()
- 对流式元素严格顺序处理
- 内部使用 `source_cache` 队列缓存元素

### with_concurrent(bind_back)
- 等价于 `concurrent() >> bind_back >> sequential()`
- 内部并发处理，外部恢复顺序

## 聚合

### when_all(...)
- **输入**: 多个 sender（作为参数）
- **输出**: `when_all_res` 结构（每个 sender 结果为 `variant<monostate, exception_ptr, T>`）
- 内部创建 `collector_wrapper` 管理各 sender 的结果收集
- 所有 sender 完成后才向下游推进

### when_any(...)
- **输入**: 多个 sender（作为参数），所有分支**必须产生相同类型**
- **输出**: `(uint32_t winner_index, variant<monostate, exception_ptr, T> result)`
  - `winner_index`: 最先完成的分支索引（从0开始）
  - `result.index() == 2`: 成功值（`std::get<2>(result)` 获取）
  - `result.index() == 1`: 异常（`std::get<1>(result)` 获取 `exception_ptr`）
  - `result.index() == 0`: done/skip 信号
- **机制**: 内部创建 `race_wrapper`，用原子操作（FINISHED_BIT + ref_count 打包到单个 `uint64_t`）实现无锁竞争。第一个完成的分支胜出，其他分支的结果被丢弃
- **约束**: 所有分支必须返回相同类型（编译期 `static_assert` 检查）
- **用法**:
```cpp
just() >> when_any(
    sched1->as_task() >> then([]() { return fast_path(); }),
    sched2->as_task() >> then([]() { return slow_path(); })
) >> then([](uint32_t winner_index, auto result) {
    if (result.index() == 2) {
        auto value = std::get<2>(result);
        // 使用 value
    }
});
```

### reduce(init, f)
- **输入**: 流式元素
- **输出**: 累积结果
- `reduce()` 无参数时等待所有流元素完成（等价于 `all()`）

### to_container\<C\>() / to_vector\<T\>()
- 将流式元素收集进容器

## 流式生产

### for_each(range)
- 创建 `stream_starter` scope
- 对 viewable_range 中每个元素推进子 pipeline
- 可与 `concurrent`/`sequential` 组合

### repeat(n) / forever()
- `repeat(n)` 重复 n 次（基于 for_each）
- `forever()` 无限重复

## 异常处理

### any_exception(f)
- **输入**: `std::exception_ptr`
- f 必须返回 sender（用于恢复）
- 返回的 sender 输出替换原异常路径

### catch_exception\<T\>(f)
- 只捕获类型 T 的异常，其他异常继续传播
- f 接收 `T&` 引用

### ignore_inner_exception(bind_back)
- 包裹子流程：子流程内异常被转为 `just()`，外层不受影响

## 协程桥接

### await_able() / await_able(ctx)
- 将 sender pipeline 转为 `co_await` 可用的 awaitable
- 无参版本需要外部 context；有参版本自带 context
- **禁止**在协程中用 `submit` 取结果，必须用 `await_able`

## 不稳定/未完成 Sender

| Sender | 状态 |
|--------|------|
| `until` | 文件为空，未实现 |
| `add_to_context` | 仅有 op 定义，无完整 sender |
| `any_case` | 内部调用 `_any_exception::sender`，行为与名称不一致 |
| `get_op_flow()` | 调试用，`compute_sender_type` 推导可能异常 |
