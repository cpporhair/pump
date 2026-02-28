# RPC 层实现计划

基于 `command.md`（需求）和 `rpc_design.md`（设计），结合现有 Net 层 / Task Scheduler / PUMP 框架代码的实现计划。

---

## 总览

共 4 个阶段、18 个步骤。每步产出可编译的增量代码。

```
Phase 1: 核心骨架（Step 1-6）   → 基础类型、数据结构              ✅ 已完成
Phase 2: Sender 实现（Step 7-10） → call / notify / serve + 对外头文件 ✅ 已完成
Phase 3: 集成测试（Step 11-14）   → echo 示例 + 功能测试            ✅ 已完成
Phase 4: 优化扩展（Step 15-18）   → 心跳、优雅关闭、pool allocator、静态 dispatcher 优化  ⬚ 未开始
```

### 实现过程中的重要修复

| 问题 | 位置 | 修复 |
|------|------|------|
| timer timestamp 未传递给 op | `tasks_scheduler.hh` `_timer::sender::make_op()` | 传递 timestamp 到 op |
| `\|\|` fold 短路导致 scheduler 未全部 advance | `env::runtime::run()` | 测试中使用 comma-fold advance 循环 |
| send_message 中 encode_result::inline_buf 是栈变量 | `channel.hh` `send_message()` | 数据线性化到堆 buffer |
| handle_stop_request 关闭 fd 未通知 pending recv | `io_uring/scheduler.hh` | 关闭 fd 前 exchange+通知 recv callback |

目标目录结构：

```
src/env/scheduler/rpc/
├── rpc.hh                      # 对外统一头文件
├── common/
│   ├── header.hh               # RPC 固定头定义与解析
│   ├── message_type.hh         # 消息类型枚举
│   ├── error.hh                # RPC 错误类型
│   ├── codec_concept.hh        # Codec concept + payload_view + encode_result + linearize 辅助
│   └── config.hh               # channel_config
├── channel/
│   ├── channel.hh              # RPC Channel 核心结构 + make_channel
│   ├── pending_map.hh          # 请求-响应关联表（open-addressing 固定数组）
│   └── dispatcher.hh           # 静态分发器（service<sid> 模板 + visit dispatch）
├── senders/
│   ├── call.hh                 # rpc::call sender + op_pusher + compute_sender_type
│   ├── notify.hh               # rpc::notify sender
│   ├── serve.hh                # rpc::serve 接收循环
│   └── reply.hh                # 内部：encode + send response 辅助
└── codec/
    └── raw_codec.hh            # 默认 trivially-copyable Codec
```

---

## Phase 1: 核心骨架 ✅

### Step 1 — 消息类型枚举 + RPC Header ✅

**产出文件**：
- `src/env/scheduler/rpc/common/message_type.hh`
- `src/env/scheduler/rpc/common/header.hh`

**内容**：

1. `message_type.hh`：定义 `pump::rpc::message_type` 枚举（request=0x01, response=0x02, notification=0x03, error=0x04）

2. `header.hh`：
   - `rpc_header` 结构体：`type`(1B) + `request_id`(4B) + `method_id`(2B)，`static constexpr size = 7`
   - `parse_header(const uint8_t* data) -> rpc_header`：用 `memcpy` 解析，避免对齐问题
   - `write_header(uint8_t* buf, const rpc_header& h)`：将 header 写入 buffer
   - 本机字节序（小端），无需转换（内部 RPC 通信）

**参考**：`rpc_design.md` 2.1-2.3 节

**注意**：
- header 大小 7 字节，足够小可内联到 iovec[0]
- `parse_header` 使用 `memcpy` 而非 `reinterpret_cast`，避免 UB

---

### Step 2 — 错误类型定义 ✅

**产出文件**：`src/env/scheduler/rpc/common/error.hh`

**内容**：

1. `error_code` 枚举：success / method_not_found / remote_error / codec_error / timeout / internal_error

2. 异常类型（均继承 `std::runtime_error`）：
   - `rpc_timeout_error`（携带 request_id）
   - `method_not_found_error`（携带 method_id）
   - `remote_error`（携带 error_code + message）
   - `connection_lost_error`
   - `channel_closed_error`
   - `pending_overflow_error`

