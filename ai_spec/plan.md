# PUMP RPC 层设计方案

> **文档目的**：记录 RPC 层的设计决策、实现方案和技术细节  
> **配套文档**：`command.md`（需求文档，描述"做什么"）；本文档描述"怎么做"  
> **前置依赖**：net 模块已实现底层网络收发（conn/connect/recv/send/join/stop）

---

## 一、设计决策记录

以下决策已在需求讨论中确认，作为实现的约束条件：

| 编号 | 决策项 | 决策结果 | 影响范围 |
|------|--------|----------|----------|
| D-1 | request_id 类型 | `uint32_t`，session 内自增 | F-CORR-1, F-CORR-5 |
| D-2 | 超时机制 | 本版不在 RPC 层实现 | F-CORR-3 降为 P2 |
| D-3 | 背压/限流 | 本版不实现，由 `concurrent(max)` 在管道层控制 | 无需额外需求 |
| D-4 | 连接级握手 | 协议层通过注册和实现"握手"RPC 命令自行处理 | 本版不实现 |
| D-5 | 消息大小限制 | 本版不实现 | 无需额外需求 |
| D-6 | RESP 按序响应 | 不为 RESP 破坏 RPC 层的 request_id 设计；RESP 将来可直接架构在 Net 层上 | RPC 层只需支持基于 request_id 的乱序响应匹配 |
| D-7 | pending_requests 容量 | 默认 256，可通过 session 初始化时配置调整 | session_rpc_state 初始化 |
| D-8 | 多帧推送策略 | 本版单次推送（每帧一次 push），不做批量推送 | 本版不实现批量推送 |
| D-9 | session RPC 状态存储 | RPC 层维护独立的 `session_id → rpc_state` 映射，通过 `get_or_bind` 首次访问时自动创建（lazy bind） | 符合 F-INT-1 |
| D-10 | 编码内存管理 | 栈/预分配固定缓冲（`std::array<iovec, 8>`），编码器将 iovec 写入 RPC 层预分配的固定大小缓冲区 | P-MEM-2, P-THR-2, F-CODEC-4 |
| D-11 | `rpc::recv` 消息类型 | 返回扁平化的 `std::variant<具体消息类型...>`，下游通过 PUMP 的 `visit()` 算子解包后用 `if constexpr` 分支处理。不使用嵌套 variant | F-API-3 |
| D-12 | handler 返回值 | 每个命令的 handler 函数统一返回 sender（如 `just(result)`），保持 pump 管道风格一致性 | 使用示例 |
| D-13 | 连接池、调度均为上层职责 | RPC 层不引入 dispatcher、connection_pool。RPC 层只关心「用哪个 session 完成一次 RPC 调用」。但 RPC 层**提供编译期 dispatch 机制**，根据帧头 `module_id` 将请求路由到对应的 `service<sid>::handle()`（见 F-SVC-4） | 1.1 职责边界 |
| D-14 | module_id | `uint16_t` 类型，值为 `static_cast<uint16_t>(service_type)`，嵌入 RPC 统一帧头。RPC 层在编码时写入帧头，在 `rpc::serve` 中通过编译期 dispatch 根据 `module_id` 路由到对应的 `service<sid>` | 帧头设计 |
| D-15 | handler 响应发送语义 | `rpc::serve` 在调用 handler 后自动将返回值编码并通过同一 session 发回。handler 只需返回响应消息（sender），不需感知连接 ID | serve 语义 |
| D-16 | net 层 recv 机制重构 | net 层已从「暴露 `packet_buffer*` 指针 + atomic 单槽位」改为「copy-out 独立帧 `recv_frame` + 多 recv 队列」模式。`recv_cache` 从单槽位改为 `recv_q` + `ready_q`；`on_read_event` 在数据到达时自动分帧并 copy-out | F-INT-1, F-FRAME-3, F-CODEC-5 |

---

## 二、协议特征（Protocol Trait）设计

**核心设计**：Protocol 与 service_type **一一对应**。每个 `service<st>` 模板特化同时满足 Protocol concept，既定义业务处理逻辑（`handle()`），又定义编解码能力（`decode()`/`encode()`）。因此 `rpc::call<st>` 通过 `service<st>` 即可获得完整的协议能力，无需额外传入 Protocol 类型。

Protocol concept 只约束与具体协议相关的能力（编解码、消息类型）。`request_id`、`module_id`、`flags` 等帧头字段由 RPC 统一帧头定义，RPC 层直接从帧头提取，**不属于** Protocol 的职责：

