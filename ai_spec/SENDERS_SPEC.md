PUMP Sender 说明（给智能体的编程参考）

说明范围
- 本文覆盖 `src/pump/sender` 下已实现的 sender，以及常见的辅助函数/别名。
- 描述基于当前代码实现，强调“输入/输出值”“控制流”和“使用注意事项”。

基础与提交

1) just(...)
- 作用：生成起始 sender，直接产出指定值（或空值）。
- 输入：任意值；无参数时表示“无值起点”。
- 输出：单值或多值（内部以 tuple_values 处理）。
- 典型用法：`just(1,2,3) >> then(...)`；或 `just() >> then(...)`。

2) submit(context)
- 作用：启动执行；把 sender 连接成 op 列表并从 pos=0 推进。
- 输入：root_context 或 pushed_context 之类的 context；可选 receiver（默认 null_receiver）。
- 输出：无（副作用是启动 pipeline）。
- 注意：`submit(context, receiver)` 会把 receiver 作为最后一个 op。

3) null_receiver
- 作用：作为默认 receiver，消化终止值/完成信号。
- 用法：一般由 `submit(context)` 自动使用。

协程桥接

- await_able() / await_able(context)
  - 作用：把 sender 转成可 `co_await` 的 awaitable。
  - 用法：`co_await (sender >> await_able())`，或 `co_await (sender >> await_able(context))`。
  - 注意：需要显式调用，避免隐式 await_transform。

变换与展开

4) then(f)
- 作用：对上一步输出做同步变换。
- 输入：上一步 value；若上一步无值则调用 `f()`。
- 输出：`f` 返回值；若 `f` 返回 void，则向后推“无值”。
- 注意：异常会通过 `push_exception` 继续传播。

5) transform(f)
- 作用：`then` 的语义包装，等价于 `then([...] { return f(...); })`。
- 适用：更强调“值变换”语义。

6) ignore_args / ignore_results
- 作用：忽略上一步所有参数，不向后传值。
- 输入：任意；输出：无值。

7) forward_value(...)
- 作用：显式把给定值向后传递（忽略前值）。
- 适用：在 `flat_map` 或错误处理里重新组织输出。

8) flat()
- 作用：把“值为 sender”的结果展开为实际执行流。
- 输入：`sender` 或 `variant<sender...>`。
- 输出：被展开 sender 的输出。
- 注意：会创建新的 runtime_scope，并在结束后回接父 scope。

9) flat_map(f)
- 作用：`transform(f) >> flat()` 的组合。
- 输入：上一步 value；输出：`f` 返回的 sender 的输出。

调度与切换

10) on(sender)
- 作用：把当前值转交给另一个 sender（常用于切换 scheduler）。
- 输入：上一步 value。
- 输出：目标 sender 的输出。
- 典型用法：`... >> on(task.as_task()) >> ...`。

控制与条件

11) maybe()
- 作用：处理 `std::optional<T>`；有值则向后推值，否则 `push_skip`。
- 输入：`std::optional<T>`。
- 输出：`T` 或 skip。

12) maybe_not()
- 作用：与 maybe 相反；无值时向后推“空值”，有值则 skip。
- 输入：`std::optional<T>`。
- 输出：无值或 skip。

13) when_skipped(f)
- 作用：当上一步触发 skip 时执行 `f` 生成的 sender。
- 输入：skip；`f(context)` 需返回 sender。
- 输出：`f` 返回 sender 的输出，并回接原流水线。

14) visit([value])
- 作用：将运行时的条件值（bool/指针/variant）提升为编译时的类型分支形态。
- 输入：bool / 指针 / variant 等。
- 输出：`std::true_type`/`std::false_type` 或原类型与其替代类型（如 variant 的具体分量类型）。
- 目的：允许在后续 `then` 算子中使用 `if constexpr` 针对不同类型进行分支处理，解决异步流程中异构 Sender 返回值的编译问题。
- 注意：有 value 版本会把 value 一并向后传。
- 原理：`visit` 为每种可能的类型分别实例化后续 lambda，每个实例化版本只有一个返回类型，因此可以编译通过。
- **详细说明与示例**：请参阅 [CONCURRENCY_MODEL.md](./CONCURRENCY_MODEL.md) 第 5.4 节「visit 算子：处理 variant 返回值」，包含 bool 条件分支、Leader/Follower 模式等完整示例。