3. Error payload 格式辅助：
   - `parse_error_payload(const uint8_t* data, size_t len) -> {error_code, message}`
   - `write_error_payload(uint8_t* buf, error_code, const char* msg) -> size_t`

**参考**：`rpc_design.md` 2.4 节、`command.md` 七章

---

### Step 3 — Codec 抽象 + payload_view + Raw Codec ✅

**产出文件**：
- `src/env/scheduler/rpc/common/codec_concept.hh`
- `src/env/scheduler/rpc/codec/raw_codec.hh`

**内容**：

1. `codec_concept.hh`：
   - `payload_view` 结构体：`const iovec* vec` + `uint8_t cnt`(1或2) + `size_t len`
   - `encode_result` 结构体：`iovec vec[4]` + `size_t cnt` + `size_t total_len` + `uint8_t inline_buf[64]` + `uint8_t* heap_buf`（析构时 `delete[]`）
   - `linearize_payload(payload_view, tmp_buf, tmp_buf_size) -> const uint8_t*`：连续时零拷贝返回指针，跨边界时拷贝到 `tmp_buf`
   - `rpc_codec` concept（可选，约束 `decode_payload<T>` 和 `encode_payload`）

2. `raw_codec.hh`：
   - `raw_codec` 结构体：适用于 `trivially_copyable` 类型
   - `decode_payload<T>(payload_view)` → `T`：用 `linearize_payload` + `memcpy`
   - `encode_payload(const T&)` → `encode_result`：`memcpy` 到 `inline_buf`（`static_assert sizeof(T) <= 64`）

**与 Net 层的交互**：
- `payload_view` 的 `iovec` 直接来自 `packet_buffer::handle_data` / `get_recv_pkt` 返回的 `pkt_iovec`
- 需要确认 `pkt_iovec`（`detail.hh` 中定义）的结构，据此构造 `payload_view`
  - `pkt_iovec` 包含 `iovec iov[2]` + `int cnt` + `size_t len`
  - `payload_view` 可直接从 `pkt_iovec` 偏移 `rpc_header::size` 构造

**参考**：`rpc_design.md` 3.1-3.3 节

---

### Step 4 — Pending Request Map ✅

**产出文件**：`src/env/scheduler/rpc/channel/pending_map.hh`

**内容**：

1. `pending_slot` 结构体：
   - `state` 枚举（empty / active）
   - `request_id: uint32_t`
   - `deadline_ms: uint64_t`（0 表示无超时）
   - `cb: std::move_only_function<void(std::variant<payload_view, std::exception_ptr>)>`

2. `pending_map<MaxPending=1024>` 结构体：
   - `std::array<pending_slot, MaxPending> slots`
   - `uint32_t active_count`
   - `insert(request_id, deadline_ms, cb) -> std::optional<uint32_t>`：`request_id % MaxPending` 起始，线性探测找空 slot
   - `remove(request_id) -> std::optional<cb_type>`：线性探测查找并移除
   - `check_timeouts(now_ms)`：全表扫描，超时的执行 `cb(exception_ptr)`
   - `fail_all(exception_ptr)`：所有 active slot 执行 `cb(e)`，`active_count = 0`

**设计要点**：
- 固定数组预分配，cache 友好
- open-addressing + 线性探测，`request_id` 单调递增分布均匀
- 单线程操作（session 所在 core），无锁
- `MaxPending` 通常 1024，线性扫描超时可接受

**参考**：`rpc_design.md` 4.1-4.3 节

---

### Step 5 — 静态分发器（Dispatcher） ✅

**产出文件**：`src/env/scheduler/rpc/channel/dispatcher.hh`

**内容**：

1. `handler_context` 结构体（v1 统一上下文类型）：
   - `uint32_t request_id`
   - `uint16_t service_id`（即 method_id，一级路由）
   - `payload_view payload`

2. `service<auto sid>` 基模板：`static constexpr bool is_service = false`