```cpp
template <typename P>
concept Protocol = requires {
    // 扁平化的消息 variant 类型（D-11）
    typename P::message_type;

    // 解码：将 recv_frame 的 payload（跳过 rpc_header 后的部分）反序列化为 message_type
    // msg_type 从 rpc_header 中提取，传入 decode 以区分消息类型
    { P::decode(std::declval<uint8_t>(), std::declval<const char*>(), std::declval<size_t>()) }
        -> std::same_as<typename P::message_type>;

    // 编码：将消息序列化为 iovec 数组（D-10）
    // 每种消息类型提供独立重载
};
```

> **帧头与 Protocol 的分工**：RPC 统一帧头（`request_id`、`module_id`、`msg_type`、`flags`）由 RPC 层统一解析，不需要 Protocol 参与。Protocol 只负责 payload 的编解码。`classify`（区分 request/response/push）通过帧头 `flags` 字段判断，`get_request_id` 通过帧头 `request_id` 字段读取——这些都是 RPC 层的通用逻辑，与具体协议无关。

> **关于分帧**：net 层通过 `uint16_t` 长度前缀从环形缓冲区中 copy-out 完整的包，产出 `recv_frame`。即使 TCP 出现粘包或不完整包，net 层都通过长度+内容保证完整性。`recv_frame` 内部的具体内容（RPC 帧头、payload 等）完全由 RPC 层决定。RPC 层直接对 `recv_frame` 进行解码即可，不需要 `try_parse_frame`。

### 2.1 协议特征实现示例

`service<st>` 模板特化同时满足 Protocol concept 和服务注册：

```cpp
template <>
struct service<service_type::service_001> {
    // --- Protocol 部分 ---
    struct add_req { int a; int b; };
    struct add_resp { int result; };
    using message_type = std::variant<add_req, add_resp>;

    // 解码 payload（rpc_header 已由 RPC 层解析并剥离）
    static message_type decode(uint8_t msg_type, const char* payload, size_t len) {
        switch (msg_type) {
            case 0x01: return *reinterpret_cast<const add_req*>(payload);
            case 0x02: return *reinterpret_cast<const add_resp*>(payload);
            default:   throw rpc::protocol_error("unknown msg_type");
        }
    }

    static size_t encode(const add_req& msg, std::array<iovec, 8>& buf) {
        return do_encode(msg, buf);
    }
    static size_t encode(const add_resp& msg, std::array<iovec, 8>& buf) {
        return do_encode(msg, buf);
    }

    // --- Service 部分 ---
    static constexpr bool is_service = true;

    static auto handle(add_req&& req) {
        return just(add_resp{req.a + req.b});
    }
};
```

---

## 三、RPC 统一帧头设计

```
┌──────────────────────────────────────────┐
│ total_len (4B)     ← 帧总长度（含帧头）    │
│ request_id (4B)    ← 关联请求和响应（D-1） │
│ module_id (2B)     ← 模块标识（D-14）        │
│ msg_type (1B)      ← 模块内的消息类型       │
│ flags (1B)         ← request/response/push │
│ payload ...                               │
└──────────────────────────────────────────┘
```

- `total_len`：RPC 帧总长度（含帧头）。虽然 net 层已通过 `uint16_t` 长度前缀完成分帧并保证包完整性，但 `total_len` 仍然保留：**net 层的长度前缀是 net 层用来保证包完整性的机制，net 层的上层不一定是 RPC 层**；`total_len` 是 RPC 层自身的帧长度信息，可用于 RPC 层的完整性校验、日志记录、payload 长度计算（`payload_len = total_len - sizeof(rpc_header)`）等。两者各司其职，互不影响
- `module_id`：由 `service_type` 枚举值决定（`module_id = static_cast<uint16_t>(st)`），RPC 层在编码时将其写入帧头，在 `rpc::serve` 中据此进行编译期 dispatch（见 D-14, F-SVC-4）
- 帧头格式为 RPC 层统一定义，`recv_frame` 的内容以此帧头开头，后跟协议特定的 payload
- `request_id`、`module_id`、`msg_type`、`flags` 由 RPC 层统一解析，不需要 Protocol 参与

### 3.1 帧头与 net 层的分工

net 层负责分帧：通过 `uint16_t` 长度前缀从环形缓冲区（`packet_buffer`）中 copy-out 完整的包，产出 `recv_frame`。net 层保证每个 `recv_frame` 是一个完整的、独立的包，处理了 TCP 粘包和不完整包的问题。

RPC 层负责解释 `recv_frame` 的内容：从中解析 RPC 统一帧头（`request_id`、`module_id`、`msg_type`、`flags`），然后将 payload 部分交给 `service<st>::decode` 进行反序列化。

