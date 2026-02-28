# PUMP RPC 层需求文档

## 一、背景与目标

### 1.1 现状

Net 层已实现底层网络收发：

| 组件 | 能力 |
|------|------|
| accept_scheduler / connect_scheduler | 服务端监听、客户端连接，产出 `session_id_t` |
| session_scheduler | `join / recv / send / stop`，管理会话 IO |
| 消息帧 | 2 字节 `uint16_t` 长度前缀 + payload（应用层手动构造） |
| 接收 | SPSC ring buffer，零拷贝包解析（`has_full_pkt / get_recv_pkt`） |
| 发送 | iovec scatter-gather，`writev` 直接写入（不自动加长度前缀） |

### 1.2 分层规划

```
┌────────────────────────────────────────────┐
│   应用协议层 (Raft / RESP / MQ / ...)      │
│   实现具体协议逻辑，定义方法和业务编解码     │
├────────────────────────────────────────────┤
│   RPC 层（本文档的设计目标）                │
│   异步请求-响应 · 消息分发 · 编解码框架     │
│   会话管理 · 接收循环 · 通知推送            │
├────────────────────────────────────────────┤
│   Net 层（已实现）                          │
│   TCP 连接管理 · 字节流收发 · 消息帧        │
└────────────────────────────────────────────┘
```

### 1.3 目标

在 Net 层之上构建 RPC 层：
1. **异步请求-响应**：核心 RPC 能力，发送请求、异步等待响应，返回 sender 可组合
2. **消息分发**：根据消息类型/方法 ID 路由到处理器
3. **编解码框架**：可插拔，不绑定任何序列化格式
4. **协议扩展性**：Raft/RESP/MQ 等不同模式的协议都能在 RPC 层之上实现

### 1.4 设计原则

| 原则 | 要求 |
|------|------|
| 极致效率 | 零拷贝读写、无锁、热路径无堆分配、编译期分发 |
| 异步抽象 | 所有操作返回 sender，可与 PUMP pipeline 组合 |
| 协议无关 | RPC 层不绑定具体序列化格式或方法体系 |
| PUMP 一致 | 遵循 Lock-Free、Single-Thread Scheduler、Sender 语义 |

---

## 二、各层职责边界

| 层 | 负责 | 不负责 |
|----|------|--------|
| **Net** | TCP 连接管理、字节流收发、消息帧（长度前缀 + payload） | 消息语义、序列化、路由 |
| **RPC** | 消息模型、请求关联、分发框架、编解码抽象、接收循环 | 具体协议逻辑、业务方法实现 |
| **协议** | 定义方法/命令、实现编解码、业务逻辑 | 底层网络、消息帧、请求跟踪 |

**关键边界问题**：消息帧（长度前缀）是属于 Net 层还是 RPC 层？
- 当前 Net 层：recv 端解析 2 字节长度前缀（`has_full_pkt`），send 端不自动加前缀
- RPC 层可能需要更大的帧（>64KB），或不同的帧格式
- **建议**：RPC 层接管消息帧定义，Net 层提供纯字节流收发；或将帧格式参数化

---

## 三、RPC 消息模型

### 3.1 消息类型

| 类型 | 语义 | 方向 | 需要关联 ID |
|------|------|------|-------------|
| **Request** | 请求，期待 Response | 双向均可发起 | 是 |
| **Response** | 对 Request 的回复 | 反向 | 是（匹配 Request） |
| **Notification** | 单向通知，不期待回复 | 双向均可发起 | 否 |
| **Error** | 错误响应（特殊的 Response） | 反向 | 是（匹配 Request） |

**为什么需要 Notification**：
- 心跳/keepalive
- MQ 消息推送
- Raft leader 通知
- 事件广播

### 3.2 消息需要携带的信息

不规定具体的二进制编码（由 Codec 决定），但每条 RPC 消息在逻辑上需要以下字段：

