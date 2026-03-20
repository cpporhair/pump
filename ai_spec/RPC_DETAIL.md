# RPC 模块详细文档

> 基于 PUMP sender/operator 框架构建的轻量远程过程调用层。基础 sender 语义见 CLAUDE.md，自建 scheduler 模式见 CLAUDE.md "自建 Scheduler" 章节。

## 1. 设计哲学

| 原则 | 说明 |
|------|------|
| Session-only API | RPC 只需 session 指针，scheduler 类型从 session 自动推导。无独立 transport trait |
| 单向调用 session | 一条连接只承载一个方向的 RPC 调用（客户端→服务端）。服务端推送需另建 session |
| sender 原生组合 | `serv` 是 sender，`call` 是 bind_back（需 `just() >>` 前缀），直接与 `then`/`flat_map`/`concurrent` 等组合 |
| 最小抽象 | 无 channel/dispatch loop 等独立运行时。`call` = 发请求+等响应，`serv` = 收请求+分派+发响应 |

## 2. 目录结构

```
src/env/scheduler/net/rpc/
├── rpc.hh                    ← 公共 API（session-only，scheduler 类型自动推导）
├── client/
│   ├── call.hh              ← 客户端 RPC 调用实现（session_t 模板参数）
│   └── trigger.hh           ← 乱序响应处理（自定义 scheduler）
├── server/
│   └── serv.hh              ← 服务端请求处理循环（session_t 模板参数）
└── common/
    ├── struct.hh            ← 帧结构、辅助类型、运行时 context
    ├── rpc_state.hh         ← pending_requests_map（使用 pump::scheduler::net::net_frame）
    ├── rpc_layer.hh         ← rpc_session_layer（session 组合 layer）
    └── service.hh           ← service trait + variant dispatch 工具

src/env/scheduler/net/common/
├── frame.hh                  ← pump::scheduler::net::net_frame（传输无关的帧类型）
├── session.hh                ← pump::scheduler::net::session_t（session 组合模板）
├── send_sender.hh            ← 通用 send sender（via do_send tag）
├── recv_sender.hh            ← 通用 recv sender（via do_recv tag）
└── frame_receiver.hh         ← frame_receiver layer（recv 队列管理）
```

示例程序：`apps/example/rpc/`（tcp_rpc.hh + kcp_rpc.hh + service.hh + rpc.cc）

## 2.1 Session-only 设计

RPC 不再使用独立的 transport trait。Session 本身通过 `do_send`/`do_recv` tag dispatch 完成收发，RPC 层只需 `session*`：

```cpp
// session 通过 layers 组合提供 recv/send 能力
// 例如 TCP session:
using session_type = pump::scheduler::net::session_t<
    tcp::common::tcp_bind<sched_t>,        // fd/状态/do_send
    tcp::common::tcp_ring_buffer<>,         // 缓冲/解包/on_recv
    rpc::rpc_session_layer,                 // RPC 状态（pending_requests_map）
    pump::scheduler::net::frame_receiver            // recv 队列/do_recv
>;
```

**添加新传输协议的 RPC 支持**：只需定义对应的 session 组合（使用协议的 bind layer + ring_buffer/frame_receiver），然后直接用 `rpc::serv`/`rpc::call`。

适用协议：所有**可靠的、面向连接的**传输协议（TCP、QUIC、KCP、ENet 等）。raw UDP 不适用。

**Session Factory 在应用层定义**：框架提供各协议的 layers（`tcp_bind`、`tcp_ring_buffer`、`kcp_bind` 等），应用层自行组合：

```cpp
struct tcp_rpc_factory {
    template<typename sched_t>
    using session_type = pump::scheduler::net::session_t<
        tcp::common::tcp_bind<sched_t>,
        tcp::common::tcp_ring_buffer<>,
        rpc::rpc_session_layer,
        pump::scheduler::net::frame_receiver
    >;
    template<typename sched_t>
    static auto* create(int fd, sched_t* sche) {
        return new session_type<sched_t>(...);
    }
};
```

## 3. 协议帧格式