3. `has_handle<T, Req>` concept：检查 `T::handle(Req&&)` 是否存在

4. `get_service_by_id<service_ids...>(sid) -> variant<service<s1>, service<s2>, ...>`：
   - 运行时 `sid` → `std::variant`
   - 未匹配抛 `method_not_found_error`

5. `dispatch<service_ids...>()` 算子：
   - 返回 `flat_map`，上游输入 `handler_context`
   - 内部：`just() >> visit(get_service_by_id<...>(ctx.service_id)) >> flat_map([](auto&& svc) { ... })`
   - 每个 `service<sid>` 分支用 `if constexpr` 检查 `is_service` + `has_handle`，调用 `svc_t::handle(ctx)`

**v1 策略**：统一 `handler_context` 类型，Service 内部完成 payload 解码和二级路由（switch/if constexpr）。后续可迭代为策略 B（类型化请求 + 二级 visit）。

**注意**：
- 依赖 `pump/sender/visit.hh`、`pump/sender/flat.hh`、`pump/sender/then.hh`
- 每个 `service<sid>::handle()` 必须返回 sender（同步用 `just()` 包装）

**参考**：`rpc_design.md` 5.1-5.6 节

---

### Step 6 — Channel 结构 + make_channel + config ✅

**产出文件**：
- `src/env/scheduler/rpc/common/config.hh`
- `src/env/scheduler/rpc/channel/channel.hh`

**内容**：

1. `config.hh`：
   - `channel_config` 结构体：`max_pending`(1024)、`default_timeout_ms`(5000)、`enable_heartbeat`(false)、`heartbeat_interval_ms`(1000)、`heartbeat_max_miss`(3)

2. `channel.hh`：
   - `channel_status` 枚举：active / draining / closed
   - `channel<Codec, SessionScheduler, TaskScheduler, ProtocolState=void>` 结构体：
     - 身份：`session_id`、`session_sched*`、`task_sched*`
     - RPC 状态：`next_request_id`(从1递增)、`status`、`pending`、`_codec`
     - 协议状态：`[[no_unique_address]] ProtocolState protocol_state`
     - 配置：`channel_config config`
     - 方法：`alloc_request_id()`、`codec()`
     - 发送辅助：`send_response()`、`send_error()`、`send_notification()`
       - 构造 `[rpc_header | payload]` 的 iovec
       - 通过 `new send_req{...}` + `prepare_send_vec` + `session_sched->schedule` 发送
       - `encode_result` 通过 move 捕获到 send callback 中保持生命周期
     - 连接管理：`on_connection_lost(e)`、`close()`
   - `make_channel<Codec, SessionScheduler, TaskScheduler, ProtocolState=void>(sid, session_sched, task_sched, config)` 工厂函数
     - 返回 `std::shared_ptr<channel_t>`（recv_loop / pending callbacks / 用户 pipeline 共享所有权）

**与 Net 层交互**：
- `send_response` / `send_error` / `send_notification` 内部都调用 Net 层的 `send_req` + `prepare_send_vec` 模式
- 参考 `src/env/scheduler/net/common/struct.hh` 中 `send_req` 和 `prepare_send_vec` 的实现

**参考**：`rpc_design.md` 6.1-6.4 节

---

## Phase 2: Sender 实现 ✅

### Step 7 — rpc::call Sender ✅

**产出文件**：`src/env/scheduler/rpc/senders/call.hh`

**内容**：

遵循 PUMP 自定义 scheduler sender 模式（参考 `net/senders/recv.hh`、`task/tasks_scheduler.hh`）：

1. `_call::req<channel_ptr_t>` 结构体（概念上的请求，实际 call 的等待通过 pending_map 实现，不需要独立的 req 入队到 scheduler）

