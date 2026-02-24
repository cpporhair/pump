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
| D-4 | 连接级握手 | 协议层通过注册和实现"握手"RPC 命令自行处理 | F-EXT-4 降为 P2 |
| D-5 | 消息大小限制 | 本版不实现 | 无需额外需求 |
| D-6 | RESP 按序响应 | 不为 RESP 破坏 RPC 层的 request_id 设计；RESP 将来可直接架构在 Net 层上 | RPC 层只需支持基于 request_id 的乱序响应匹配 |
| D-7 | pending_requests 容量 | 默认 256，可通过 session 初始化时配置调整 | session_rpc_state 初始化 |
| D-8 | 多帧推送策略 | 本版单次推送（每帧一次 push），不做批量推送 | P-THR-3 降为 P2 |
| D-9 | session RPC 状态存储 | RPC 层维护独立的 `session_id → rpc_state` 映射，通过 `get_or_bind` 首次访问时自动创建（lazy bind） | 符合 F-INT-1 |
| D-10 | 编码内存管理 | 栈/预分配固定缓冲（`std::array<iovec, 8>`），编码器将 iovec 写入 RPC 层预分配的固定大小缓冲区 | P-MEM-2, P-THR-2, F-CODEC-4 |
| D-11 | `rpc::recv` 消息类型 | 返回扁平化的 `std::variant<具体消息类型...>`，下游通过 PUMP 的 `visit()` 算子解包后用 `if constexpr` 分支处理。不使用嵌套 variant | F-API-3 |
| D-12 | handler 返回值 | 每个命令的 handler 函数统一返回 sender（如 `just(result)`），保持 pump 管道风格一致性 | 使用示例 |
| D-13 | 连接池、调度、路由均为上层职责 | RPC 层不引入 dispatcher、connection_pool、模块路由。RPC 层只关心「用哪个 session 完成一次 RPC 调用」 | 1.1 职责边界 |
| D-14 | module_id | `uint16_t` 类型，每个协议特征必须声明，嵌入 RPC 统一帧头。RPC 层只负责将其写入帧头，路由逻辑由上层实现 | 帧头设计 |
| D-15 | handler 响应发送语义 | `rpc::serve` 在调用 handler 后自动将返回值编码并通过同一 session 发回。handler 只需返回响应消息（sender），不需感知连接 ID | serve 语义 |
| D-16 | net 层 recv 机制重构 | net 层已从「暴露 `packet_buffer*` 指针 + atomic 单槽位」改为「copy-out 独立帧 `recv_frame` + 多 recv 队列」模式。`recv_cache` 从单槽位改为 `recv_q` + `ready_q`；`on_read_event` 在数据到达时自动分帧并 copy-out | F-INT-1, F-FRAME-5, F-CODEC-5 |

---

## 二、协议特征（Protocol Trait）设计

上层协议通过满足以下 concept 接入 RPC 层：

```cpp
struct frame_result {
    enum status { complete, incomplete, error };
    status st;
    size_t frame_offset;
    size_t frame_length;
};

enum class message_kind {
    request,
    response,
    push
};

template <typename P>
concept Protocol = requires {
    // 模块标识（D-14）
    { P::MODULE_ID } -> std::convertible_to<uint16_t>;

    // 扁平化的消息 variant 类型（D-11）
    typename P::message_type;

    // 分帧：给定 packet_buffer，判断是否包含完整帧
    { P::try_parse_frame(std::declval<const packet_buffer&>()) }
        -> std::same_as<frame_result>;

    // 解码：将 copy-out 后的独立帧反序列化为 message_type
    { P::decode(std::declval<const recv_frame&>()) }
        -> std::same_as<typename P::message_type>;

    // 编码：将消息序列化为 iovec 数组（D-10）
    // 每种消息类型提供独立重载

    // 从消息中提取 request_id（D-1）
    { P::get_request_id(std::declval<const typename P::message_type&>()) }
        -> std::same_as<uint32_t>;

    // 判断消息类别
    { P::classify(std::declval<const recv_frame&>()) }
        -> std::same_as<message_kind>;
};
```

### 2.1 协议特征实现示例