```
┌──────────────┬───────────────┬──────────────┬───────┬────────────────────┐
│ total_len(4) │ request_id(8) │ service_id(2)│ flags │    payload(变长)    │
│   uint32     │   uint64      │   uint16     │ uint8 │                    │
└──────────────┴───────────────┴──────────────┴───────┴────────────────────┘
                        共 15 字节 header（__attribute__((packed))）
```

- **total_len**：整帧字节数（header + payload）
- **request_id**：全局唯一，`(thread_index << 48) | counter++`，线程内递增，跨线程不冲突
- **service_id**：enum class → uint16_t，标识服务类型
- **flags**：request(0x00) / response(0x01) / error(0x02)

### 错误帧

当 flags=0x02 时，payload 为 2 字节 `rpc_error_code`（uint16_t）：

```cpp
enum class rpc_error_code : uint16_t {
    unknown_service    = 1,   // service_id 未注册
    handler_exception  = 2,   // handle() 抛异常
};
```

客户端收到错误帧时抛出 `rpc_error` 异常（继承自 `std::runtime_error`），可用 `catch_exception<rpc_error>` 捕获。

### 帧内存管理

`rpc_frame_helper`：RAII 包装，move-only。
- `realloc_frame(payload_size)` 分配/扩展（只增不减，减少 heap allocation）
- `get_payload()` / `get_len()` 访问帧数据
- 析构时 `delete[] reinterpret_cast<char*>(frame)` 释放

## 4. 服务端流程

```
rpc::serv<service_id1, service_id2, ...>(session)

展开为：
push_context(session_state{session})               ← 压入 session 状态
>> serv_proc()                                     ← 主处理逻辑
>> pop_context()
>> ignore_args()

注：连接生命周期管理（TCP join/stop、KCP connect 等）由应用层负责，不在 rpc::serv 内部。

serv_proc 内部：
  for_each(check_rpc_state(sd))                    ← 协程：sd.closed 前持续 yield true
  >> with_context(serv_runtime_context)(            ← 每次迭代一个新的 req/res 帧对
      recv_req(sd)                                 ← session->invoke(do_recv{}) → rpc_frame
      >> flat_map(                                 ← 隔离 dispatch 异常作用域
          dispatch<service_ids...>()               ← 按 service_id 分派到 handle()
          >> any_exception(build_error_response)   ← dispatch 失败 → 构建错误帧
          >> send_res(sd)                          ← session->invoke(do_send{}, data, len)
      )
  )
  >> handle_exception(sd)                          ← 异常时标记 closed + 传播异常
  >> reduce()                                      ← 等全部流元素处理完
```

### 错误响应机制

dispatch 失败时（handle 抛异常或 unknown service_id），`any_exception` 在 `flat_map` 内层捕获异常，通过 `build_error_response()` 构建错误帧（flags=0x02），然后正常 `send_res` 发回客户端。session 保持打开。

`recv_req` 失败（连接断开等）则异常逃出 `flat_map`，由 `handle_exception` 关闭 session。

### 并发处理模式

`rpc::serv<concurrency, ids...>(session)` 在串行 `serv` 基础上增加：
- `concurrent(concurrency)` 允许 N 个请求同时 in-flight（编译期 `uint16_t` 模板参数）
- 不需要额外的 task_scheduler — recv 本身是异步的，`concurrent` 只是允许并发，是否真正并发取决于下游处理逻辑
- 内部通过 `apply_concurrency<N>()` 模板函数条件性地插入 `concurrent(N)`，`serv` 和 `serv_concurrent` 共用同一个 `serv_proc`
- 每个迭代独立 recv → dispatch → send
- 响应可能乱序发送（客户端 trigger 已支持乱序匹配）

### dispatch 机制

1. `get_service_class_by_id<ids...>(runtime_id)` → `variant<monostate, service<id1>, service<id2>, ...>`
2. `visit()` 将 variant 转为编译期类型
3. `if constexpr (T::is_service)` 匹配具体 service，调用 `T::handle(req, res)` → 返回 sender
4. `monostate` = 未注册 service_id → 抛异常

### 连接生命周期