2. `_call::op<channel_ptr_t>` 结构体：
   - 类型标记：`constexpr static bool rpc_call_op = true`
   - 成员：`ch`、`method_id`、`encoded_payload`、`timeout_ms`
   - `start<pos>(ctx, scope)`：
     1. 检查 `ch->status != active` → `push_exception(channel_closed_error)`
     2. `alloc_request_id()` 生成 rid
     3. 计算 deadline（`now_ms() + timeout_ms`，0 表示无超时）
     4. 注册到 `ch->pending.insert(rid, deadline, callback)`
        - callback 闭包捕获 `ctx` 和 `scope`
        - 收到 `payload_view` → `op_pusher<pos+1>::push_value(ctx, scope, pv)`
        - 收到 `exception_ptr` → `op_pusher<pos+1>::push_exception(ctx, scope, e)`
     5. 插入失败 → `push_exception(pending_overflow_error)`
     6. 构造 `[rpc_header(request) | payload]` iovec
     7. `new send_req{...}` + `prepare_send_vec` + `ch->session_sched->schedule` 发送

3. `_call::sender<channel_ptr_t>` 结构体：
   - 成员：`ch`、`method_id`、`encoded_payload`、`timeout_ms`
   - `make_op()` → `op<channel_ptr_t>{...}`
   - `connect<ctx_t>()` → `op_list_builder<0>().push_back(make_op())`

4. `op_pusher` 特化（`pump::core` 命名空间）：
   - `requires ... && get_current_op_type_t<pos, scope_t>::rpc_call_op`
   - `push_value(ctx, scope)` → `std::get<pos>(...).template start<pos>(ctx, scope)`

5. `compute_sender_type` 特化：
   - `count_value() = 1`
   - `value_type = payload_view`（调用方负责解码）

6. 对外 API：
   - `rpc::call(ch, method_id, encode_result&&, timeout_ms=0)` → `_call::sender{...}`
   - `rpc::call<Request>(ch, method_id, const Request&, timeout_ms=0)` → 先 `ch->codec().encode_payload(req)` 再构造 sender

**关键区别**（vs Net sender）：
- call 的"等待"不是通过 scheduler 队列，而是通过 `pending_map`
- 真正的唤醒发生在 `serve` 的接收循环中（recv_loop 收到 Response 后执行 pending callback）
- send 是 fire-and-forget（发送失败由连接断开检测处理）

**参考**：`rpc_design.md` 8.1-8.3 节

---

### Step 8 — rpc::notify Sender ✅

**产出文件**：`src/env/scheduler/rpc/senders/notify.hh`

**内容**：

notify 是 fire-and-forget，不进入 pending_map，不需要自定义 op（设计决策 D5）。

1. `rpc::notify(ch, method_id, encode_result&&)` → 返回 sender：
   ```
   just() >> then([ch, method_id, payload=move(payload)]() mutable {
       ch->send_notification(method_id, std::move(payload));
   })
   ```

2. 便捷版本：`rpc::notify<Message>(ch, method_id, const Message&)` → 先 encode 再调上面的版本

**注意**：`send_notification` 内部构造 `rpc_header(notification, 0, method_id)` + payload iovec → `send_req` → `prepare_send_vec` → `schedule`

**参考**：`rpc_design.md` 8.4 节

---

### Step 9 — rpc::serve 接收循环 + reply 辅助 ✅

**产出文件**：
- `src/env/scheduler/rpc/senders/reply.hh`
- `src/env/scheduler/rpc/senders/serve.hh`

**内容**：

#### reply.hh（内部辅助）：

1. `process_buffer(ch, packet_buffer*)`：
   - `while (has_full_pkt(buf))`：
     - `get_recv_pkt(buf)` 获取 pkt_iovec
     - 解析 RPC header（前 7 字节）
     - 构造 `payload_view`（偏移 7 字节后的部分）
     - 按 `header.type` 分流：
       - `response` / `error` → `handle_response(ch, header, pv)`
         - `ch->pending.remove(header.request_id)` 查找 callback
         - 未找到 → 丢弃（已超时或重复）
         - error → `cb(make_exception_ptr(remote_error(...)))`
         - response → `cb(pv)`
       - `request` / `notification` → 收集为 `handler_context` 待 dispatch 处理
     - `buf->forward_head(pkt_total_len)` 推进 head
   - 末尾调用 `ch->pending.check_timeouts(now_ms())`

