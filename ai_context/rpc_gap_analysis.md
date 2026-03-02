# RPC 模块功能缺失分析

> 基于对 `src/env/scheduler/rpc/` 全部源码的逐行审查，从功能完备性角度识别当前缺失项。
> 按优先级分层：P0 = 影响正确性/可用性，P1 = 常规 RPC 框架必备，P2 = 增强型功能。

---

## P0：正确性与可用性问题

### 1. 断连后 pending 请求永久挂起

**现状**：客户端并发 pipelining 时，若 session 断开，`trigger` 中 `pending_requests_map` 里已注册 callback 的请求不会被通知。Pipeline 永远不会完成。

**具体位置**：`client/call.hh:75` — `trigger` 是 `static thread_local`，`fail_all()` 方法已在 `rpc_state.hh:70` 实现，但从未被调用。

**缺失**：
- 客户端 `net::recv` 抛出 `session_closed_error` 时，没有调用 `trigger.fail_all()`
- 服务端 `handle_exception` 只标记 `closed`，不通知同一 session 上的其他 pending 请求

**影响**：生产环境中任何网络断开都会导致资源泄漏和 pipeline 挂起。

---

### 2. send 失败的处理不完整

**现状**：`call.hh:59` 中 `wait_res` 检查了 `net::send` 返回的 `bool ok`，失败时抛异常。但抛出异常后：
- `call_runtime_context` 中的 `res` 帧未被正确清理
- 同一 session 上的其他并发 call 不知道 send 失败了

**缺失**：send 失败应触发 session 级别的错误传播机制。

---

## P1：常规 RPC 框架必备功能

### 3. 请求超时

**现状**：完全没有超时机制。客户端发出请求后，如果服务端处理缓慢或帧丢失，客户端将无限等待。

**可行方案**：框架已有 `task::delay()` 和 `when_any`，可以组合实现：
```cpp
when_any(
    rpc::call<id>(sche, sid, args...),
    task::delay(task_sche, timeout_ms) >> then([]{ throw rpc_timeout_error(); })
)
```
但这应该是框架内置能力，而非每个调用点手写。

**建议**：提供 `rpc::call_with_timeout<id>(net_sche, task_sche, sid, timeout_ms, args...)` 便捷 API。

---

### 4. 服务端 session 主动关闭 / 优雅关闭

**现状**：
- 服务端 `serv` 只有被动关闭（异常时 `sd.close()` + 停止 recv 循环）
- 没有主动调用 `net::stop()` 关闭底层 fd
- 服务端无法主动断开恶意/空闲客户端

**缺失**：
- `serv` 循环结束后不调用 `net::stop(sche, session_id)` 释放网络资源
- 无 idle timeout 检测（长时间无请求的 session 不会被回收）
- 无 graceful shutdown 机制（进程退出时不通知客户端）

---

### 5. 客户端 session 生命周期管理

**现状**：`rpc::call` 只处理单次请求-响应。session 的创建（`net::connect` + `net::join`）和销毁（`net::stop`）完全由应用层手动管理。

**缺失**：
- 无连接池抽象（example 中 `connection_pool` 是应用层硬编码的）
- 无断线重连
- 无 session 健康检查（心跳）
- 客户端不知道 session 何时被服务端关闭

---

### 6. 错误码 vs 异常

**现状**：所有错误都走 C++ 异常（`std::runtime_error`、`std::logic_error`）。RPC 协议帧没有错误码字段。

**缺失**：
- 服务端 `handle()` 抛异常时，异常信息不会传回客户端 — 服务端直接关闭 session
- 没有 RPC 级别的错误响应（如：service not found、invalid arguments、internal error）
- 客户端只能区分"成功"和"连接断开"，无法获知具体业务错误

**建议**：在 `rpc_header.flags` 中增加 error 标志位，或在 payload 前加 status code。

---

### 7. 服务端并发处理

**现状**：`serv_proc` 中 `for_each` + `with_context` 是**严格串行**的 — 一个请求处理完才 recv 下一个。

**代码**（serv.hh:112-121）：
```cpp
for_each(check_rpc_state(sd))
    >> with_context(serv_runtime_context())(
        recv_req >> dispatch >> send_res
    )
    >> reduce();
```

**缺失**：单 session 上无法并发处理多个请求。如果 `handle()` 是异步的（如访问 NVMe），整个 session 阻塞在等待。

**建议**：在 `for_each` 后加 `concurrent(N)`，`concurrent` 的语义是允许并发，不需要额外的 `on(task_scheduler)` — `recv` 本身就是异步的。

---

## P2：增强型功能

### 8. 双向 RPC / 服务端推送

