PUMP 项目 AI 规范（面向智能体）

目的
- 本文面向 AI/自动化编程场景，提供项目常用模块索引、编码关注点与缺失信息清单。
- 与现有 `ai_spec/*` 和 `apps/kv/AI_GUIDE.md` 互补，避免重复细节。

快速概览（常用入口）
- 构建系统：`CMakeLists.txt`（C++26，依赖 nvme/net 子模块 cmake）。
- 核心库：`src/pump`（sender/pipeline/runtime/context）。
- 运行环境：`src/env`（scheduler 族与 runtime/share_nothing）。
- 主要应用：
  - KV：`apps/kv`（SPDK/NVMe 版 KV，已有详细 AI 指南）。
  - AISAQ：`apps/aisaq`（索引/读取示例应用）。
  - 示例：`apps/example/*`（hello/echo/nvme/share_nothing 等）。

常用模块与文件（优先阅读顺序）
1) Pump 编排层
   - `ai_spec/PUMP_SPEC.md`：核心抽象与运行模型。
   - `ai_spec/SENDERS_SPEC.md`：已实现 sender 语义与注意事项。
   - 代码目录：`src/pump/core/*`、`src/pump/sender/*`、`src/pump/coro/*`。
2) Scheduler 与运行环境
   - `ai_spec/SCHEDULER_SPEC.md`：scheduler 统一定义。
   - 代码目录：`src/env/scheduler/*`、`src/env/runtime/*`、`src/env/runtime/share_nothing.hh`。
3) KV 子系统
   - `apps/kv/AI_GUIDE.md`：结构/流程/扩展路径。
   - 入口：`apps/kv/main.cc`，启动流程：`apps/kv/senders/start_db.hh`。

编码与修改时的注意点（基于代码现状）
- sender 侧注意事项：`SENDERS_SPEC.md` 已标出未完成/需谨慎的 sender（如 `until` 空实现、`add_to_context` 未成型）。
- `CMakeLists.txt` 设定 C++26 与 `-O0`，默认用于调试；性能或发布构建需另行说明或改动。
- scheduler 通常是“队列 + advance 循环”模型，任何新 sender 若涉及调度切换要明确接入的 scheduler，并确保回调能继续推进 `op_pusher`。
- KV 系统依赖 NVMe/SPDK 与多 scheduler 协作，新增功能应优先复用现有 sender 组合，避免跨 scheduler 的隐式捕获与共享状态。
- `cmake-build-debug/` 在仓库中存在；脚本/自动化时避免误读该目录的编译产物当源码。
- `src` 目录下都是公共库文件,需要谨慎修改,如果修改需要注意解耦,抽象以及代码质量

推荐的 AI 工作方式
- 先读 `ai_spec/PUMP_SPEC.md` 与 `ai_spec/SENDERS_SPEC.md`，明确 sender 语义与 context 使用方式。
- 对 KV 相关改动，先读 `apps/kv/AI_GUIDE.md`，再定位具体 sender 与 scheduler。
- 若需新增 scheduler 或 sender，优先参考同类型实现，保证：
  - sender 的 value_type 推导正确；
  - 异常与 skip 通过 `op_pusher` 正确传播；
  - scope/context 的 push/pop 对称。