- **net 层的长度前缀**（`uint16_t`）：用于分帧，保证包的完整性
- **RPC 帧头中的字段**：用于 RPC 层的请求-响应关联、服务分发等语义，与 net 层分帧无关
- RPC 层**不需要**参与分帧过程，也不需要提供 `try_parse_frame`

---

## 四、数据接收流程

### 4.1 net 层：分帧与 copy-out

net 层的 `on_read_event` 在数据到达时自动完成分帧和 copy-out：

```
io_uring/epoll 事件循环:
  数据到达 → buf.forward_tail(res)
  → 循环:
      copy_out_frame(&buf)          // 按 uint16_t 长度前缀切分
      if frame.size() > 0:
          if recv_q 有等待的回调 → 触发回调，传递 recv_frame
          else → 暂存到 ready_q
          继续循环（处理粘包）
      if frame.size() == 0:
          break  // 不完整帧，等待更多数据
```

### 4.2 RPC 层：解码与分发

RPC 层收到 net 层产出的 `recv_frame`（已保证完整性），直接解析内容：

```
net::recv(sid)
  → recv_frame&&                        // net 层已完成分帧的完整包
  → 解析 RPC 帧头（request_id, module_id, msg_type, flags）
  → service<st>::decode(msg_type, payload, len)  // struct overlay 解码 payload
  → 推送给下游（call 的响应匹配 / serve 的 dispatch）
```

---

## 五、Sender API 签名设计

```cpp
namespace pump::scheduler::rpc {

    // --- rpc::call ---
    // 模板参数为 service_type 值，标识目标服务
    template <service_type st, typename scheduler_t>
    auto call(scheduler_t* sche, session_id_t sid, auto&& req);
    // 返回 sender，完成值类型为对应 service 的响应类型

    template <service_type st>
    auto call(session_id_t sid, auto&& req);  // flat_map 版本

    // --- rpc::send ---
    template <service_type st, typename scheduler_t>
    auto send(scheduler_t* sche, session_id_t sid, auto&& msg);

    template <service_type st>
    auto send(session_id_t sid, auto&& msg);

    // --- rpc::recv ---
    template <service_type st, typename scheduler_t>
    auto recv(scheduler_t* sche, session_id_t sid);
    // 返回 sender，完成值类型为 service<st>::message_type（扁平 variant）

    template <service_type st>
    auto recv(session_id_t sid);

    // --- rpc::serve ---
    // 模板参数为 service_type 值列表，标识本端提供的服务集合
    template <service_type ...sids, typename scheduler_t>
    auto serve(scheduler_t* sche, session_id_t sid);

    template <service_type ...sids>
    auto serve(session_id_t sid);
}
```

---

## 六、会话 RPC 状态设计

每个 session 维护以下 RPC 状态（D-9）。状态与具体协议无关（`pending_requests_map` 和 `next_request_id` 是通用的 RPC 机制）：

```cpp
// completion_callback: recv 到响应后触发的回调
using completion_callback = std::move_only_function<void(recv_frame&&)>;

struct session_rpc_state {
    uint32_t next_request_id = 0;                                    // D-1
    pending_requests_map pending{256};                                // D-7
    // 无需分帧状态：net 层已完成分帧，RPC 层直接解码 recv_frame
};

// D-9: RPC 层独立维护 session_id → rpc_state 映射
struct rpc_state_registry {
    std::unordered_map<session_id_t, session_rpc_state> states;

    auto& get_or_bind(session_id_t sid) {
        auto it = states.find(sid);
        if (it != states.end()) return it->second;
        return states.emplace(sid, session_rpc_state{}).first->second;
    }

    void unbind(session_id_t sid) { states.erase(sid); }
};
```

**绑定时机**：`rpc::call` / `rpc::serve` / `rpc::recv` 内部首次对某个 session 进行 RPC 操作时，通过 `get_or_bind(sid)` 自动创建状态（lazy init）。

> **为何不需要 Protocol 模板参数**：`session_rpc_state` 只管理 `request_id` 分配和 `pending_requests_map`，这些是协议无关的 RPC 基础设施。具体的编解码（`decode`/`encode`）在 `rpc::call<st>` / `rpc::serve<sids...>` 的实现中通过 `service<st>` 获取，不需要存储在 session 状态中。

### 6.1 pending_requests_map 实现

`pending_requests_map` 用于存储 in-flight 请求的回调，需要 O(1) 查找（P-LAT-2）和预分配（P-MEM-1）：