`check_rpc_state` 协程通过 `session_state.closed` 控制：
- 正常 → `co_yield true` → for_each 驱动下一次 recv
- 异常 → `handle_exception` 调用 `sd.close()` → 协程返回 false → 流结束

注：连接的建立和销毁（TCP 的 join/stop、KCP 的 connect/disconnect 等）由应用层负责，不在 `rpc::serv` 内部。

## 5. 客户端流程

```
just() >> rpc::call<service_id>(session, args...)

call 是 bind_back（operator），展开为：
with_context(call_runtime_context{rid, session, req, res})(
    send_req<service_id>()                          ← 序列化参数 → 填 header → session->invoke(do_send{})
    >> wait_res<service_id>()                       ← session->invoke(do_recv{}) → 匹配 request_id → 反序列化
)
```

### 乱序响应处理（wait_res 核心逻辑）

单个 call：session 上唯一请求，下一帧必然是自己的 → 直接返回。

N 个并发 call（pipelining）：N 个 recv 竞争收帧：
1. 收到帧 → 检查 `request_id` 是否匹配
2. **匹配**：`recv_res<true>` → 直接传递到反序列化
3. **不匹配**：`recv_res<false>` → `trigger.on_response(wrong_rid, frame)` 存入 map → `trigger.wait_response(my_rid)` 挂起等待
4. `visit()` 分支处理 variant → `flat()`

关键洞察：trigger 不做 recv，只暂存+匹配。各并发 call 的 recv 互相为对方接力。

### 错误响应检测

`wait_res` 最后的 `then` 中检查 `flags == error`，若是则抛出 `rpc_error(code)`。

### 断连通知（fail_session）

`wait_res` 末尾包裹 `any_exception`：当 recv 或 send 失败时，调用 `trigger.fail_session(address_raw, ex)` 通知同连接上所有 pending 请求（通过 slot 中记录的 `address_raw` 精确匹配），然后重新抛出原异常。

## 6. trigger 自定义 scheduler

`trigger` 是轻量微型 scheduler，遵循 PUMP 自定义 scheduler 六组件模式：

| 组件 | 实现 |
|------|------|
| op | `storage_at_op = true`，`start<pos>()` 注册回调到 map |
| sender | `storage_at_sender`，`connect()` 构建 op_tuple |
| scheduler | `trigger`，持有 `pending_requests_map`，提供 `wait_response(rid, session_raw)`/`on_response(rid, frame)`/`fail_session(session_raw, ex)` |
| op_pusher 特化 | `requires storage_at_op`，调用 `op.start<pos>()` |
| compute_sender_type 特化 | 输出类型为 `pump::scheduler::net::net_frame` |

**`static thread_local`**：利用 single-thread scheduler 不变量，无需同步原语。

### pending_requests_map 两阶段匹配

slot 状态机：`empty ↔ wait_frame / wait_callback`

| 操作 | slot 为 empty | slot 已有另一方 |
|------|--------------|----------------|
| `on_callback(rid, session_raw, cb)` | 存 cb + session_raw → wait_frame | 已有 frame → 立即执行 cb(frame)，清空 |
| `on_frame(rid, frame)` | 存 frame → wait_callback | 已有 cb → 立即执行 cb(frame)，清空 |

- 定长 vector，`rid % capacity` hash，零分配，cache 友好
- 默认 capacity 2048，同时在飞请求数不超过 capacity 即无冲突

## 7. Service 定义模式

应用层通过特化 `pump::scheduler::rpc::service<service_id>` 定义服务：