2. `encode_and_send(ch, response)` 辅助：encode response → `ch->send_response(...)`

**关键问题 — Request 分流**：
- `process_buffer` 中 Response/Error 直接处理（pending_map callback）
- Request/Notification 需要进入 dispatch pipeline
- 实现方式：`process_buffer` 在处理完 Response/Error 后，对 Request 调用 dispatch 相关逻辑
- serve pipeline 的整体结构需要处理这种混合：
  - 方案：`process_buffer` 内部对 Response/Error 直接 callback；对 Request 则通过参数返回的方式传给外部 pipeline 处理
  - 或者：`process_buffer` 内部同时处理两种，Request 直接调用 `dispatch` 并在内部完成 encode+send

**推荐实现**（简化 v1）：`process_buffer` 内部处理所有消息类型，Request 的 dispatch 也在内部完成。serve pipeline 只负责驱动接收循环。

#### serve.hh：

1. `rpc::serve<service_ids...>(ch)` → 返回 sender pipeline：
   ```
   forever()
       >> net::recv(ch->session_sched, ch->session_id)
       >> then([ch](packet_buffer* buf) {
           process_all_messages<service_ids...>(ch, buf);
       })
       >> any_exception([ch](std::exception_ptr e) {
           ch->on_connection_lost(e);
           return just();
       })
   ```
   - `process_all_messages` 内部：
     - 解析所有完整消息
     - Response/Error → pending_map callback
     - Request → dispatch → handler → encode → send_response
     - Notification → dispatch → handler（无 response）
     - 检查超时

2. Request 的 dispatch 在 `process_all_messages` 中是同步调度的：
   - 由于 handler 可能返回异步 sender，需要用 `submit` 启动
   - 方案：对于每个 Request，构造 `dispatch pipeline >> then(encode_and_send) >> submit(ctx)`
   - 这样 handler 的异步操作可以正常执行，不阻塞接收循环

**替代方案**（如果 handler 全部同步）：直接在循环中调用，但不符合设计要求（handler 可返回异步 sender）。

**参考**：`rpc_design.md` 7.1-7.5 节、5.6 节

**注意**：
- 接收循环通过 `forever() >> recv(...)` 驱动，每次 recv 返回后处理 buffer 中所有完整消息（批处理）
- 参考 echo 示例中的 `check_session` 协程 + `for_each` 循环模式
- `any_exception` 确保单条消息处理异常不中断整个 serve 循环
- 连接断开（recv 返回异常）会触发 `on_connection_lost`

---

### Step 10 — 统一对外头文件 ✅

**产出文件**：`src/env/scheduler/rpc/rpc.hh`

**内容**：

```cpp
#pragma once
// RPC 层对外统一入口
#include "common/message_type.hh"
#include "common/header.hh"
#include "common/error.hh"
#include "common/codec_concept.hh"
#include "common/config.hh"
#include "channel/pending_map.hh"
#include "channel/dispatcher.hh"
#include "channel/channel.hh"
#include "senders/call.hh"
#include "senders/notify.hh"
#include "senders/serve.hh"
#include "codec/raw_codec.hh"
```

此步骤同时需要：
- 检查所有头文件的 include 关系和命名空间一致性
- 确保 `pump::rpc` 命名空间统一
- 确保与 CMakeLists.txt 兼容（header-only，无需额外编译单元）

---

## Phase 3: 集成测试 ✅

### Step 11 — Echo RPC 示例 ✅

**产出文件**：
- `apps/example/rpc_echo/main.cc`
- `apps/example/rpc_echo/CMakeLists.txt`（如需要）

**内容**：

参考 `apps/example/echo/echo.cc` 的结构，实现一个 RPC echo 服务：

1. 定义 `service_type` 枚举：`echo = 1`
2. 实现 `service<service_type::echo>`：
   - 接收 `handler_context`
   - 从 payload 解码（raw_codec）
   - 返回 `just(原始数据)` 作为 echo
3. 服务端：
   - `accept_scheduler` 监听
   - `wait_connection >> join >> serve<echo>(ch) >> submit`