**现状**：设计上一条 TCP 连接只承载单方向调用。服务端主动推送需要反向建连。

**缺失**：
- 无 server push / notification 机制
- 无双向 streaming（类 gRPC 的 bidirectional stream）
- 应用层若需双向通信，必须管理两套 session，增加复杂度

---

### 9. 序列化框架

**现状**：使用 `reinterpret_cast` + `__attribute__((packed))` 做 zero-copy 序列化。要求：
- 所有字段 trivially copyable
- 固定大小（no variable-length fields like string）
- 只支持同架构（字节序、对齐）

**缺失**：
- 不支持变长字段（string、vector、嵌套结构）
- 不支持跨架构通信（大端/小端）
- 无版本号 / 前向兼容 / 后向兼容
- 用户需手写 `req_to_pkt` / `pkt_to_res`，易出错

---

### 10. 服务注册与发现

**现状**：服务端 `serv<service_ids...>()` 在编译期硬编码所有支持的 service_id。客户端 `call<service_id>()` 也是编译期指定。

**缺失**：
- 无运行时服务注册
- 无服务发现机制
- 新增 service 需修改 `serv<>` 模板参数并重新编译

---

### 11. 可观测性

**现状**：零监控、零日志、零指标。

**缺失**：
- 无请求耗时统计
- 无 QPS / 延迟百分位指标
- 无请求日志（可选的 debug tracing）
- 无 pending 请求数监控（`trigger` map 使用率不可观测）

---

### 12. 流量控制 / 背压

**现状**：客户端可以无限制发送请求（`concurrent()` 无参数 = 无上限）。服务端串行处理，不会反馈压力。

**缺失**：
- 无滑动窗口 / 令牌桶式流控
- 无服务端反压信号
- `pending_requests_map` 容量固定 2048，超过时 hash 冲突导致 UB（相同 `rid % capacity` 的两个请求会覆盖）

---

### 13. 多路复用（Multiplexing）

**现状**：一条 TCP 连接上可以 pipelining（同一方向并发多个请求），但不支持多个独立的逻辑"通道"。

**缺失**：
- 无 stream/channel 概念
- 不同优先级的请求无法隔离
- 大请求阻塞小请求（head-of-line blocking）

---

## 代码层面的小问题

| 问题 | 位置 | 说明 |
|------|------|------|
| ~~`rpc_flags` 缺少 `error` 标志~~ | struct.hh:24-28 | **已修复** — 新增 `error = 0x02` |
| `pending_requests_map` hash 冲突未防护 | rpc_state.hh:31 | `rid % slots.size()` 碰撞时直接 throw/覆盖 |
| `thread_index_allocator` 从 1 开始 | struct.hh:101-102 | `++` 前置，第一个线程 index=1，浪费 index=0 |
| 服务端 `recv_req` lambda 捕获 `&st` 引用 | serv.hh:77 | `st` 是 context 栈上的引用，生命周期依赖 context 不被销毁 |

---

## 总结：优先级排序

| 优先级 | 项目 | 工作量估计 | 状态 |
|--------|------|-----------|------|
| **P0** | 1. fail_session 集成 | 小 | **已完成** — slot 增加 session_raw，`fail_session()` 按 session 精确匹配 |
| **P0** | 2. send 失败传播 | 小 | **已完成** — `any_exception` 捕获后调用 `fail_session` 并重新抛出 |
| **P1** | 3. 请求超时 | 中 | 暂不需要 |
| **P1** | 4. 服务端 session 关闭 | 小 | **已完成** — `serv()` 结束后 `net::stop`，异常路径也覆盖 |
| **P1** | 5. 客户端 session 管理 | 大 | 不需要,设计就是希望rpc层和连接无关 |
| **P1** | 6. RPC 错误响应 | 中 | **已完成** — flags=0x02 + `rpc_error_code` + `rpc_error` 异常类 |
| **P1** | 7. 服务端并发处理 | 中 | **已完成** — `serv_concurrent<N>(sche, sid)`，编译期 concurrency，与 `serv` 共用 `serv_proc`，无需额外 scheduler |
| **P2** | 8. 双向 RPC | 大 | 不需要 |
| **P2** | 9. 序列化框架 | 大 | 暂不需要 |
| **P2** | 10. 服务注册发现 | 中 | 不需要 |
| **P2** | 11. 可观测性 | 中 | 暂不需要 |
| **P2** | 12. 流量控制 | 中 | 暂不需要 |
| **P2** | 13. 多路复用 | 大 | 不需要,本意就是希望应用层给rpc提供连接,如果需要高优先级的.就给高优先级的连接.这样一个同一类的rpc可以灵活的走不同优先级的连接 |

P0 和 P1 中标记"需要"的项目均已完成。