```cpp
// 1. 定义 service_id（必须是 uint16_t 底层类型的 enum class）
enum class type : uint16_t { add, sub };

// 2. 特化 service
template <>
struct pump::scheduler::rpc::server::service<type::add> {
    constexpr static bool is_service = true;

    struct __attribute__((packed)) req_struct { int a, b; };
    struct __attribute__((packed)) res_struct { int v; };

    // 服务端：处理请求，填充响应帧，返回 sender
    static auto handle(rpc_frame_helper& req, rpc_frame_helper& res) {
        auto* r = reinterpret_cast<req_struct*>(req.get_payload());
        res.realloc_frame(sizeof(res_struct));
        reinterpret_cast<res_struct*>(res.get_payload())->v = r->a + r->b;
        return just();  // 同步完成，也可返回异步 pipeline
    }

    // 客户端：将参数序列化到请求帧
    static auto req_to_pkt(rpc_frame_helper& req, int a, int b) {
        req.realloc_frame(sizeof(req_struct));
        auto* r = reinterpret_cast<req_struct*>(req.get_payload());
        r->a = a; r->b = b;
    }

    // 客户端：从响应帧反序列化结果
    static auto pkt_to_res(rpc_frame_helper& res) -> res_struct {
        return *reinterpret_cast<res_struct*>(res.get_payload());
    }
};
```

要点：
- `handle` 返回 sender（`just()` = 同步，也可返回异步 pipeline）
- 序列化用 `reinterpret_cast`，zero-copy，依赖 `__attribute__((packed))` 保证布局
- `uint16_enum_concept` 约束 service_id 类型

## 8. 所有权与生命周期

| 对象 | 所有权 | 生命周期 |
|------|--------|---------|
| `rpc_frame`（via `rpc_frame_helper`） | RAII，`delete[] char*` | 帧处理期间。send 前 `frame = nullptr` 转移给传输层 |
| `call_runtime_context` | 值语义，`with_context` 作用域 | 单次 RPC 调用 |
| `serv_runtime_context` | 值语义，`with_context` 作用域 | 单次请求处理（recv→dispatch→send） |
| `session_state` | 值语义，`push_context` 作用域 | session 存续期间 |
| `trigger` | thread_local 静态变量 | 线程生命周期 |

### 帧所有权转移链

**发送**：`rpc_frame_helper.frame` → `ctx.req.frame = nullptr` 释放所有权 → `session->invoke(do_send{})` 接管 → 传输层写完后释放

**接收**：`session->invoke(do_recv{})` → `pump::scheduler::net::net_frame` → `frame.release()` 拿出 `char*` → `reinterpret_cast<rpc_frame*>` → `ctx.res.frame` 接管 → `rpc_frame_helper` 析构释放

## 9. 公共 API

```cpp
// --- Session-only API（scheduler 类型从 session 自动推导）---

// 串行服务端
rpc::serv<service_id1, service_id2, ...>(session)

// 并发服务端（concurrency 为编译期 uint16_t）
rpc::serv<concurrency, service_id1, service_id2, ...>(session)

// 客户端调用（bind_back，需 just() >> 前缀）
just() >> rpc::call<service_id>(session, args...)
```

### Session 要求

RPC session 必须包含以下 layers：
- **bind layer**（如 `tcp_bind`/`kcp_bind`）：提供 `do_send` tag 处理
- **buffer/unpack layer**（如 `tcp_ring_buffer`）：提供 `on_recv` 解包
- **`rpc_session_layer`**：提供 `pending_requests_map`（乱序响应匹配）
- **`frame_receiver`**：提供 `do_recv` tag 处理（recv 队列）

## 10. 已知限制与待完善

| 项目 | 状态 | 说明 |
|------|------|------|
| Session-only API | 已实现 | 无独立 transport trait，scheduler 从 session 类型推导 |
| Session Factory | 应用层 | 框架提供 layers，应用层定义组合（factory struct） |
| `fail_session` 精度 | 已实现 | 按 `address_raw` 匹配，仅 fail 同连接的 pending 请求 |
| 错误响应 | 已实现 | flags=0x02 传递 `rpc_error_code`，客户端抛 `rpc_error` |
| 连接生命周期 | 应用层 | `serv()` 不含 join/stop，由应用层管理（TCP 需 `tcp::join`/`tcp::stop`） |
| 并发处理 | 已实现 | concurrency 作为模板参数，无需额外 scheduler |
| 帧长度截断 | 已知 | `total_len` 为 uint32_t，但 TCP 层 send 用 uint16_t，超 64KB 帧会截断 |
| 命名空间 | 待优化 | 共用类型在 `server::` 命名空间下，客户端代码需 `server::` 前缀 |