```cpp
struct MyProtocol {
    static constexpr uint16_t MODULE_ID = 0x0001;
    using message_type = std::variant<request_a, request_b, response_a, response_b>;

    static frame_result try_parse_frame(const packet_buffer& buf) {
        constexpr size_t HEADER_SIZE = sizeof(my_header);
        if (buf.used() < HEADER_SIZE)
            return { .st = incomplete };
        uint32_t total_len = buf.handle_data(sizeof(uint32_t),
            [](const char* d, size_t) { return read_u32(d); });
        if (buf.used() < total_len)
            return { .st = incomplete };
        return { .st = complete, .frame_offset = buf.head(), .frame_length = total_len };
    }

    static message_type decode(const recv_frame& frame) {
        auto* hdr = frame.as<my_header>();
        switch (hdr->msg_type) {
            case 0x01: return *frame.as<request_a>();
            case 0x02: return *frame.as<request_b>();
            case 0x03: return *frame.as<response_a>();
            case 0x04: return *frame.as<response_b>();
            default:   throw rpc::protocol_error("unknown msg_type");
        }
    }

    static size_t encode(const request_a& msg, std::array<iovec, 8>& buf) {
        return do_encode(msg, buf);
    }
    // ... 其他消息类型的 encode 重载

    static uint32_t get_request_id(const message_type& msg) {
        return std::visit([](const auto& m) { return m.request_id; }, msg);
    }

    static message_kind classify(const recv_frame& frame) {
        auto* hdr = frame.as<my_header>();
        return (hdr->msg_type <= 0x02) ? message_kind::request : message_kind::response;
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

- `module_id`：由 `Protocol::MODULE_ID` 决定，RPC 层将其写入帧头，上层据此实现消息路由
- 帧头格式为 RPC 层统一定义，协议特征的 `try_parse_frame` 在帧头之后处理协议特定部分

---

## 四、分帧器工作流程

net 层的 `on_read_event` 在数据到达时自动完成分帧和 copy-out：

```
io_uring/epoll 事件循环:
  数据到达 → buf.forward_tail(res)
  → 循环:
      copy_out_frame(&buf)
      if frame.size() > 0:
          if recv_q 有等待的回调 → 触发回调，传递 recv_frame
          else → 暂存到 ready_q
          buf.forward_head(frame_length)
          继续循环（处理粘包）
      if frame.size() == 0:
          break  // 不完整帧，等待更多数据
```

RPC 层在 net 层之上完成协议级别的解码：

```
net::recv(sid)
  → recv_frame&&
  → Protocol::decode(frame)  // struct overlay 解码
  → 推送给下游
```

---

## 五、Sender API 签名设计

```cpp
namespace pump::scheduler::rpc {

    // --- rpc::call ---
    template <Protocol P, typename scheduler_t>
    auto call(scheduler_t* sche, session_id_t sid, typename P::request_type req);
    // 返回 sender，完成值类型为 P::response_type

    template <Protocol P>
    auto call(session_id_t sid, typename P::request_type req);  // flat_map 版本

    // --- rpc::send ---
    template <Protocol P, typename scheduler_t>
    auto send(scheduler_t* sche, session_id_t sid, auto&& msg);

    template <Protocol P>
    auto send(session_id_t sid, auto&& msg);

    // --- rpc::recv ---
    template <Protocol P, typename scheduler_t>
    auto recv(scheduler_t* sche, session_id_t sid);
    // 返回 sender，完成值类型为 P::message_type（扁平 variant）

    template <Protocol P>
    auto recv(session_id_t sid);

    // --- rpc::serve ---
    template <typename scheduler_t, typename... Services>
    auto serve(scheduler_t* sche, session_id_t sid);

    template <typename... Services>
    auto serve(session_id_t sid);
}
```

---

## 六、会话 RPC 状态设计

每个绑定了协议特征的 session 维护以下状态（D-9）：

```cpp
template <Protocol P>
struct session_rpc_state {
    uint32_t next_request_id = 0;                                    // D-1
    pending_requests_map<uint32_t, completion_callback> pending{256}; // D-7
    // 分帧器状态无需额外缓存：net 层 packet_buffer 天然支持部分帧保留
};

// D-9: RPC 层独立维护 session_id → rpc_state 映射
template <Protocol P>
struct rpc_state_registry {
    std::unordered_map<session_id_t, session_rpc_state<P>> states;

    auto& get_or_bind(session_id_t sid) {
        auto it = states.find(sid);
        if (it != states.end()) return it->second;
        return states.emplace(sid, session_rpc_state<P>{}).first->second;
    }

    void unbind(session_id_t sid) { states.erase(sid); }
};
```

**绑定时机**：`rpc::call` / `rpc::serve` / `rpc::recv` 内部首次对某个 session 进行 RPC 操作时，通过 `get_or_bind(sid)` 自动创建状态（lazy init）。

---

## 七、`rpc::call` 内部机制

### 7.1 核心流程

```
rpc::call<P>(sched, sid, req) 内部：
  1. auto& state = registry.get_or_bind(sid)    // lazy bind（D-9）
  2. auto rid = state.next_request_id++          // 分配 request_id（D-1）
  3. P::encode(req, iov_buf)                     // 编码为 iovec（D-10）
  4. net::send(sched, sid, vec, cnt)             // 发送
  5. pending_map[rid] = continuation             // 注册等待回调
  6. net::recv(sched, sid)                       // 注册 recv
  7. recv 回调收到 recv_frame：
     → P::classify(frame) → P::get_request_id(P::decode(frame))
     → 在 pending_map 中查找对应的 callback → 触发