异常处理

15) any_exception(f)
- 作用：捕获异常并转成 sender 继续执行。
- 输入：`std::exception_ptr`。
- 输出：`f` 返回 sender 的输出（可用于错误恢复）。

16) catch_exception<T>(f)
- 作用：只处理指定类型异常；内部基于 `any_exception`。
- 输入：异常；输出：`f(T&)` 返回 sender。

17) ignore_all_exception()
- 作用：吞掉所有异常，转为 `just()`（无值继续）。
- 输出：无值。

18) ignore_inner_exception(bind_back)
- 作用：在内部子流程中忽略异常，外层继续执行。
- 用法：`flat_map` 组合，内部将异常变为 `just()`。

并发与顺序流

19) concurrent(max)
- 作用：将“流式产生的元素”并发处理，限制最大并发。
- 输入：来自 `for_each`/`generate` 这类流。
- 输出：每个流元素的处理结果（并发推进）。
- 注意：内部使用 `concurrent_counter` 追踪完成与 pending。

20) sequential()
- 作用：对流式元素顺序处理。
- 输入：流式元素。
- 输出：按顺序推进处理结果。
- 注意：内部会把元素缓存到队列（source_cache）。

21) with_concurrent(bind_back)
- 作用：简写 `concurrent() >> bind_back >> sequential()`。
- 适用：并发处理子步骤，再回到顺序流。

聚合与归约

22) when_all(...)
- 作用：并行/并发收集多个 sender 的结果，汇成聚合值。
- 输入：多个 sender。
- 输出：`when_all_res` 结构（详见 `when_all.hh` 的类型推导）。
- 注意：内部创建 when_all_context，统一管理结果。

23) reduce(init, f)
- 作用：对流式元素进行累积归约。
- 输入：初始值 + 归约函数。
- 输出：归约后的结果。

24) to_container<Container>()
- 作用：把流式元素收集进容器。
- 输出：容器实例。

流式生产

25) for_each(range)
- 作用：把 range 拆成元素流，并对每个元素推进子流水线。
- 输入：可迭代范围（viewable_range）。
- 输出：流式元素（作为后续 sender 的输入）。

26) generate / stream / range
- 作用：`for_each` 的别名；语义强调“生成/流式化”。
- 用法：`generate(r)`、`stream(r)` 等。

27) loop(n)
- 作用：产生 `0..n-1` 的流式序列。

28) repeat(n) / forever()
- 作用：基于 `for_each` 生成固定次数或无限序列。

上下文相关

29) push_context(...)
- 作用：把数据压入 context（新建 pushed_context）。
- 输入：要入栈的数据，或把当前值作为 context 内容（`push_result_to_context()`）。
- 输出：不改变当前值流，只改变 context。
- 注意：内部通过编译期 id 区分不同 push 层级。

30) pop_context()
- 作用：弹出最近的 pushed_context（恢复 base_context）。
- 输出：与上一步相同的值，但 context 发生回退。

31) get_context<T...>()
- 作用：从 context 中按类型读取引用。
- 输出：`tuple<T&...>`，并与当前值拼接形成多值输出。

32) get_full_context_object()
- 作用：获取完整 context 对象本身（推为一个值）。
- 输出：`context_t&` 或 `compute_context_type` 的引用。

调试/流程辅助

33) get_op_flow()
- 作用：把当前 scope（op 流）作为值向后传。
- 输出：`scope` 作为一个值，常用于调试或自定义流程控制。
- 注意：`compute_sender_type` 的推导逻辑可能因特定 sender 的 `value_type` 定义不全而出现异常。

未完成或需注意的 sender

- add_to_context：仅定义了 op，未提供完整 sender/接线（可能是未完成的实验功能）。
- any_case：实现中调用了 `_any_exception::sender`，行为与名称不一致，谨慎使用。
- until：文件为空（未实现）。

使用建议（编程侧重点）
- 在编排上优先用 `just/then/flat_map` 组合，再用 `for_each/concurrent/sequential` 处理流。
- 需要跨 scheduler 时用 `on(...)` 或直接插入 scheduler sender。
- 用 `push_context/get_context` 传递跨步骤状态，减少 lambda 捕获。
- 对异常处理统一走 `any_exception` 或 `catch_exception<T>`，避免在 then 内部吞异常。
