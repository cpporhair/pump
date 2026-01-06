PUMP 库定义（给智能体的说明）

目标
- pump 是一个“可组合异步流程”的管道库，用于把业务步骤组织成可执行的 sender/op 流水线。
- 它提供 sender 组合、运行时 scope/context 管理、异常/跳过处理，以及与各类 scheduler 的衔接。

核心抽象
- sender：描述“可产生值/事件的流程片段”，支持通过 `connect<context_t>()` 构建 op 列表。
- op：sender 的执行节点，通过 `op_pusher` 驱动向后推进（push_value / push_exception / push_skip / push_done）。
- pipeline：通过 `>>` 串联 sender/bind_back，形成流水线。
- scope：运行期的 op 列表 + 层级关系（root / stream_starter / other）。
- context：可叠加的运行期数据环境（root_context / pushed_context / when_all_context）。

运行模型（简化）
- `submit(context)` 会把 sender 转成 op 列表，创建 root_scope，并从 pos=0 开始推送。
- 每个 op 通过 `op_pusher` 把结果推到下一个 op；异常/跳过同样走 `op_pusher` 的对应入口。
- `flat`/`flat_map` 会创建新的 runtime_scope，把子 sender 的结果回接到父 scope。
- `for_each`/`generate`/`stream` 会创建 stream_starter scope，把一个范围拆成流式处理。

常用 sender（与代码一致）
- 起点：`just(...)`、`submit(context)`。
- 变换：`then(f)`、`transform(f)`、`flat()`、`flat_map(f)`。
- 并发/顺序：`concurrent(max)`、`sequential()`、`with_concurrent(...)`。
- 聚合：`when_all(...)`、`reduce(...)`、`to_container(...)`。
- 条件/跳过：`maybe()`、`maybe_not()`、`when_skipped(f)`、`visit()`。
- 异常：`any_exception(f)`、`catch_exception<T>(f)`、`ignore_all_exception()`。
- 流式：`for_each(range)`、`generate(range)`、`repeat(n)`。
- context：`push_context(...)` / `push_result_to_context()`、`pop_context()`、`get_context<T...>()`。
- 调度：`on(sender)`（把当前值转交给某个 scheduler 的 sender）。

值与多值
- sender 的 value_type 由 `compute_sender_type` 推导；支持单值、多值（tuple）与无值。
- `then` 根据返回值是否为 void 决定是否向后传值。
- `flat`/`flat_map` 期望上一步返回 sender（或 variant<sender...>）。

与 scheduler 的关系
- scheduler 只是执行域，真正的编排与链式组合由 pump 负责。
- scheduler 对外提供 sender（如 task::as_task / nvme::get / net::recv），把“调度点”嵌入流水线。
- `on(scheduler_sender)` 将当前值转交给指定 scheduler 的 sender。

使用建议（给未来编码）
- 优先把复杂流程拆成小 sender，再用 `>>` 组合。
- 需要跨执行域时，显式插入对应 scheduler 的 sender（或 `on(...)`）。
- 用 `push_context`/`get_context` 管理跨步骤共享数据，避免大范围捕获。

一句话总结
- pump 是“异步流程编排层”：用 sender/op/pipeline 组织任务，用 scope/context 管理运行期，用 scheduler sender 选择执行域。