| 字段 | 说明 | Request | Response | Notification |
|------|------|---------|----------|-------------|
| type | 消息类型标识 | ✓ | ✓ | ✓ |
| request_id | 请求关联 ID | ✓ | ✓ | ✗ |
| method_id | 方法/命令标识 | ✓ | 可选 | ✓ |
| payload | 业务数据 | ✓ | ✓ | ✓ |
| error_code | 错误码 | ✗ | 仅 Error | ✗ |

### 3.3 消息大小

**需求**：必须支持大消息（>64KB）。

理由：
- Raft InstallSnapshot 传输完整状态快照，可能达到 MB 级别
- MQ 消息体可能较大
- 当前 Net 层 `uint16_t` 长度前缀最大 65535 字节，不够用

**方案选项**（待讨论）：
1. 将 Net 层长度前缀扩展为 4 字节（`uint32_t`），最大 ~4GB
2. RPC 层使用自己的帧格式，Net 层退化为纯字节流
3. 长度前缀大小参数化（模板参数或配置）

---

## 四、核心功能需求

### 4.1 编解码框架（Codec）

#### 4.1.1 需求

1. 提供 Codec concept/trait，由协议层实现具体编解码
2. RPC 框架通过 Codec 操作消息，不假设任何固定格式
3. Codec 负责 RPC 头部 + payload 的完整编解码

#### 4.1.2 Codec 必须提供的能力

| 能力 | 输入 | 输出 | 说明 |
|------|------|------|------|
| `decode_header` | buffer 视图 | `{type, request_id, method_id, payload_offset, total_len}` | 从原始字节解析消息头 |
| `decode_payload<T>` | buffer 视图 + payload 区域 | `T` | 反序列化 payload 为类型 T |
| `encode` | 消息头字段 + payload | `iovec[]` | 将完整消息序列化为可发送的 iovec |
| `header_size` | — | `size_t` | 消息头固定长度（用于预读判断） |

#### 4.1.3 零拷贝约束

- **decode 必须支持零拷贝**：直接从 `packet_buffer`（ring buffer）读取，返回视图/引用而非拷贝
- **ring buffer 环绕处理**：Codec 需要处理数据跨 ring buffer 边界的情况（或由 RPC 层提供线性化辅助）
- **encode 直接写入 iovec**：避免中间缓冲区，直接构造可 writev 的 iovec 数组
- 对于跨步骤需要持有的数据，由上层决定是否拷贝

#### 4.1.4 扩展性

- 不同协议使用不同的 Codec：
  - Raft: 紧凑的二进制 Codec
  - RESP: Redis 行协议 Codec（`+OK\r\n`、`$5\r\nhello\r\n` 等）
  - MQ: 可能使用 protobuf 或 flatbuffers
- Codec 通过模板参数注入到 RPC Channel

### 4.2 请求-响应关联

#### 4.2.1 核心需求

1. **请求 ID 生成**：每个 Channel 维护单调递增的 `uint32_t` 请求 ID
2. **Pending Request Map**：
   - 发送 Request 时注册：`{request_id → callback}`
   - 收到 Response 时查找并匹配
   - 匹配成功后通过 callback 继续调用方的 sender pipeline（类似 scheduler 的 req/cb 模式）
3. **超时**：
   - 支持 per-request 超时配置
   - 超时后自动移除 pending entry，向调用方传播超时异常
   - 利用 `task_scheduler` 的 `delay` sender 实现超时检测
4. **连接复用**：
   - 单连接支持多个并发 in-flight 请求（不同 request_id）
   - 响应可以乱序到达（按 request_id 匹配，而非按顺序）
5. **Pipelining**：
   - 支持连续发送多个请求而不等待前一个响应
   - RESP 等协议需要此能力

#### 4.2.2 性能约束

- Pending map 在单线程（session 所在 core 的 scheduler）上操作，**无需锁**
- 查找复杂度 O(1)（数组索引 或 open-addressing hash map）
- 热路径无堆分配：预分配固定大小的 pending slot 数组
- 最大并发请求数可配置（预分配大小的上限）

#### 4.2.3 生命周期

- Channel 关闭时，所有未完成的 pending request 收到 `connection_closed` 异常
- 请求超时后，即使后来响应到达也应丢弃（request_id 已从 map 中移除）

