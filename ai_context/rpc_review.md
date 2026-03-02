# RPC 模块代码审查

## 一、Bug

### 1. on_frame 错误信息写反 (rpc_state.hh:57-58)

```cpp
case slot_state::wait_callback: {
    throw std::runtime_error("invalid state : wait_frame");  // 应为 "wait_callback"
}
```

当前状态是 `wait_callback`（已有帧，等 callback），此时又来一帧，说明同一 slot 收到了重复帧。错误信息应该反映当前状态 `wait_callback`，而不是 `wait_frame`。

### 2. wait_res 忽略 send 的返回值 (call.hh:59)

```cpp
>> sender::flat_map([](ctx_t &ctx, bool ok) {
    return net::recv(ctx.scheduler, ctx.sid)
```

`bool ok` 来自 `net::send` 的结果，但被完全忽略。如果 send 失败（`ok == false`），仍然去 recv 等响应，会永远挂起。应该检查 `ok`，失败时抛异常或直接返回错误。

### 3. total_len 是 uint32_t，但 net::send 的 len 参数是 uint16_t (call.hh:45, serv.hh:94)

```cpp
// call.hh:42-45
auto len = ctx.req.get_len();         // 返回 uint32_t
return net::send<scheduler_t>(..., f, len);  // 参数类型 uint16_t，隐式截断

// serv.hh:91-94，同样的问题
auto len = memo.res.get_len();
return net::send(st.scheduler, st.session_id, f, len);
```

rpc_header.total_len 是 `uint32_t`，但 net 层的帧长度用 `uint16_t`（wire format 的 length prefix 也是 uint16_t）。如果 RPC 帧超过 65535 字节，len 会被静默截断。

两个选择：
- rpc_header.total_len 改为 `uint16_t`，与 net 层一致
- 或者保留 `uint32_t` 但加 static_assert / 运行时检查

## 二、废代码 / 需清理

### 4. rpc_flags::push (struct.hh:29)

已确认是废代码，删除。

### 5. rpc_state::next_request_id 未使用 (rpc_state.hh:79)

```cpp
template <typename session_t>
struct rpc_state {
    uint32_t next_request_id = 0;  // 没有任何代码使用这个字段
    pending_requests_map pending{256};
    session_t session;
};
```

`request_id` 的生成在 `struct.hh` 的 `request_id` 结构体中通过 thread_local 实现，与 `rpc_state::next_request_id` 无关。并且 `rpc_state` 本身也没有被使用——客户端用的是 `call_runtime_context`，服务端用的是 `session_state` + `serv_runtime_context`。整个 `rpc_state` 结构体可能是废代码。

### 6. has_handle_concept 未使用 (service.hh:29-30)

```cpp
template<typename T, typename Req>
concept has_handle_concept = requires(Req &&r) { T::handle(std::forward<Req>(r)); };
```

定义了但没有任何代码使用。dispatch 中直接用 `if constexpr (requires { T::is_service; })` 判断。

### 7. trigger.hh 中的 req 结构体未使用 (trigger.hh:12-15)

```cpp
struct req {
    std::move_only_function<void()> cb;
};
```

trigger 内部实际用的是 `pending_requests_map` 的 `completion_callback`，这个 `req` 没有被引用。

### 8. 重复 include (struct.hh:7-8)

```cpp
#include "env/scheduler/net/net.hh"
#include "env/scheduler/net/net.hh"  // 重复
```

### 9. 双分号 (serv.hh:81)

```cpp
memo.req.frame = reinterpret_cast<server::rpc_frame *>(frame.release());;
```

## 三、设计问题

### 10. 命名空间：所有公共类型都在 server:: 下

`struct.hh`、`rpc_state.hh`、`service.hh` 位于 `common/` 目录，但命名空间是 `pump::scheduler::rpc::server`。导致客户端代码充斥 `server::` 前缀：

```cpp
// client/call.hh
using ctx_t = server::call_runtime_context<scheduler_t>;
server::service<service_id>::req_to_pkt(ctx.req, ...);
server::request_id().value;
```

`rpc_header`、`rpc_frame`、`rpc_frame_helper`、`request_id`、`call_runtime_context`、`service` 这些都是客户端和服务端共用的类型，应该放在 `pump::scheduler::rpc` 或 `pump::scheduler::rpc::common` 命名空间。

### 11. rpc_frame_helper 缺少 move assignment operator

有 move constructor，copy constructor 被删除，但没有 move assignment operator（隐式也被删除了）。当前代码中通过显式 move constructor（在 `serv_runtime_context` 和 `call_runtime_context` 中）绕过了这个问题，但如果将来需要 `helper_a = std::move(helper_b)`，会编译失败。

### 12. request_id 的 thread_index 从 2 开始

```cpp
static inline std::atomic<uint16_t> thread_index_allocator = 1;
static inline thread_local const uint16_t current_thread_index = ++thread_index_allocator;
```

初始值 1 + pre-increment → 第一个线程得到 2。索引 0 和 1 永远不会被使用。如果想从 1 开始，应改为 `= 0`。

### 13. fail_all 未实现 (rpc_state.hh:71-73)

已确认为待完善项。session 断开时 pending map 中的 callback 永远不会被调用，导致对应的 pipeline 挂起。

### 14. handle_exception 重新抛出异常的去向 — 已理解，正确设计

异常向上层传播，由应用层决定如何处理。示例程序（rpc.cc）简单处理是因为它只是示例。