4. 客户端：
   - `connect >> join`
   - 启动 `serve<>(ch)` 后台接收循环
   - `rpc::call(ch, echo, data) >> then(验证响应)`
5. 使用 `share_nothing` runtime 多核运行

**验证点**：
- 基础 Request-Response 往返
- 编解码正确性
- pipeline 组合能力

**修改**：需要在顶层或 apps 的 `CMakeLists.txt` 中添加此示例的构建目标

---

### Step 12 — 超时测试 ✅

**产出文件**：`apps/test/rpc_timeout_test.cc`

**内容**：

1. 服务端注册一个 "慢" handler（用 `task_sched->delay(long_ms)` 模拟延迟）
2. 客户端发起 `rpc::call(ch, slow_method, req, short_timeout_ms)`
3. 验证收到 `rpc_timeout_error` 异常
4. 验证超时后到达的响应被丢弃（pending_map 中已移除）
5. 验证 `pending.active_count` 正确归零

---

### Step 13 — 并发 RPC 调用测试 ✅

**产出文件**：`apps/test/rpc_concurrent_test.cc`

**内容**：

1. `when_all` 并发：同时发起 N 个 call，验证全部响应正确匹配
2. `for_each >> concurrent(N)` 扇出：向同一 channel 发起流式 call
3. 乱序响应：服务端 handler 按不同延迟返回，验证 request_id 匹配正确
4. pending_map 压力：接近 MaxPending 上限时的行为
5. 超过 MaxPending 时验证 `pending_overflow_error`

---

### Step 14 — 连接断开恢复测试 ✅

**产出文件**：`apps/test/rpc_disconnect_test.cc`

**内容**：

1. 服务端主动关闭连接（`net::stop`）
2. 验证客户端所有 pending requests 收到 `connection_lost_error`
3. 验证 serve 循环正常终止
4. 验证 channel 状态变为 closed
5. 验证 close 后尝试 call 收到 `channel_closed_error`

---

## Phase 4: 优化扩展

### Step 15 — 心跳/保活

**修改文件**：`src/env/scheduler/rpc/channel/channel.hh`（新增方法）
**新建文件**：`src/env/scheduler/rpc/senders/heartbeat.hh`（可选，也可内联在 channel 中）

**内容**：

1. `start_heartbeat(ch)` 返回独立 pipeline：
   - `forever() >> on(task_sched->delay(interval_ms)) >> then(send heartbeat notification)`
   - `any_exception` 捕获 `channel_closed_error` 终止循环
2. 接收端在 `process_buffer` 中记录最后收到消息的时间戳
3. 心跳超时检测：连续 N 次未收到任何消息 → 触发 `on_connection_lost`
4. 心跳可通过 `channel_config.enable_heartbeat` 启用/禁用

---

### Step 16 — 优雅关闭

**修改文件**：`src/env/scheduler/rpc/channel/channel.hh`

**内容**：

1. `channel::close()` 实现：
   - 状态 → draining
   - 停止接受新的 call（call 时检查 status）
   - `pending.active_count == 0` → 直接 closed + `net::stop`
   - 否则：通过 `task_sched->delay(drain_timeout)` 设置超时
   - 超时后 `fail_all` + closed + `net::stop`
2. 可选：发送关闭通知 Notification

---

### Step 17 — send_req Pool Allocator

**修改文件**：`src/env/scheduler/rpc/channel/channel.hh`（或独立文件）

**内容**：

1. Channel 内部维护 `send_req` 的对象池（固定大小 free list）
2. `send_response` / `send_error` / `send_notification` 从池中分配而非 `new`
3. send callback 完成后归还到池中
4. 减少热路径堆分配

---

### Step 18 — 静态 Dispatcher 优化（策略 B）

**修改文件**：`src/env/scheduler/rpc/channel/dispatcher.hh`

**内容**：

从 v1 的统一 `handler_context` 迭代到完全类型安全的二级 visit：

1. 每个 Service 定义 `request_types = std::variant<Req1, Req2, ...>` + `decode()` 方法
2. 框架侧实现两级 visit dispatch：
   - 第一级 visit：`service_id → service<sid>`
   - 第二级 visit：`request variant → 具体请求类型 → handle(req)` 重载匹配