### 4.3 消息分发（Dispatch）

#### 4.3.1 需求

1. **按 method_id 路由**：收到 Request/Notification 后，查找 method_id 对应的处理器
2. **处理器注册**：
   - 运行期注册：callback 表（`method_id → handler`）
   - 编译期注册（可选）：模板特化，零运行时开销
3. **处理器签名**：
   - 同步处理器：接收 Request payload，返回 Response 值
   - 异步处理器：接收 Request payload，返回 `sender<Response>`
4. **自动响应**：Request handler 返回后，框架自动 encode Response 并发送（使用原 request_id）
5. **默认处理器**：未注册 method 的请求交给默认处理器，或自动回复 `MethodNotFound` Error

#### 4.3.2 分发流程

```
收到消息 → Codec::decode_header →
  type == Response/Error → 查找 pending_map[request_id] → 执行 callback → 继续调用方 pipeline
  type == Request        → 查找 handler[method_id] → 执行 handler → encode Response → send
  type == Notification   → 查找 handler[method_id] → 执行 handler（无 Response）
```

#### 4.3.3 性能约束

- method_id 路由查找 O(1)（数组或编译期 switch）
- 分发过程无堆分配
- 处理器调用无虚函数（模板或 `move_only_function`）

### 4.4 接收循环（Recv Loop）

#### 4.4.1 需求

1. 每个 RPC Channel 启动一个持续运行的接收循环
2. 不断调用 `net::recv` 读取数据
3. 每次 recv 返回后，从 `packet_buffer` 中解析出所有完整消息（一次可能有多条）
4. 对每条消息执行 4.3 的分发逻辑
5. 接收循环生命周期与 Channel 绑定

#### 4.4.2 消息批处理

ring buffer 一次 recv 可能包含多个完整消息。接收循环必须在一次 advance 中处理所有可用消息，而非每次只处理一条。

#### 4.4.3 与发送的关系

- 接收和发送是独立的 pipeline
- 接收循环通过 pending request callback 与发送方的 pipeline 交互
- 两者运行在同一个 scheduler 线程上（session 所在 core），无竞态
- 发送可以从任意 core 发起（通过 session_scheduler 的 lock-free send queue 跨核投递）

### 4.5 RPC Channel（会话管理）

#### 4.5.1 概念

一个 **RPC Channel** 对应一个 net session，封装该连接上所有 RPC 层状态。

#### 4.5.2 Channel 状态

| 状态 | 说明 |
|------|------|
| `session_id` | 底层 net session 标识 |
| `request_id_counter` | 单调递增，用于生成请求 ID |
| `pending_requests` | `{request_id → callback}` 映射 |
| `dispatcher` | 消息处理器注册表 |
| `codec` | 编解码器实例（模板参数） |
| `protocol_state` | 上层协议附加的 per-session 状态（模板参数注入） |
| `status` | Channel 状态（active / draining / closed） |

#### 4.5.3 Channel 生命周期

| 阶段 | 触发 | 行为 |
|------|------|------|
| 创建 | connect/accept 成功后 | 初始化状态，绑定 session_id |
| 启动 | 调用 `serve()` 或第一次 `call()` | 启动接收循环 |
| 运行 | 正常工作 | 收发消息、分发处理 |
| 关闭 | 连接断开 / 主动 close | 停止接收循环，清理所有 pending requests（异常通知），释放资源 |

#### 4.5.4 双向 RPC

Channel 必须支持**双向通信**——两端都可以发起 Request：
- Raft: Leader 向 Follower 发 AppendEntries，Follower 也可以向 Leader 发 RequestVote
- MQ: Broker 主动推送消息给 Subscriber，Subscriber 也可以发 ACK

这意味着 Channel 的两端角色对等，每端都有：dispatcher（处理收到的 Request）+ 发起 call（发送 Request 等待 Response）。

### 4.6 通知/推送（Notification）

#### 4.6.1 需求