```cpp
struct pending_requests_map {
    struct slot {
        bool occupied = false;
        completion_callback cb;
    };

    std::vector<slot> slots;  // 预分配，大小由 D-7 决定（默认 256）

    explicit pending_requests_map(size_t capacity) : slots(capacity) {}

    // 注册等待回调，返回 false 表示容量满
    bool insert(uint32_t rid, completion_callback&& cb) {
        auto idx = rid % slots.size();
        if (slots[idx].occupied) return false;
        slots[idx] = {true, std::move(cb)};
        return true;
    }

    // 提取并移除回调
    std::optional<completion_callback> extract(uint32_t rid) {
        auto idx = rid % slots.size();
        if (!slots[idx].occupied) return std::nullopt;
        slots[idx].occupied = false;
        return std::move(slots[idx].cb);
    }

    // 检查某个 request_id 是否仍在等待中
    bool contains(uint32_t rid) const {
        auto idx = rid % slots.size();
        return slots[idx].occupied;
    }

    // 连接断开时通知所有等待者（F-CORR-6, F-ERR-4）
    void fail_all(std::exception_ptr ex) {
        for (auto& s : slots) {
            if (s.occupied) {
                s.occupied = false;
                auto cb = std::move(s.cb);
                // 通过回调传递异常，通知等待者连接已断开
                // 具体实现取决于 completion_callback 的异常传播方式
                // 例如：cb 内部调用 push_exception(ex)
            }
        }
    }
};
```

使用 `rid % capacity` 作为槽位索引。由于 request_id 单调递增且 capacity 固定，只要 in-flight 请求数不超过 capacity，就不会冲突。

### 6.2 会话清理机制

当 session 关闭时（F-SESSION-2），需要清理该 session 的 RPC 状态：

- **触发时机**：net 层的 `close_session` / `process_err` 会通知 `recv_q` 中的所有等待者（已实现）。RPC 层在收到异常回调时，应调用 `rpc_state_registry::unbind(sid)` 清理状态
- **pending 请求通知**：`pending_requests_map::fail_all()` 将所有未完成的 call 通过异常传播机制通知下游（F-CORR-6）
- **清理顺序**：先 `fail_all()` 通知所有等待者 → 再 `unbind()` 销毁状态

---

## 七、`rpc::call` 内部机制

### 7.1 核心流程

net 层已支持多 recv 注册（D-16），客户端不需要独立的 recv 循环。每次 call 在发送完成后直接向 session_scheduler 注册 recv 回调，通过 `pending_requests_map` 实现响应匹配：

```
rpc::call<st>(sched, sid, req) 内部：
  1. auto& state = registry.get_or_bind(sid)    // lazy bind（D-9）
  2. auto rid = state.next_request_id++          // 分配 request_id（D-1）
  3. service<st>::encode(req, iov_buf)            // 编码为 iovec（D-10）
     // RPC 层在 iov_buf 中写入 rpc_header（含 rid, module_id, msg_type, flags）
     // net 层 send 会自动添加 uint16_t 长度前缀，编码器无需处理
  4. net::send(sched, sid, vec, cnt)             // 等待发送成功（sender，通过 >> 组合）
  5. state.pending.insert(rid, callback)         // 在 pending_map 中注册 request_id 和回调
  6. net::recv(sched, sid)                       // 向 session_scheduler 注册 recv 回调（sender）
  7. recv 回调收到 recv_frame：
     → 从帧头提取 request_id
     → 在 pending_map 中查找 request_id 对应的回调 → 触发
```

**关键语义**：步骤 4 必须等待 send 完成后，才能执行步骤 5-6。步骤 6 的 recv 回调被 session_scheduler 触发时，收到的帧的 request_id 未必是本次 call 的——可能是其他并发 call 的响应。recv 回调通过 `pending_requests_map` 将帧转交给正确的等待者。

### 7.2 多 call 并发的响应分发

多个应用并发调用 `rpc::call` 时，每个 call 都会向 session_scheduler 注册自己的 recv 回调。当 net 层收到一个响应帧时，只有一个 recv 回调会被触发（按 `recv_q` 的 FIFO 顺序）。该回调根据帧中的 `request_id` 查找 `pending_requests_map`，找到真正等待该响应的 call 并调用其回调函数：