3. handler 签名从 `handle(handler_context&&)` 变为类型化的 `handle(FooReq&&)`
4. 完全编译期类型安全，无需 Service 内部 switch

---

## 依赖关系

```
Step 1 (message_type + header)
Step 2 (error)
Step 3 (codec + payload_view)
     ↓
Step 4 (pending_map)  ← 依赖 Step 2 (error types) + Step 3 (payload_view)
Step 5 (dispatcher)   ← 依赖 Step 1 (header) + Step 2 (error) + Step 3 (payload_view)
     ↓
Step 6 (channel)      ← 依赖 Step 1-5 全部
     ↓
Step 7 (call)         ← 依赖 Step 6 (channel)
Step 8 (notify)       ← 依赖 Step 6 (channel)
Step 9 (serve)        ← 依赖 Step 5 (dispatcher) + Step 6 (channel) + Step 7 (call 的 pending 交互)
     ↓
Step 10 (rpc.hh)      ← 依赖 Step 7-9
     ↓
Step 11-14 (测试)      ← 依赖 Step 10
     ↓
Step 15-18 (优化)      ← 依赖 Phase 3 验证通过
```

**前三步（Step 1-3）可并行实现**，互不依赖。

---

## 需要关注的风险点

### 1. payload_view 与 ring buffer 生命周期

`payload_view` 指向 `packet_buffer` 内的数据。在 `process_buffer` 中调用 `forward_head` 后数据可能被覆盖。必须确保：
- Response 的 pending callback 在 `forward_head` 之前被调用
- Request 的 handler 如果是异步的，需要在 dispatch 前将 payload 拷贝出来（或确保 buffer 不会被提前推进）

**对策**：在 `process_buffer` 中，对于异步 handler，在调用 dispatch 前先将 payload 数据拷贝到独立 buffer。对于同步 handler 和 Response callback，可以直接使用 payload_view（零拷贝）。

### 2. serve 中 Request 的异步处理

handler 可能返回异步 sender（如切换到 task_scheduler 或 nvme_scheduler）。这意味着 `process_buffer` 不能同步等待 handler 完成。

**对策**：对每个 Request 构造独立的 `dispatch >> encode >> send_response >> submit(ctx)` pipeline。这样异步 handler 可以在其他 scheduler 上执行，完成后通过 pipeline 自动 encode 并 send response。

### 3. shared_ptr Channel 的引用循环

Channel 被 serve 循环、pending callbacks、用户 pipeline 共享持有。需确保：
- serve 循环终止时释放 channel 引用
- 所有 pending callbacks 被 fail_all 或正常执行后释放

**对策**：`on_connection_lost` 中 `fail_all` 清理所有 pending callback。serve 循环通过 `any_exception` 终止并释放 channel 引用。

### 4. encode_result 生命周期

`encode_result` 持有 payload 数据（inline_buf 或 heap_buf）。`send_req` 的 callback 中需要保持 `encode_result` 存活直到 send 完成。

**对策**：将 `encode_result` move 到 send callback 的闭包中（`rpc_design.md` 6.4 节已描述此方案）。

### 5. 与 CMake 集成

RPC 层是 header-only（与 Net 层和 Task Scheduler 一致），不需要额外编译单元。但需要：
- 确认 include 路径覆盖 `src/env/scheduler/rpc/`
- 示例和测试的 CMakeLists.txt 正确链接依赖

---

## 编码规范（遵循现有代码风格）

- 命名空间：`pump::rpc`（框架部分）、`pump::core`（op_pusher / compute_sender_type 特化）
- 文件组织：header-only，`.hh` 后缀
- 模板参数：`typename` 优先，`auto` 用于 non-type 参数（如 service_id）
- Lambda 参数：`auto&&` 完美转发
- 不可拷贝对象：`std::move`
- 异常传播：通过 `op_pusher::push_exception`，不在 then 中吞掉
- 无锁：所有 RPC 状态在 session 所在 core 单线程操作