```

### 7.2 多 call 并发的响应分发

每个 `rpc::call` 各自注册一个 recv，但 recv 回调拿到的帧可能不属于自己（TCP 字节流中响应乱序到达）。recv 回调通过 `pending_requests_map` 查找真正的等待者：

```cpp
auto recv_callback = [&state, my_rid](recv_frame&& frame) {
    auto msg = P::decode(frame);
    auto rid = P::get_request_id(msg);
    if (rid == my_rid) {
        return just(std::move(msg));       // 恰好是自己的响应
    } else {
        if (auto cb = state.pending.extract(rid))
            cb(std::move(msg));            // 转交给真正的等待者
        return net::recv(sched, sid) >> then(/* 继续等待 */);
    }
};
```

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
- 编码器负责在 `iovec` 中包含帧头，确保接收端分帧器能识别
- 解码器输入为 `recv_frame`（独立线性 buffer），通过 `frame.as<T>()` struct overlay 零开销解码
- **D-10: 编码内存管理**：
  - RPC 层在 send 路径上持有 `std::array<iovec, 8>` 固定缓冲
  - 编码器签名接收缓冲区引用，写入 iovec 段，返回实际段数
  - 超过 8 段时 throw 异常
  - 缓冲区生命周期与 send op 绑定

---

## 九、服务注册与分发设计

> 参考 rpc.md 中的设计思路，服务端通过模板特化实现服务注册，RPC 层提供编译期分发机制。

### 9.1 服务注册

```cpp
// 由 RPC 层提供：服务类型枚举（应用层增加新的 service_type 值）
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

应用层通过模板特化定义具体服务，每个服务可有多个 `handle` 重载处理不同请求类型：

```cpp
// 应用层 A 实现 service_001
template <>
struct service<service_type::service_001> {
    struct add_req { int a; int b; };
    struct sleep_and_add_req { int a; int b; int sleep_ms; };

    static constexpr bool is_service = true;

    static auto handle(add_req&& req) {
        return just(req.a + req.b);
    }

    static auto handle(sleep_and_add_req&& req) {
        return just()
            >> then([req = __fwd__(req)]() {
                return ... /*异步操作的pipeline*/;
            })
            >> flat();
    }
};

// 应用层 B 实现 service_002
template <>
struct service<service_type::service_002> {
    struct sub_req { int a; int b; };
    static constexpr bool is_service = true;

    static auto handle(sub_req&& req) {
        return just(req.a - req.b);
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
    return flat_map([](uint16_t service_id, auto &&msg) {
        return just()
            >> visit(get_service_class_by_id<service_ids...>(
                   static_cast<service_type>(service_id)))
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

```cpp
template<service_type ...service_ids>
auto dispatch_proc(session_data& sd) {
    return for_each(coro::make_view_able(check_session(sd)))
        >> flat_map([&sd](auto&& is_running) {
            return scheduler::net::recv(sd.scheduler, sd.id);
        })
        >> decode_msg(...)
        >> dispatch<service_ids...>()
        >> then([](auto&& response) {
            return scheduler::net::send(sd.scheduler, sd.id, ... /*response*/);
        })
        >> reduce();
}

template<service_type ...service_ids>
auto serv(session_scheduler_t* sc, scheduler::net::common::session_id_t id) {
    return scheduler::net::join(session_sched, sid)
        >> dispatch_proc<service_ids...>(session_data<session_scheduler_t>(sc, id));
}
```

### 9.5 客户端调用

```cpp
// rpc::call — 发送请求并等待响应
template<service_type st>
auto call(session_scheduler_t* sc, session_id_t id, auto&& req) {
    return rpc::encode_msg<st>(req)
        >> flat_map([sc, id](char* buf, size_t len) {
            return scheduler::net::send(sc, id, buf, len);
        })
        >> rpc::wait_response_at(sc, id);
}

// rpc::send — 单向发送，不等待响应
auto send(session_scheduler_t* sc, session_id_t id, auto&& req) {
    return rpc::encode_msg<st>(req)
        >> flat_map([sc, id](char* buf, size_t len) {
            return scheduler::net::send(sc, id, buf, len);
        });
}
```