1. 发送方可以发送不需要回复的单向消息
2. 接收方通过注册的 notification handler 处理
3. 不分配 request ID，不进入 pending map
4. 发送后立即完成（fire-and-forget），不阻塞

#### 4.6.2 用途

- Heartbeat/Keepalive
- 事件通知
- MQ 消息推送
- 状态变更广播

### 4.7 连接管理

#### 4.7.1 心跳/保活（可选）

| 需求 | 说明 |
|------|------|
| 可配置 | Channel 可选启用/禁用心跳 |
| 间隔可调 | 心跳间隔由协议决定 |
| 超时检测 | 连续 N 次未收到心跳/任何消息则判定死连接 |
| 实现 | 利用 task_scheduler 的 delay sender 定时发送 Notification |

#### 4.7.2 优雅关闭

1. 停止接受新的 RPC 请求（drain 模式）
2. 等待已发送的 pending requests 完成或超时
3. 发送关闭通知（可选，协议层决定）
4. 调用 `net::stop` 关闭底层 session

#### 4.7.3 连接断开处理

1. 检测到连接断开时（recv 返回异常/EOF）
2. 所有 pending requests 收到 `connection_lost` 异常
3. 接收循环终止
4. Channel 状态置为 closed
5. 通知上层协议（callback 或 exception 传播）

---

## 五、Sender API 需求

### 5.1 客户端 RPC 调用

```cpp
// 发起请求，等待响应（返回 sender<ResponseType>）
rpc::call(channel, method_id, request)
    >> then([](auto&& response) { /* 处理响应 */ });

// 带超时的调用
rpc::call(channel, method_id, request, timeout_ms)
    >> then([](auto&& response) { ... })
    >> any_exception([](auto e) { /* 超时或其他错误 */ });

// 发送通知（fire-and-forget，返回 sender<void>）
rpc::notify(channel, method_id, message);
```

### 5.2 服务端注册与启动

```cpp
// 注册方法处理器
dispatcher.on(METHOD_FOO, [](FooRequest&& req) {
    return FooResponse{...};  // 同步处理器
});

dispatcher.on(METHOD_BAR, [](BarRequest&& req) {
    return just() >> async_process(req);  // 异步处理器，返回 sender
});

// 启动 RPC 服务（接收循环 + 分发）
rpc::serve(channel, dispatcher) >> submit(ctx);
```

### 5.3 Channel 创建

```cpp
// 从 net session 创建 RPC channel
auto channel = rpc::make_channel<MyCodec>(session_id, session_scheduler);

// 在 pipeline 中使用
connect(sched, addr, port)
    >> then([sched](session_id_t sid) {
        return net::join(sched, sid)
            >> then([sid, sched]() {
                auto ch = rpc::make_channel<MyCodec>(sid, sched);
                return rpc::call(ch, METHOD_HELLO, HelloReq{});
            }) >> flat();
    }) >> flat()
    >> then([](HelloResp&& resp) { ... });
```

### 5.4 组合能力要求

RPC 操作必须是可组合的 sender，支持所有 PUMP pipeline 算子：

```cpp
// 并发 RPC 调用
when_all(
    rpc::call(ch, METHOD_A, req_a),
    rpc::call(ch, METHOD_B, req_b),
    rpc::call(ch, METHOD_C, req_c)
) >> then([](auto&& results) { ... });

// 流式处理中的 RPC
for_each(items) >> concurrent(N)
    >> then([ch](auto item) {
        return rpc::call(ch, METHOD_PROCESS, ProcessReq{item});
    }) >> flat() >> reduce();

// 扇出：同一请求发给多个 peer
for_each(peers) >> concurrent()
    >> then([req](auto& peer_ch) {
        return rpc::call(peer_ch, METHOD_VOTE, req);
    }) >> flat() >> reduce();
```

---

## 六、性能约束