```cpp
// recv 回调（由应用 A 的 call 注册，但处理的帧未必属于应用 A）
auto recv_callback = [&state, sched, sid, my_rid](recv_frame&& frame) {
    auto rid = extract_request_id(frame);     // 从帧头提取 request_id
    if (auto cb = state.pending.extract(rid)) {
        (*cb)(std::move(frame));              // 转交给 request_id 对应的等待者
    }
    // 如果 rid == my_rid，本次 call 的回调已在上面被触发
    // 如果 rid != my_rid，说明收到了其他 call 的响应，已转交
    // 若 my_rid 的响应尚未到达，需要重新注册 recv 继续等待
    if (!state.pending.contains(my_rid)) {
        return;  // my_rid 已完成（可能被其他 recv 回调代为转交）
    }
    // 重新注册 recv 继续等待自己的响应
    net::recv(sched, sid) >> ...;
};
```

**核心机制**：虽然 recv 回调是应用 A 注册的，但帧中的 `request_id` 可能属于应用 B。recv 回调通过 `pending_requests_map` 找到应用 B 的回调并触发，然后如果应用 A 自己的响应尚未到达，继续注册 recv 等待。

### 7.3 跨线程安全

| 资源 | 写入者 | 读取者 | 安全保证 |
|------|--------|--------|----------|
| `send_q` (lock_free_queue) | 任意线程（多生产者） | scheduler 线程（单消费者） | MPSC 无锁队列 |
| `recv_q` (spsc::queue) | scheduler 线程 | scheduler 线程 | 单线程不变量 |
| `pending_requests_map` | scheduler 线程 | scheduler 线程 | 单线程不变量 |
| `next_request_id` | scheduler 线程 | scheduler 线程 | 单线程不变量 |

### 7.4 与 on_data 方案的对比

选择了「每个 call 各自注册 recv」而非「on_data 持久化回调」方案：

| 方面 | on_data 方案 | 多 recv 方案（采用） |
|------|-------------|---------------------|
| 新增概念 | 需要在 net 层加 `on_data` | 不需要，复用现有 recv |
| net 层改动 | 改 `recv_cache` + `on_read_event` | 只需 recv 从单槽位变队列（D-16 已完成） |
| 侵入性 | 需要 `bind` 设置 on_data | 不需要额外 bind |
| 生命周期 | on_data 引用 rpc_state | recv 回调随 call 自然创建销毁 |
| 符合 sender 模型 | on_data 是命令式回调 | recv 本身就是 sender |

---

## 八、编码与 Net 层对齐

- 编码输出为 `iovec*` + `size_t cnt`，与 `net::send(sid, vec, cnt)` 签名直接匹配
- 编码器负责在 `iovec` 中包含完整的发送内容：RPC 帧头 + payload。net 层的 `send` 会自动在数据前添加 `uint16_t` 长度前缀，编码器无需处理长度前缀
- 解码器输入为 `recv_frame`（独立线性 buffer），通过 `frame.as<T>()` struct overlay 零开销解码
- **D-10: 编码内存管理**：
  - RPC 层在 send 路径上持有 `std::array<iovec, 8>` 固定缓冲
  - 编码器签名接收缓冲区引用，写入 iovec 段，返回实际段数
  - 超过 8 段时 throw 异常
  - 缓冲区生命周期与 send op 绑定

---

## 九、服务注册与分发设计

服务端通过模板特化实现服务注册，RPC 层提供编译期分发机制。核心概念：

1. **应用层如何注册服务**：通过 `service<sid>` 模板特化，定义请求类型和 `handle()` 方法（§9.1-9.2）
2. **RPC 层如何在编译期得知服务并进行分发**：通过 `dispatch<service_ids...>()` 编译期展开，根据帧头 `module_id` 路由到对应的 `service<sid>::handle()`（§9.3）
3. **`rpc::serve` 的核心职责**：持续接收请求、解码、分发到 handler、编码响应并发回（§11）

### 9.1 服务注册

```cpp
// 独立文件：service_type.hh
// 应用层新增服务时只需在此文件中添加枚举值，无需修改 RPC 层其他代码
enum class service_type {
    service_001,
    service_002,
    service_003,
    max_service
};

// 由 RPC 层提供：service 基模板
template<service_type sid>
struct service {
    static constexpr bool is_service = false;
};

// 由 RPC 层提供：handle 能力检测
template<typename T, typename Req>
concept has_handle_concept = requires(Req &&r) {
    T::handle(std::forward<Req>(r));
};
```

### 9.2 应用层实现服务

应用层通过模板特化定义具体服务。每个 `service<st>` 特化同时满足 Protocol concept（提供 `message_type`、`decode`、`encode`）和服务注册（提供 `handle`）。完整示例见 §2.1。

简化示例（省略 Protocol 部分，只展示多 handle 重载）：

