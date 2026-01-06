SCHEDULER 定义（给智能体的说明）

什么是 scheduler
- 在本项目里，scheduler 是“按职责/资源划分的执行上下文”，负责接收请求并在自己的执行循环中推进任务。
- 你可以把它理解为：有队列 + 事件循环的执行体，通过回调推进 pump 流水线。
- 它不是通用线程池，而是面向具体领域（task / nvme / net）的执行者。

代码中的共同特征
- scheduler 通常维护请求队列（req），并在 `advance()` 中处理队列与事件。
- sender/op 把“切到某个 scheduler”嵌入 pipeline，触发时将 req 入队。
- 请求完成时通过回调继续 `op_pusher`，把结果推回流水线。

当前仓库里的 scheduler 类型
- task scheduler（`src/env/scheduler/task/tasks_scheduler.hh`）：
  - 通用任务调度 + 定时器；有 preemptive 队列支持。
  - sender：`as_task()` / `as_preemptive_task()` / `delay(ms)`。
- nvme scheduler（`src/env/scheduler/nvme/*.hh`）：
  - 面向 NVMe 设备的读写调度，基于 SPDK 队列。
  - sender：`get(page)` / `put(page)`。
- net scheduler（`src/env/scheduler/net/*`）：
  - 网络连接/读写调度；实现包含 epoll 与 io_uring 两套。
  - sender：`wait_connection()` / `join()` / `recv()` / `send()` / `stop()`。

运行方式（环境）
- `src/env/runtime/share_nothing.hh` 提供多核/多线程的 run/start 模式。
- 应用层（如 KV 项目）通过 `apps/kv/runtime/scheduler_objects.hh` 管理 scheduler 实例集合。

如何在编程时使用这个定义
- 当你需要“把一段逻辑安排到某个资源域里执行”，就选对应 scheduler 的 sender。
- 典型用法：`pipeline >> on(task_scheduler.as_task()) >> ...` 或直接调用该 scheduler 的 sender。
- scheduler 的选择体现了“这段逻辑由谁推进”；跨域时显式插入调度点。

一句话总结
- scheduler 是“按职责划分的执行上下文”，以队列+事件循环驱动 pump 流水线在指定资源域内推进。