| 约束 | 具体要求 |
|------|----------|
| **零拷贝 decode** | 从 ring buffer 直接读取消息头和 payload，不 memcpy |
| **高效 encode** | 直接构造 iovec 数组，writev 发送 |
| **无锁** | 所有 RPC 状态（pending map、dispatcher）在 session 所属 core 的单线程 scheduler 上操作 |
| **热路径无堆分配** | pending slot 预分配；消息对象栈上或预分配 |
| **编译期分发** | method dispatch 尽量通过模板或 constexpr if 实现 |
| **批处理** | 一次 recv 处理所有可用消息，不逐条 recv |
| **最小跨核开销** | 跨核 RPC 调用仅通过 lock-free queue 传递指针/ID |
| **cache 友好** | pending request 数组线性存储，避免指针追逐 |

---

## 七、错误处理

### 7.1 RPC 级错误

| 错误 | 触发条件 | 传播方式 |
|------|----------|----------|
| `rpc_timeout` | 请求超时未收到响应 | 通过 pending callback 传播 exception |
| `method_not_found` | 收到未注册 method 的请求 | 自动回复 Error 消息 |
| `remote_error` | 对端 handler 返回/抛出错误 | 作为 Error 消息传回，调用方收到 exception |
| `codec_error` | 编解码失败 | exception 传播到当前 pipeline |
| `connection_lost` | 连接断开 | 所有 pending requests 收到 exception |
| `channel_closed` | Channel 已关闭，尝试调用 | 立即 exception |
| `pending_overflow` | pending requests 数量超过上限 | 拒绝新请求，exception |

### 7.2 错误消息格式

- Error 消息使用独立的 type 标识
- 包含 request_id（匹配原始请求）
- 包含 error_code（数值）和可选 error_message
- 编码方式由 Codec 决定

### 7.3 Handler 异常处理

- Handler 抛出异常时，框架捕获并自动构造 Error Response 发回
- Handler 返回的 sender 产生异常时，同样转为 Error Response
- 框架不吞掉异常，异常信息应完整传回调用方

---

## 八、上层协议扩展分析

### 8.1 各协议需求对照

| 能力 | Raft | RESP | MQ |
|------|------|------|-----|
| Request-Response | ✓ | ✓ | ✓ |
| 双向发起 | ✓ (vote/append) | ✗ (client→server) | ✓ (push) |
| Notification | ✓ (heartbeat) | ✓ (pub/sub push) | ✓ (message push) |
| 连接复用 | 低（通常 1 inflight） | 高（pipelining） | 中 |
| 大消息 | ✓ (InstallSnapshot) | ✗ (通常 <1MB) | ✓ |
| 自定义 wire format | 可用标准格式 | ✓ (RESP 行协议) | 取决于设计 |
| 流式传输 | ✓ (日志复制) | ✗ | ✓ (消费流) |
| Per-session 状态 | ✓ (term/index) | ✓ (auth/selected_db) | ✓ (subscriptions) |

### 8.2 扩展点

| 扩展点 | 机制 | 说明 |
|--------|------|------|
| **Codec** | 模板参数 | 协议实现自己的编解码 |
| **Protocol State** | 模板参数注入到 Channel | 协议附加 per-session 状态 |
| **Handler** | 注册表 | 协议注册方法处理器 |
| **Connection Policy** | 配置 | 心跳间隔、超时、最大并发请求数 |
| **Message Type** | Codec 内扩展 | 协议可以定义额外的消息类型 |

### 8.3 RESP 特殊考量

RESP 有完全不同的 wire format（行协议，`+OK\r\n`、`$5\r\nhello\r\n`）：
- 不使用通用的二进制 RPC 头
- 自己的长度语义和分隔符
- Pipelining：连续发送多条命令，响应按序返回

RPC 层需要的灵活性：**Codec 必须能完全控制消息帧的解析**，RPC 层不硬编码任何帧格式。对于 RESP 这类协议，Codec 自己处理行协议解析，RPC 层只负责提供请求关联和分发框架。

### 8.4 协议实现方式

上层协议通过以下方式使用 RPC 层：