```cpp
// service_002：多个请求类型
template <>
struct service<service_type::service_002> {
    struct sub_req { int a; int b; };
    struct mul_req { int a; int b; };
    struct sub_resp { int result; };
    struct mul_resp { int result; };
    using message_type = std::variant<sub_req, mul_req, sub_resp, mul_resp>;

    static message_type decode(uint8_t msg_type, const char* payload, size_t len) { /* ... */ }
    static size_t encode(const sub_resp& msg, std::array<iovec, 8>& buf) { /* ... */ }
    static size_t encode(const mul_resp& msg, std::array<iovec, 8>& buf) { /* ... */ }

    static constexpr bool is_service = true;

    static auto handle(sub_req&& req) {
        return just(sub_resp{req.a - req.b});
    }

    static auto handle(mul_req&& req) {
        return just(mul_resp{req.a * req.b});
    }
};
```

### 9.3 服务端分发机制

```cpp
template<service_type ...service_ids>
auto get_service_class_by_id(service_type sid) {
    using res_t = std::variant<service<service_ids>...>;
    std::optional<res_t> result;
    (void)((sid == service_ids && (result.emplace(service<service_ids>{}), true)) || ...);
    if (result) return result.value();
    throw std::logic_error("unknown service type");
}

template<service_type ...service_ids>
auto dispatch() {
    return flat_map([](uint16_t module_id, auto &&msg) {
        return just()
            >> visit(get_service_class_by_id<service_ids...>(
                   static_cast<service_type>(module_id)))
            >> flat_map([req = __fwd__(msg)](auto &&result) mutable {
                if constexpr (std::decay_t<decltype(result)>::is_service) {
                    if constexpr (has_handle_concept<
                            std::decay_t<decltype(result)>, decltype(req)>)
                        return std::decay_t<decltype(result)>::handle(__mov__(req));
                    else
                        return just_exception(
                            std::logic_error("unknown request type"));
                } else {
                    return just_exception(
                        std::logic_error("unknown service type"));
                }
            });
    });
}
```

### 9.4 服务端会话处理

应用层使用 `rpc::serve` 启动服务分发循环（内部机制见第十一节）：

```cpp
rpc::serve<service_type::service_001, service_type::service_002>(session_sched, sid);
```

### 9.5 客户端调用

应用层使用 `rpc::call` / `rpc::send` 发起 RPC 调用（内部机制见第七节）：

```cpp
// rpc::call — 发送请求并等待响应
rpc::call<service_type::service_001>(sched, sid, add_req{1, 2})
    >> then([](auto&& response) {
        // 处理响应
    });

// rpc::send — 单向发送，不等待响应
rpc::send<service_type::service_001>(sched, sid, notification_msg);
```

### 9.6 service_type 与 Protocol Trait 的关系

Protocol 与 service_type **一一对应**。每个 `service<st>` 模板特化同时承担两个角色：

| 角色 | 提供的能力 | 说明 |
|------|-----------|------|
| **Protocol** | `message_type`、`decode()`、`encode()` | 定义线格式：字节 ↔ 消息 |
| **Service** | `is_service`、`handle()` | 定义业务处理逻辑 |

**`module_id = static_cast<uint16_t>(st)`**。每个 service_type 值对应一个 `module_id`，RPC 层从帧头读取 `module_id` 后转为 `service_type` 进行分发：

- RPC 层从统一帧头中读取 `module_id`，转为 `service_type`
- `dispatch<service_ids...>()` 根据 `module_id` 路由到对应的 `service<sid>`
- `service<sid>::decode()` 解码 payload，`service<sid>::handle()` 处理请求

---

## 十、错误处理设计

### 10.1 错误类型层次

```cpp
namespace pump::scheduler::rpc {
    // 基类：所有 RPC 层错误
    struct rpc_error : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    // 帧错误：RPC 帧头解析时检测到非法帧格式
    struct frame_error : rpc_error {
        using rpc_error::rpc_error;
    };

    // 编解码错误：序列化/反序列化失败
    struct codec_error : rpc_error {
        using rpc_error::rpc_error;
    };

    // 协议错误：未知消息类型、版本不兼容
    struct protocol_error : rpc_error {
        using rpc_error::rpc_error;
    };

    // 服务分发错误：未知 service_type 或无匹配 handler
    struct dispatch_error : rpc_error {
        using rpc_error::rpc_error;
    };
}
```

### 10.2 错误传播路径