```cpp
// 1. 定义 Codec
struct RaftCodec {
    // 实现 decode_header, decode_payload, encode ...
};

// 2. 定义 Protocol State（可选）
struct RaftSessionState {
    uint64_t current_term;
    uint64_t last_log_index;
    // ...
};

// 3. 创建 Channel
auto ch = rpc::make_channel<RaftCodec, RaftSessionState>(session_id, scheduler);

// 4. 注册 Handler
ch.dispatcher().on(APPEND_ENTRIES, [](AppendEntriesReq&& req) { ... });
ch.dispatcher().on(REQUEST_VOTE, [](RequestVoteReq&& req) { ... });

// 5. 启动服务
rpc::serve(ch) >> submit(ctx);

// 6. 发起调用
rpc::call(ch, APPEND_ENTRIES, AppendEntriesReq{...})
    >> then([](AppendEntriesResp&& resp) { ... });
```

---

## 九、流式传输需求（Phase 2）

以下需求不要求第一版实现，但架构设计时应预留扩展空间。

### 9.1 服务端流（Server Streaming）

一个 Request 触发多个 Response：
- Raft 日志复制：一次请求返回多条日志条目
- MQ 消费：订阅后持续收到消息

```cpp
// 服务端
dispatcher.on_stream(METHOD_SUBSCRIBE, [](SubscribeReq&& req) {
    return for_each(messages) >> then([](auto& msg) { return msg; });
    // 返回 sender 流，每个值自动作为一条 Response 发送
});

// 客户端
rpc::call_stream(ch, METHOD_SUBSCRIBE, req)
    >> for_each([](SubscribeResp&& resp) { /* 处理每条消息 */ });
```

### 9.2 双向流（Bidirectional Streaming）

建立流通道后，双方持续收发消息。需要：stream_id、flow control、背压机制。

---

## 十、待讨论问题

### Q1: 消息帧归属

当前 Net 层 recv 端硬编码 2 字节长度前缀（`has_full_pkt`），send 端不自动加前缀。

- **选项 A**：扩展 Net 层，长度前缀参数化（2/4 字节），RPC 层复用 Net 层的帧
- **选项 B**：RPC 层完全接管帧，Net 层退化为纯字节流（recv 直接返回 buffer，RPC 层自己解析帧）
- **选项 C**：保持现状，RPC 层在 64KB payload 内工作，大消息在 RPC 层分片重组

哪种方案更符合你的预期？

### Q2: RPC 头格式

- **选项 A（标准头）**：RPC 层定义固定二进制头 `[type:1B][req_id:4B][method_id:2B]`，框架直接解析，Codec 只负责 payload
  - 优点：框架可以通用处理关联/分发，Codec 更简单
  - 缺点：RESP 等自定义 wire format 的协议无法使用

- **选项 B（Codec 全权）**：RPC 头部也由 Codec 定义和解析，框架通过 Codec trait 获取 type/req_id/method_id
  - 优点：最大灵活性，任何协议都能适配
  - 缺点：Codec 实现更复杂，框架难以做通用优化

倾向哪种？

### Q3: 流式传输优先级

流式传输（Server Streaming / Bidirectional Streaming）是否在第一版就需要？还是先完成 Request-Response + Notification，后续迭代加入？

### Q4: 跨核 RPC 调用

当 core 0 上的代码需要通过 core 2 上的 session 发送 RPC 请求：
- 发送通过 session_scheduler 的 lock-free send queue 跨核投递（已有机制）
- 响应到达 core 2（session 所在 core），需要将结果传回 core 0

如何高效完成最后一步？选项：
- **A**：响应 callback 直接在 core 2 上执行（调用方 pipeline 在 core 2 继续）
- **B**：通过 task_scheduler 将结果投递回 core 0（额外一次跨核，但调用方在原 core 继续）

### Q5: 背压

当 pending requests 过多时：
- **A**：hard limit + 拒绝新请求（简单，符合无锁设计）
- **B**：流控协商（复杂，可能需要协议配合）

### Q6: 请求取消

是否需要支持取消已发送但未收到响应的请求？
- 本地取消（从 pending map 移除，后续响应丢弃）？
- 远程取消（通知对端停止处理）？

### Q7: 连接池

客户端是否需要内置连接池（多个连接到同一目标，自动负载均衡）？还是由应用层自行管理？