| 错误来源 | 检测位置 | 传播方式 |
|----------|----------|----------|
| 连接断开 / 发送失败 | net 层 | `push_exception` → 下游，同时触发 `fail_all()` |
| 帧格式非法 | RPC 帧头解析（帧头字段不合法） | `push_exception(frame_error)` |
| 反序列化失败 | `service<st>::decode` | `push_exception(codec_error)` |
| 未知消息类型 | `dispatch` | `push_exception(protocol_error)` |
| pending 容量满 | `pending_requests_map::insert` | `push_exception(rpc_error)` |

### 10.3 帧错误恢复策略

F-ERR-3 要求帧错误不导致连接关闭。由于 net 层已保证 `recv_frame` 的完整性（通过长度前缀），帧错误主要发生在 RPC 层解析帧头内容时（如未知 msg_type、flags 不合法等）：
- RPC 层检测到帧头内容非法时，向下游发送 `frame_error` 异常，由上层决定是否关闭连接
- 当前帧被丢弃，继续处理下一个 `recv_frame`（每个 recv_frame 是独立的包，一个帧的错误不影响后续帧）

---

## 十一、rpc::serve 内部机制

### 11.1 核心流程

```
rpc::serve<sids...>(sched, sid) 内部：
  1. auto& state = registry.get_or_bind(sid)    // lazy bind（D-9）
  2. 进入循环（由 for_each + check_session 驱动）：
     a. net::recv(sched, sid)                   // 接收 recv_frame
     b. 从帧头提取 request_id、module_id、msg_type
     c. 根据 module_id 路由到 service<sid>::decode(msg_type, payload, len)  // 解码为消息
     d. dispatch<sids...>(module_id, msg)       // 路由到 service<sid>::handle()
     e. handler 返回 response sender
     f. encode(response, iov_buf)               // 编码响应，写入原 request_id
     g. net::send(sched, sid, vec, cnt)          // 发回响应
  3. 循环直到 session 关闭
```

### 11.2 与 rpc::call 的区别

| 方面 | rpc::call | rpc::serve |
|------|-----------|------------|
| recv 用途 | 等待特定 request_id 的响应 | 接收任意请求 |
| pending_map | 需要（等待响应匹配） | 不需要（请求到达即处理） |
| request_id | 由客户端生成 | 从收到的请求中提取，原样写入响应 |
| 生命周期 | 一次性（发送 → 等待 → 完成） | 持续循环直到 session 关闭 |

### 11.3 handler 响应发送语义（D-15）

handler 只返回响应消息的 sender，不感知连接：

```cpp
// serve 内部伪代码
auto handle_request = [&state, sched, sid](recv_frame&& frame) {
    auto* hdr = frame.as<rpc_header>();
    auto rid = hdr->request_id;                 // 从帧头提取
    auto module = hdr->module_id;               // 从帧头提取
    auto msg_type = hdr->msg_type;              // 从帧头提取
    auto* payload = frame.data() + sizeof(rpc_header);
    auto payload_len = frame.size() - sizeof(rpc_header);
    // 先 decode，再 dispatch（与 §9.3 签名一致）
    auto msg = service_decode<sids...>(module, msg_type, payload, payload_len);
    return dispatch<sids...>(module, std::move(msg))
        >> then([sched, sid, rid](auto&& response) {
            std::array<iovec, 8> iov_buf;
            auto cnt = encode(response, iov_buf);
            write_request_id(iov_buf, rid);     // 写入原始 request_id
            return scheduler::net::send(sched, sid, iov_buf.data(), cnt);
        })
        >> flat();
};
```

---

## 十二、待完善设计点

以下设计点在实现过程中需要进一步确认：

| 编号 | 问题 | 当前状态 | 影响 |
|------|------|----------|------|
| Q-1 | `Protocol::encode` 如何纳入 concept 约束 | concept 中仅有注释，未写 requires 表达式 | 每种消息类型有独立重载，无法用单一 requires 表达；可能需要辅助 trait 或放弃编译期检查 |
| ~~Q-2~~ | ~~net 层 `copy_out_frame` 的泛化~~ | **已关闭**：net 层通过 `uint16_t` 长度前缀完成分帧，RPC 层不参与分帧，无需泛化 `copy_out_frame` | 无 |
| ~~Q-3~~ | ~~`service_type` 枚举的扩展方式~~ | **已关闭**：`service_type` 枚举定义在独立文件 `service/service_type.hh` 中，应用层新增服务只需在该文件中添加枚举值，不涉及 RPC 层的核心代码（dispatch、pending_map、sender 等） | 无 |
| Q-4 | `rpc::recv` 独立使用时与 `rpc::call` 的 recv 共存 | 7.2 节仅描述 call 内部的 recv 分发 | 若同一 session 上同时有 call 和独立 recv，两者的 recv 回调如何协调需要明确 |

---

## 十三、实现计划

### 13.1 文件结构

```
src/env/scheduler/rpc/
├── common/
│   ├── struct.hh          # rpc_header, error types
│   └── protocol.hh        # Protocol concept 定义（decode/encode 约束）
├── state/
│   ├── pending_map.hh     # pending_requests_map
│   └── registry.hh        # session_rpc_state, rpc_state_registry
├── senders/
│   ├── call.hh            # rpc::call sender (op/sender/op_pusher/compute_sender_type)
│   ├── send.hh            # rpc::send sender
│   ├── recv.hh            # rpc::recv sender (协议级，解码后的消息)
│   └── serve.hh           # rpc::serve sender
├── service/
│   ├── service_type.hh    # service_type 枚举定义（应用层修改此文件新增服务）
│   ├── service.hh         # service 基模板, has_handle_concept
│   └── dispatch.hh        # get_service_class_by_id, dispatch()
└── rpc.hh                 # 统一入口头文件
```

### 13.2 实现阶段

#### Phase 1：基础设施（无外部依赖，可独立编译测试）

| 步骤 | 内容 | 产出文件 | 对应需求 |
|------|------|----------|----------|
| 1.1 | RPC 帧头定义 + 错误类型 | `common/struct.hh` | F-FRAME-1, F-ERR-2 |
| 1.2 | Protocol concept 定义 | `common/protocol.hh` | F-PROTO-1~3 |
| 1.3 | pending_requests_map | `state/pending_map.hh` | F-CORR-1~2, P-LAT-2, P-MEM-1 |
| 1.4 | session_rpc_state + registry | `state/registry.hh` | F-SESSION-1~4, D-9 |
| 1.5 | service 基模板 + dispatch | `service/service.hh`, `service/dispatch.hh` | F-SVC-1~5 |
| 1.6 | 单元测试：pending_map、registry、dispatch | `apps/test/rpc/` | NF-TEST-1~2 |

#### Phase 2：Sender 实现（依赖 Phase 1 + net 层）

| 步骤 | 内容 | 产出文件 | 对应需求 |
|------|------|----------|----------|
| 2.1 | `rpc::send` sender | `senders/send.hh` | F-API-2, F-CODEC-1, F-CODEC-4 |
| 2.2 | `rpc::recv` sender | `senders/recv.hh` | F-API-3, F-CODEC-2, F-CODEC-5 |
| 2.3 | `rpc::call` sender | `senders/call.hh` | F-API-1, F-CORR-1~2, F-CORR-5 |
| 2.4 | `rpc::serve` sender | `senders/serve.hh` | F-API-4, D-15 |
| 2.5 | 统一入口 + flat_map 版本 | `rpc.hh` | F-API-5 |
| 2.6 | 集成测试：echo 协议的 call/serve | `apps/test/rpc/` | F-API-6~8, F-INT-5~7 |

每个 sender 需实现四件套：`op` / `sender` / `op_pusher` 特化 / `compute_sender_type` 特化，参考 `net/senders/connect.hh` 的模式。

#### Phase 3：net 层适配（可能需要修改 net 层）

| 步骤 | 内容 | 影响文件 | 对应需求 |
|------|------|----------|----------|
| 3.1 | 会话关闭时的 RPC 状态清理钩子 | `net/io_uring/scheduler.hh`, `net/epoll/scheduler.hh` | F-SESSION-2, F-CORR-6 |

#### Phase 4：示例协议 + 端到端验证

| 步骤 | 内容 | 产出文件 | 对应需求 |
|------|------|----------|----------|
| 4.1 | 实现一个完整的示例协议（如简单 KV 协议） | `apps/example/rpc_kv/` | NF-DOC-2, F-PROTO-4 |
| 4.2 | 示例：客户端 call + 服务端 serve | 同上 | F-API-1, F-API-4 |
| 4.3 | 示例：单向 send + recv | 同上 | F-API-2, F-API-3 |
| 4.4 | 性能基准测试 | `apps/test/rpc/bench/` | P-LAT-1, P-THR-1~2, P-MEM-1~2 |

### 13.3 依赖关系

```
Phase 1 (基础设施)
    │
    ├──→ Phase 2 (Sender 实现)
    │        │
    │        └──→ Phase 4 (示例 + 端到端)
    │
    └──→ Phase 3 (net 层适配)
             │
             └──→ Phase 4
```

Phase 2 和 Phase 3 可以并行推进。net 层现有的 `uint16_t` 长度前缀分帧机制直接使用，RPC 层不需要修改 net 层的分帧逻辑。
