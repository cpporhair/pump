# PUMP RPC 层需求文档

> **文档目的**：细化 RPC 层的完整需求，作为后续实现的指导依据  
> **文档范围**：仅关注需求定义，不涉及具体实现方案  
> **前置依赖**：net 模块已实现底层网络收发（conn/connect/recv/send/join/stop）

---

## 一、分层架构定位

```
┌─────────────────────────────────────────────┐
│         应用协议层 (Application Protocol)     │
│   raft / resp / mq / 自定义协议 ...          │
├─────────────────────────────────────────────┤
│              RPC 层 (本文档)                  │
│   消息帧 / 编解码 / 请求-响应关联 / 流控      │
├─────────────────────────────────────────────┤
│         Net 层 (已实现)                       │
│   conn / connect / recv / send / join / stop │
└─────────────────────────────────────────────┘
```

### 1.1 RPC 层的职责边界

**RPC 层负责**：
- 将 net 层的原始字节流转化为结构化的消息帧
- 提供异步请求-响应（request-response）语义
- 提供请求与响应的关联匹配机制
- 提供面向上层协议的可扩展编解码抽象
- 提供连接级别的多路复用（multiplexing），使单连接可并发处理多个独立请求

**RPC 层不负责**：
- 具体应用协议的语义（如 raft 选举逻辑、resp 命令解析）
- 连接建立与生命周期管理（由 net 层负责）
- 底层字节收发（由 net 层负责）
- 服务发现、负载均衡等更上层关注点

### 1.2 设计原则

| 原则 | 说明 |
|------|------|
| **零拷贝优先** | 消息帧的解析和构造应尽可能直接操作 `packet_buffer`，避免不必要的内存拷贝 |
| **无锁** | 与 PUMP 框架一致，RPC 层所有组件必须无锁设计 |
| **单线程 scheduler** | RPC 层的状态管理必须限制在单个 scheduler 线程内，不跨线程共享可变状态 |
| **编译期多态** | 优先使用模板和 concepts 实现协议扩展，避免虚函数开销 |
| **Sender 原生** | 所有异步操作以 sender 形式暴露，可无缝接入 PUMP 管道组合 |
| **最小分配** | 减少运行时动态内存分配，优先使用预分配和对象池 |

---

## 二、核心抽象

### 2.1 消息帧（Frame）

#### 2.1.1 需求描述

RPC 层需要将 net 层的连续字节流切分为独立的消息帧。不同上层协议有不同的分帧策略，因此分帧机制必须可扩展。

#### 2.1.2 功能需求

- **F-FRAME-1**：支持从 `packet_buffer` 中识别和提取完整的消息帧
- **F-FRAME-2**：支持处理不完整帧（数据不足时等待更多数据到达）
- **F-FRAME-3**：支持处理单次 recv 中包含多个完整帧的情况（粘包）
- **F-FRAME-4**：分帧策略必须由上层协议自定义，RPC 层提供抽象接口
- **F-FRAME-5**：帧的解析过程应直接操作 `packet_buffer`，不进行额外拷贝

#### 2.1.3 分帧策略抽象

上层协议需要提供的分帧能力：
- 给定当前缓冲区内容，判断是否包含完整帧
- 如果包含完整帧，返回帧的边界（起始偏移 + 长度）
- 如果不完整，返回"需要更多数据"的信号

常见分帧模式（RPC 层应能支持但不限于）：
- **固定长度前缀**：如 2/4 字节表示后续 payload 长度（echo 协议当前使用的模式）
- **分隔符**：如 `\r\n` 分隔（resp 协议）
- **固定长度**：每帧固定字节数
- **自描述**：帧头中包含类型和长度信息（典型 RPC 二进制协议）

### 2.2 编解码（Codec）

#### 2.2.1 需求描述

在帧之上，RPC 层需要提供将原始帧字节与结构化消息对象之间互相转换的编解码抽象。编解码必须由上层协议定义，RPC 层只提供扩展点。

#### 2.2.2 功能需求

- **F-CODEC-1**：提供编码抽象——将结构化消息序列化为可发送的字节（iovec 数组）
- **F-CODEC-2**：提供解码抽象——将完整帧的字节反序列化为结构化消息
- **F-CODEC-3**：编解码过程中的错误（格式错误、版本不匹配等）应产生可传播的异常
- **F-CODEC-4**：编码结果应直接产出 `iovec` 数组，与 net 层 `send` 接口对齐
- **F-CODEC-5**：解码应支持零拷贝模式——解码后的消息可以引用 `packet_buffer` 中的原始数据（适用于只读访问场景）

### 2.3 请求-响应关联（Request-Response Correlation）

#### 2.3.1 需求描述

异步 RPC 的核心挑战是将收到的响应与之前发出的请求正确匹配。在单连接多路复用场景下，响应可能乱序到达。

#### 2.3.2 功能需求

- **F-CORR-1**：每个请求必须携带唯一标识（request_id），响应必须携带对应的 request_id
- **F-CORR-2**：发送请求后，RPC 层应能异步等待对应的响应到达
- **F-CORR-3**：支持请求超时——如果在指定时间内未收到响应，应产生超时异常
- **F-CORR-4**：支持取消等待中的请求
- **F-CORR-5**：request_id 的生成和管理应在单个 session 范围内，不需要全局唯一
- **F-CORR-6**：连接断开时，所有未完成的等待请求应收到异常通知

#### 2.3.3 关联模式

RPC 层需要支持的通信模式：
- **请求-响应（Request-Response）**：最基础的模式，发送一个请求等待一个响应
- **单向通知（One-way / Fire-and-forget）**：只发送不期待响应
- **服务端推送（Server Push）**：服务端主动发送消息，无需客户端请求触发

注意：流式 RPC（如双向 streaming）是否需要支持取决于上层协议需要，RPC 层应为其预留扩展空间，但不要求首期实现。

### 2.4 协议扩展点（Protocol Trait）

#### 2.4.1 需求描述

由于上层协议多样（raft、resp、mq 等），RPC 层必须提供统一的协议扩展机制，允许上层协议通过实现一组"协议特征"（Protocol Trait）来接入 RPC 框架。

#### 2.4.2 功能需求

- **F-PROTO-1**：上层协议通过实现协议特征来定义自己的帧格式、编解码规则和消息类型
- **F-PROTO-2**：协议特征应在编译期完全确定，不引入运行时多态开销
- **F-PROTO-3**：RPC 层的 sender API 应以协议特征为模板参数进行参数化
- **F-PROTO-4**：不同的连接/session 可以使用不同的协议特征（例如同一个进程既提供 raft RPC 又提供 resp 服务）

#### 2.4.3 协议特征需要定义的内容

一个完整的协议特征应包含：

| 特征项 | 说明 |
|--------|------|
| **分帧器（Framer）** | 如何从字节流中切分帧（对应 2.1 节） |
| **请求消息类型** | 请求消息的 C++ 类型 |
| **响应消息类型** | 响应消息的 C++ 类型 |
| **编码器（Encoder）** | 消息 → 字节（对应 2.2 节） |
| **解码器（Decoder）** | 字节 → 消息（对应 2.2 节） |
| **请求ID提取** | 如何从消息中提取 request_id（对应 2.3 节） |
| **消息分类** | 判断一个帧是请求、响应还是推送通知 |

---

## 三、Sender API 需求

### 3.1 核心 Sender

RPC 层应暴露以下 sender 供管道组合使用：

#### 3.1.1 `rpc::call` — 异步 RPC 调用

- **语义**：发送一个请求并异步等待对应的响应
- **输入**：session_id + 请求消息
- **输出**：响应消息
- **异常**：超时、连接断开、编解码错误
- **需求编号**：F-API-1

#### 3.1.2 `rpc::send` — 单向发送

- **语义**：发送一条消息，不等待响应
- **输入**：session_id + 消息
- **输出**：发送完成确认（bool 或 void）
- **需求编号**：F-API-2

#### 3.1.3 `rpc::recv` — 接收下一条消息

- **语义**：从连接中接收并解码下一条完整消息
- **输入**：session_id
- **输出**：解码后的消息对象
- **说明**：这是底层 recv 之上的高级版本，内部处理分帧和解码
- **需求编号**：F-API-3

#### 3.1.4 `rpc::serve` — 请求处理循环

- **语义**：持续接收请求消息，为每个请求调用用户提供的处理函数，将处理结果作为响应发回
- **输入**：session_id + 请求处理函数
- **输出**：流式（每个请求-响应对作为流中的一个元素）
- **说明**：对标 echo 示例中的 `forever >> recv >> process >> send` 模式，但封装了分帧、编解码和 request_id 关联
- **需求编号**：F-API-4

### 3.2 Sender 组合性需求

- **F-API-5**：所有 RPC sender 必须同时提供显式 scheduler 版本和 flat_map 版本（与 net 层 API 风格一致）
- **F-API-6**：RPC sender 必须可以与现有的 PUMP sender（then/flat_map/any_exception/concurrent 等）自由组合
- **F-API-7**：RPC sender 的类型推导必须满足 `compute_sender_type` 规范，确保编译期类型正确传播
- **F-API-8**：RPC sender 必须支持 `op_pusher` 的 position-based pushing 机制

### 3.3 使用示例（伪代码，仅表达期望的用户体验）

```
// 客户端：发送请求并获取响应
scheduler::net::connect(sched, addr, port)
    >> rpc::call<MyProtocol>(request_msg)
    >> then([](MyProtocol::response_type& resp) {
        // 处理响应
    })

// 服务端：处理请求循环
scheduler::net::wait_connection(sched)
    >> flat_map([](session_id_t sid) {
        return rpc::serve<MyProtocol>(sid, [](MyProtocol::request_type& req) {
            // 处理请求，返回响应
            return make_response(req);
        });
    })

// 单向推送
rpc::send<MyProtocol>(sid, notification_msg)

// 并发处理多个请求
rpc::serve<MyProtocol>(sid, handler)
    >> concurrent(max_inflight)
```

---

## 四、连接与会话管理需求

### 4.1 会话级 RPC 状态

- **F-SESSION-1**：每个 session 需要维护独立的 RPC 状态，包括：
  - 待响应请求的映射表（request_id → 等待回调）
  - request_id 生成器
  - 分帧器的部分帧缓存状态
- **F-SESSION-2**：session 的 RPC 状态必须在 session 关闭时自动清理
- **F-SESSION-3**：session 的 RPC 状态只能在其所属的 scheduler 线程中访问（单线程不变量）

### 4.2 会话初始化

- **F-SESSION-4**：RPC 层需要提供一种机制将协议特征绑定到 session，可以在 join 之后、首次收发之前完成
- **F-SESSION-5**：绑定后，该 session 的所有收发操作自动使用绑定的协议特征进行编解码

---

## 五、错误处理需求

### 5.1 错误分类

RPC 层需要定义清晰的错误类型层次：

| 错误类别 | 示例 | 处理方式 |
|----------|------|----------|
| **传输错误** | 连接断开、发送失败 | 来自 net 层，直接传播为异常 |
| **帧错误** | 帧格式不合法、帧过大 | 由分帧器检测，产生异常 |
| **编解码错误** | 反序列化失败、类型不匹配 | 由编解码器检测，产生异常 |
| **协议错误** | 未知消息类型、版本不兼容 | 由协议特征检测，产生异常 |
| **超时错误** | 请求等待超时 | 由关联机制检测，产生异常 |

### 5.2 功能需求

- **F-ERR-1**：所有错误必须通过 PUMP 的异常传播机制（`push_exception`）向下游传递
- **F-ERR-2**：错误类型应足够具体，使上层可以通过 `catch_exception<T>` 区分不同种类的错误
- **F-ERR-3**：帧错误不应导致连接关闭（除非协议特征明确要求），应允许跳过错误帧继续处理
- **F-ERR-4**：连接级错误应通知所有在该 session 上等待的 pending 请求

---

## 六、性能需求

### 6.1 延迟

- **P-LAT-1**：RPC 层引入的额外延迟（不含网络传输和业务处理）应在微秒级
- **P-LAT-2**：请求-响应关联的查找复杂度应为 O(1)
- **P-LAT-3**：分帧操作不应引入额外的数据拷贝

### 6.2 吞吐

- **P-THR-1**：单 session 应支持大量 in-flight 请求的并发处理
- **P-THR-2**：消息编解码路径上不应有动态内存分配（热路径）
- **P-THR-3**：分帧器应支持批量处理——单次 recv 返回多个完整帧时，应批量解码而非逐个调度

### 6.3 内存

- **P-MEM-1**：pending 请求的存储应使用预分配或对象池，避免频繁 new/delete
- **P-MEM-2**：编码产出的 iovec 数组应尽量复用或栈分配
- **P-MEM-3**：分帧器不应为部分帧分配额外缓冲区——应直接利用 `packet_buffer` 的已有缓冲

---

## 七、协议扩展支持需求

### 7.1 对 Raft 协议的支持考量

- 请求-响应模式：投票请求（RequestVote）、日志复制请求（AppendEntries）
- 需要支持：request_id 关联、超时检测
- 消息类型：二进制序列化、固定头部 + 变长 payload
- 特点：消息类型少但频率极高，要求极低延迟

### 7.2 对 RESP 协议的支持考量

- 文本/二进制混合协议，`\r\n` 分隔
- 支持 pipeline：客户端连续发送多个命令不等响应
- 需要支持：按序响应（RESP 不需要 request_id，响应顺序与请求顺序一致）
- 特点：分帧规则复杂（内联命令 vs 多批量命令），编解码有多种数据类型

### 7.3 对 MQ 协议的支持考量

- 需要支持：服务端主动推送（消息投递）
- 需要支持：单向确认（ACK 无响应）
- 需要支持：消费者组等高级语义（由应用层处理，但 RPC 层需提供推送通道）
- 特点：消息体可能很大，零拷贝尤为重要

### 7.4 扩展性功能需求

- **F-EXT-1**：协议特征的切换不应影响 RPC 层核心代码的编译
- **F-EXT-2**：新增一个协议只需实现协议特征，无需修改 RPC 层任何已有文件
- **F-EXT-3**：不同协议之间不应有编译依赖（实现 raft 协议不需要包含 resp 的头文件）
- **F-EXT-4**：RPC 层应提供足够的 hook 点，允许上层协议注入自定义行为（如连接建立后的握手、认证）

---

## 八、与现有架构的集成需求

### 8.1 与 Net 层的集成

- **F-INT-1**：RPC 层建立在 net 层之上，通过组合 net sender 实现功能，不修改 net 层代码
- **F-INT-2**：RPC 层使用 net 层的 `session_id_t` 标识连接
- **F-INT-3**：RPC 层使用 net 层的 `packet_buffer` 作为数据载体

### 8.2 与 Scheduler 的集成

- **F-INT-4**：RPC 层不引入新的 scheduler 类型，复用 net 层的 session_scheduler
- **F-INT-5**：RPC 的状态（pending 请求映射等）应跟随 session 绑定在 session_scheduler 的线程上

### 8.3 与 PUMP Sender 框架的集成

- **F-INT-6**：RPC sender 必须实现 `op` / `sender` / `op_pusher` / `compute_sender_type` 四件套
- **F-INT-7**：RPC sender 的 op 必须支持 `Flat OpTuple` 平铺
- **F-INT-8**：RPC sender 必须支持通过 `context` 传递跨步骤状态

---

## 九、非功能性需求

### 9.1 代码组织

- **NF-ORG-1**：RPC 层代码应放置在独立的命名空间（如 `pump::scheduler::rpc` 或 `pump::rpc`）
- **NF-ORG-2**：RPC 层的文件应独立于 net 层目录，建议放置在平行目录（如 `src/env/scheduler/rpc/`）
- **NF-ORG-3**：协议特征的实现应与 RPC 核心分离，放在各自的协议目录下

### 9.2 可测试性

- **NF-TEST-1**：RPC 层的分帧器、编解码器应可独立测试，不依赖网络连接
- **NF-TEST-2**：请求-响应关联机制应可独立测试
- **NF-TEST-3**：应提供 mock 协议特征用于 RPC 层自身的单元测试

### 9.3 文档

- **NF-DOC-1**：提供协议特征的 concept 文档，明确上层协议需要实现哪些接口
- **NF-DOC-2**：提供至少一个完整的协议实现示例作为参考

---

## 十、需求优先级

### P0 — 核心（首期必须实现）

| 编号 | 需求 | 说明 |
|------|------|------|
| F-FRAME-1~5 | 消息分帧 | RPC 最基础的能力 |
| F-CODEC-1~4 | 编解码 | 消息结构化的基础 |
| F-PROTO-1~4 | 协议特征 | 可扩展性的基石 |
| F-API-1~3 | call/send/recv sender | 核心 API |
| F-API-5~8 | Sender 组合性 | 与 PUMP 框架集成 |
| F-ERR-1~2 | 异常传播 | 基本错误处理 |
| F-INT-1~8 | 架构集成 | 与现有模块兼容 |

### P1 — 重要（核心完成后立即实现）

| 编号 | 需求 | 说明 |
|------|------|------|
| F-CORR-1~2 | 请求-响应关联 | 多路复用能力 |
| F-CORR-5~6 | request_id 管理 | 关联机制的基础 |
| F-API-4 | serve sender | 服务端请求处理循环 |
| F-SESSION-1~5 | 会话级状态 | 有状态 RPC |
| F-ERR-3~4 | 高级错误处理 | 健壮性 |

### P2 — 增强（按需实现）

| 编号 | 需求 | 说明 | 决策依据 |
|------|------|------|----------|
| F-CORR-3~4 | 超时与取消 | 高级流控 | D-2: 本版不做超时 |
| F-CODEC-5 | 零拷贝解码 | 性能优化 | |
| F-EXT-4 | Hook 点 | 高级扩展 | D-4: 握手由协议层自行处理 |
| P-* | 性能优化需求 | 需有基准测试支撑 | |
| P-THR-3 | 多帧批量推送 | 本版单次推送 | D-8: 本版不做批量推送 |
| D-3 | 背压/限流 | 本版不实现 | D-3: 由 concurrent(max) 控制 |
| D-5 | 消息大小限制 | 本版不实现 | D-5: 暂不需要 |

---

## 十一、设计决策记录

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
| D-9 | session RPC 状态存储 | RPC 层维护独立的 `session_id → rpc_state` 映射，不修改 net 层 | 符合 F-INT-1 |
| D-10 | 编码内存管理 | 栈/预分配固定缓冲（`std::array<iovec, 8>`），编码器将 iovec 写入 RPC 层预分配的固定大小缓冲区 | P-MEM-2, P-THR-2, F-CODEC-4 |
| D-11 | `rpc::recv` 消息类型 | 返回扁平化的 `std::variant<具体消息类型...>`（如 `variant<vote_request, append_request, vote_response, append_response>`），下游通过 PUMP 的 `visit()` 算子解包后用 `if constexpr` 分支处理。不使用嵌套 variant（不先分 req/res 再分具体命令类型） | F-API-3, 12.4 recv 签名, 12.1 Protocol concept |
| D-12 | handler 返回值 | 每个命令的 handler 函数统一返回 sender（如 `just(result)`），无论内部实现是同步还是异步，保持 pump 管道风格一致性 | 13.1, 13.2 使用示例 |

---

## 十二、P0 需求细化规格

> 本节对首期必须实现的 P0 需求进行进一步细化，作为编码实现的直接依据。

### 12.1 协议特征（Protocol Trait）概念定义

上层协议通过满足以下 concept 接入 RPC 层（伪代码，最终实现以 C++ concepts 为准）：

```cpp
// 分帧结果
struct frame_result {
    enum status { complete, incomplete, error };
    status st;
    size_t frame_offset;  // 帧在 buffer 中的起始偏移
    size_t frame_length;  // 帧的总长度（含帧头）
};

// 消息分类
enum class message_kind {
    request,    // 请求消息
    response,   // 响应消息
    push        // 服务端推送 / 单向通知
};

template <typename P>
concept Protocol = requires {
    // 类型定义
    typename P::message_type;       // 扁平化的消息 variant 类型（D-11）
                                    // 例如: std::variant<vote_request, append_request,
                                    //                    vote_response, append_response>

    // 分帧：给定 packet_buffer，判断是否包含完整帧
    // 返回 frame_result
    { P::try_parse_frame(std::declval<const packet_buffer&>()) }
        -> std::same_as<frame_result>;

    // 解码：将完整帧的字节反序列化为具体消息类型（D-11: 扁平 variant）
    // 参数：packet_buffer 引用 + 帧偏移 + 帧长度
    // 返回：message_type（扁平 variant，直接包含所有具体命令类型）
    // { P::decode(std::declval<const packet_buffer&>(), size_t{}, size_t{}) }
    //     -> std::same_as<typename P::message_type>;

    // 编码：将消息序列化为 iovec 数组（D-10: 写入预分配缓冲）
    // 参数：具体消息引用 + iovec 缓冲区引用
    // 返回：实际使用的 iovec 段数
    // 编码器对每种具体消息类型提供重载（非 variant）

    // 从消息中提取 request_id（D-1: uint32_t）
    // 对 message_type variant 中的每个具体类型都需支持
    { P::get_request_id(std::declval<const typename P::message_type&>()) }
        -> std::same_as<uint32_t>;

    // 判断消息类别
    { P::classify(std::declval<const packet_buffer&>(), size_t{}, size_t{}) }
        -> std::same_as<message_kind>;
};
```

### 12.2 分帧器工作流程

```
net::recv(sid)
  → packet_buffer*
  → Protocol::try_parse_frame(buf)
  → 循环:
      if complete → 提取帧 → 解码 → 推送给下游
                   → forward_head(frame_length)
                   → 继续 try_parse_frame（处理粘包）
      if incomplete → 再次 net::recv 等待更多数据
      if error → push_exception（帧错误）
```

- 分帧循环必须处理单次 recv 返回多个完整帧的情况（F-FRAME-3）
- 不完整帧的数据保留在 `packet_buffer` 中，下次 recv 后继续解析（F-FRAME-2）
- 整个过程零拷贝，直接操作 `packet_buffer` 的 head/tail 指针（F-FRAME-5）
- D-8: 每解出一个完整帧，逐个推送给下游（每帧一次 push），本版不做批量推送

### 12.3 编解码与 Net 层对齐

- 编码输出必须是 `iovec*` + `size_t cnt`，与 `net::send(sid, vec, cnt)` 签名直接匹配（F-CODEC-4）
- 编码器负责在 `iovec` 数组中包含帧头（长度前缀/分隔符等），确保接收端的分帧器能识别
- 解码器输入为 `packet_buffer` + 帧偏移 + 帧长度，可直接通过 `handle_data()` 零拷贝访问原始数据
- **D-10: 编码内存管理**：编码器将 iovec 写入 RPC 层预分配的栈缓冲区，而非动态分配
  - RPC 层在 send 路径上持有一个 `std::array<iovec, 8>` 固定缓冲
  - 编码器签名接收缓冲区引用，将 iovec 段写入其中，返回实际使用的段数
  - 若单条消息的 iovec 段数超过 8，编码器应 throw 异常（协议设计应保证不超限）
  - 缓冲区生命周期与 send op 绑定，send 完成后自动释放，无需手动管理
  - 对比现有 echo 中 `new iovec[2]` + 手动 `delete[]` 的模式，彻底消除热路径动态分配

### 12.4 Sender API 签名细化

```cpp
namespace pump::scheduler::rpc {

    // --- rpc::call ---
    // 显式 scheduler 版本
    template <Protocol P, typename scheduler_t>
    auto call(scheduler_t* sche, session_id_t sid,
              typename P::request_type req);
    // 返回 sender，完成值类型为 P::response_type

    // flat_map 版本（从上游接收 scheduler）
    template <Protocol P>
    auto call(session_id_t sid, typename P::request_type req);

    // --- rpc::send ---
    template <Protocol P, typename scheduler_t>
    auto send(scheduler_t* sche, session_id_t sid, auto&& msg);

    template <Protocol P>
    auto send(session_id_t sid, auto&& msg);

    // --- rpc::recv ---
    // 接收并解码下一条完整消息（D-11: 返回扁平化 variant）
    template <Protocol P, typename scheduler_t>
    auto recv(scheduler_t* sche, session_id_t sid);
    // 返回 sender，完成值类型为 P::message_type（扁平 variant）
    // 例如 RaftProtocol 的 message_type = variant<vote_request, append_request,
    //                                            vote_response, append_response>
    // 下游通过 visit() 算子解包，直接获得具体命令类型：
    //   rpc::recv<RaftProto>(sid) >> visit() >> then([](auto&& msg) {
    //       if constexpr (std::is_same_v<__typ__(msg), vote_request>) { ... }
    //       else if constexpr (std::is_same_v<__typ__(msg), append_request>) { ... }
    //       ...
    //   });

    template <Protocol P>
    auto recv(session_id_t sid);

    // --- rpc::serve ---
    // 请求处理循环
    template <Protocol P, typename scheduler_t, typename Handler>
    auto serve(scheduler_t* sche, session_id_t sid, Handler&& handler);
    // Handler 签名: auto handler(auto&& msg) -> sender（D-12: handler 统一返回 sender）
    // handler 接收具体消息类型（经 visit() 解包后），返回 sender

    template <Protocol P, typename Handler>
    auto serve(session_id_t sid, Handler&& handler);
}
```

### 12.5 会话 RPC 状态结构

每个绑定了协议特征的 session 需要维护以下状态（D-9: 存储在 RPC 层独立映射中，不修改 net 层）：

```cpp
template <Protocol P>
struct session_rpc_state {
    // request_id 生成器（D-1: uint32_t 自增）
    uint32_t next_request_id = 0;

    // 待响应请求映射表：request_id → 回调/continuation
    // 使用预分配的 flat_map 或数组，O(1) 查找
    // D-7: 默认容量 256，可在 session 初始化时配置
    pending_requests_map<uint32_t, completion_callback> pending{256};

    // 分帧器状态：无需额外缓存，直接利用 packet_buffer
    // （packet_buffer 的 head/tail 已天然支持部分帧保留）
};

// D-9: RPC 层独立维护 session_id → rpc_state 映射
// 每个 scheduler 线程持有自己的映射（单线程不变量，无需加锁）
template <Protocol P>
struct rpc_state_registry {
    // session_id → session_rpc_state 的映射
    // 使用 flat_map 或 unordered_map，按 session_id 查找
    std::unordered_map<session_id_t, session_rpc_state<P>> states;

    // session 绑定协议时创建状态
    auto& bind(session_id_t sid) {
        return states.emplace(sid, session_rpc_state<P>{}).first->second;
    }

    // session 关闭时清理状态（F-SESSION-2）
    void unbind(session_id_t sid) {
        states.erase(sid);
    }
};
```

### 12.6 使用示例（结合 echo 改造）

以下展示将现有 echo 示例改造为使用 RPC 层的效果：

```cpp
// 定义 Echo 协议特征
struct EchoProtocol {
    using request_type = echo_request;
    using response_type = echo_response;

    static frame_result try_parse_frame(const packet_buffer& buf) {
        // 4 字节长度前缀 + payload
        if (buf.used() < 4) return { .st = incomplete };
        uint32_t len = read_u32(buf.data() + buf.head());
        if (buf.used() < 4 + len) return { .st = incomplete };
        return { .st = complete, .frame_offset = buf.head(), .frame_length = 4 + len };
    }
    // ... encode / decode / get_request_id / classify
};

// 服务端：用 rpc::serve 替代手动 recv >> process >> send
// D-12: handler 统一返回 sender
scheduler::net::join(session_sched, sid)
    >> rpc::serve<EchoProtocol>(sid, [](echo_request& req) {
        return just(echo_response{ req.data });  // 返回 sender
    })
    >> any_exception([](std::exception_ptr e) { /* 错误处理 */ });

// D-11: 扁平化 recv + visit()，直接解出具体命令类型
rpc::recv<RaftProto>(sid)
    >> visit()
    >> then([](auto&& msg) {
        // visit() 解包后，msg 直接是具体类型（如 vote_request、append_request 等）
        if constexpr (std::is_same_v<__typ__(msg), vote_request>) {
            return handle_vote(msg);  // handler 返回 sender（D-12）
        } else if constexpr (std::is_same_v<__typ__(msg), append_request>) {
            return handle_append(msg);
        } else if constexpr (std::is_same_v<__typ__(msg), vote_response>) {
            return handle_vote_resp(msg);
        } else {
            return handle_append_resp(msg);
        }
    });
```

---

## 十三、应用协议层使用示例

> 本节展示上层协议如何基于 RPC 层构建，覆盖不同通信模式。所有示例均为伪代码，展示期望的用户体验和 sender 管道风格。

### 13.1 Raft 协议

Raft 协议是典型的请求-响应模式，消息类型少但频率极高，要求极低延迟（参见 7.1 节）。

#### 13.1.1 消息定义

```cpp
// --- Raft 消息类型 ---

struct raft_header {
    uint32_t total_len;     // 帧总长度（含 header）
    uint32_t request_id;    // D-1: session 内自增
    uint8_t  msg_type;      // 0x01=RequestVote, 0x02=VoteResponse,
                            // 0x03=AppendEntries, 0x04=AppendResponse
    uint8_t  reserved[3];
};

// --- RequestVote ---
struct vote_request {
    uint32_t request_id;
    uint64_t term;
    uint32_t candidate_id;
    uint64_t last_log_index;
    uint64_t last_log_term;
};

struct vote_response {
    uint32_t request_id;
    uint64_t term;
    bool     vote_granted;
};

// --- AppendEntries ---
struct append_request {
    uint32_t request_id;
    uint64_t term;
    uint32_t leader_id;
    uint64_t prev_log_index;
    uint64_t prev_log_term;
    uint64_t leader_commit;
    uint32_t entry_count;
    // entries 紧跟在固定字段之后（变长 payload）
    // 零拷贝：entries 数据直接引用 packet_buffer
    const char* entries_data;
    size_t entries_len;
};

struct append_response {
    uint32_t request_id;
    uint64_t term;
    bool     success;
    uint64_t match_index;   // follower 已匹配的最高日志索引
};

// D-11: 扁平化的消息类型，所有命令类型在同一层 variant 中
using raft_message = std::variant<
    vote_request, append_request,       // 请求类
    vote_response, append_response      // 响应类
>;
```

#### 13.1.2 协议特征定义

```cpp
struct RaftProtocol {
    // D-11: 扁平化 message_type，所有命令类型在同一层
    using message_type = raft_message;

    // 分帧：固定 header（12 字节）包含 total_len
    static frame_result try_parse_frame(const packet_buffer& buf) {
        constexpr size_t HEADER_SIZE = sizeof(raft_header);
        if (buf.used() < HEADER_SIZE)
            return { .st = incomplete };

        uint32_t total_len = buf.handle_data(sizeof(uint32_t),
            [](const char* d, size_t) { return read_u32(d); });

        if (buf.used() < total_len)
            return { .st = incomplete };

        return { .st = complete, .frame_offset = buf.head(), .frame_length = total_len };
    }

    // 解码：根据 msg_type 直接返回具体消息类型（D-11: 扁平 variant）
    static message_type
    decode(const packet_buffer& buf, size_t offset, size_t length) {
        auto msg_type = buf.handle_data(offset + offsetof(raft_header, msg_type), 1,
            [](const char* d, size_t) { return static_cast<uint8_t>(*d); });

        switch (msg_type) {
            case 0x01: return decode_vote_request(buf, offset, length);
            case 0x02: return decode_vote_response(buf, offset, length);
            case 0x03: return decode_append_request(buf, offset, length);
            case 0x04: return decode_append_response(buf, offset, length);
            default:   throw rpc::protocol_error("unknown raft msg_type");
        }
    }

    // 编码：对每种具体消息类型提供重载（D-10）
    static size_t encode(const vote_request& msg, std::array<iovec, 8>& buf) {
        return do_encode(msg, buf);
    }
    static size_t encode(const vote_response& msg, std::array<iovec, 8>& buf) {
        return do_encode(msg, buf);
    }
    static size_t encode(const append_request& msg, std::array<iovec, 8>& buf) {
        return do_encode(msg, buf);
    }
    static size_t encode(const append_response& msg, std::array<iovec, 8>& buf) {
        return do_encode(msg, buf);
    }

    // request_id 提取（D-1）—— 对 message_type variant 操作
    static uint32_t get_request_id(const message_type& msg) {
        return std::visit([](const auto& m) { return m.request_id; }, msg);
    }

    // 消息分类
    static message_kind classify(const packet_buffer& buf, size_t offset, size_t length) {
        auto msg_type = /* 读取 msg_type 字段 */;
        return (msg_type == 0x01 || msg_type == 0x03)
            ? message_kind::request
            : message_kind::response;
    }
};
```

#### 13.1.3 Candidate 发起投票

```cpp
// Candidate 节点向所有 peer 发起 RequestVote
// peers: 已建立连接的 session_id 列表

auto request_votes(session_scheduler_t* sched,
                   const std::vector<session_id_t>& peers,
                   uint64_t term, uint32_t candidate_id,
                   uint64_t last_log_index, uint64_t last_log_term)
{
    vote_request req {
        .request_id = 0,  // RPC 层自动分配
        .term = term,
        .candidate_id = candidate_id,
        .last_log_index = last_log_index,
        .last_log_term = last_log_term
    };

    // 并行向所有 peer 发送 RequestVote，收集响应
    return generate(peers)
        >> flat_map([sched, req](session_id_t peer_sid) {
            return rpc::call<RaftProtocol>(sched, peer_sid, req)
                >> visit()   // D-11: 解包扁平 variant，直接得到 vote_response
                >> then([](auto&& resp) {
                    // D-12: handler 返回 sender
                    if constexpr (std::is_same_v<__typ__(resp), vote_response>) {
                        return just(std::optional<vote_response>{resp});
                    } else {
                        return just(std::optional<vote_response>{std::nullopt});
                    }
                });
        })
        >> concurrent(peers.size())  // 并行等待所有投票响应
        >> reduce(0u, [](uint32_t granted, std::optional<vote_response>& resp) {
            return granted + (resp && resp->vote_granted ? 1 : 0);
        })
        >> then([majority = peers.size() / 2 + 1](uint32_t granted) {
            return granted >= majority;  // true = 赢得选举
        });
}
```

#### 13.1.4 Follower 处理请求

```cpp
// Follower 节点：接收 Leader/Candidate 的请求并响应
// D-11: recv + visit() 直接解出具体命令类型，无需分层解包

auto follower_loop(session_scheduler_t* sched, session_id_t sid,
                   raft_state& state)
{
    return scheduler::net::join(sched, sid)
        >> forever()
        >> flat_map([sched, sid, &state](...) {
            return rpc::recv<RaftProtocol>(sched, sid)
                >> visit()    // D-11: 解包扁平 variant，直接得到具体命令类型
                >> then([sched, sid, &state](auto&& msg) {
                    // D-12: 每个 handler 统一返回 sender
                    if constexpr (std::is_same_v<__typ__(msg), vote_request>) {
                        return handle_vote_request(sched, sid, state, msg);
                    } else if constexpr (std::is_same_v<__typ__(msg), append_request>) {
                        return handle_append_request(sched, sid, state, msg);
                    } else {
                        // Follower 不应收到响应，忽略
                        return just();
                    }
                });
        })
        >> any_exception([sid](std::exception_ptr e) {
            std::println("Raft session {} error", sid.raw());
            return just();
        });
}

// D-12: handler 统一返回 sender
auto handle_vote_request(session_scheduler_t* sched, session_id_t sid,
                         raft_state& state, const vote_request& req)
{
    bool granted = (req.term >= state.current_term
                    && (state.voted_for == 0 || state.voted_for == req.candidate_id)
                    && req.last_log_index >= state.last_log_index());
    if (granted) {
        state.current_term = req.term;
        state.voted_for = req.candidate_id;
    }
    vote_response resp {
        .request_id = req.request_id,
        .term = state.current_term,
        .vote_granted = granted
    };
    return rpc::send<RaftProtocol>(sched, sid, resp);
}

auto handle_append_request(session_scheduler_t* sched, session_id_t sid,
                           raft_state& state, const append_request& req)
{
    if (req.term < state.current_term) {
        append_response resp {
            .request_id = req.request_id,
            .term = state.current_term,
            .success = false,
            .match_index = 0
        };
        return rpc::send<RaftProtocol>(sched, sid, resp);
    }
    state.current_term = req.term;
    // ... 日志匹配、追加、提交 ...
    append_response resp {
        .request_id = req.request_id,
        .term = state.current_term,
        .success = true,
        .match_index = state.last_log_index()
    };
    return rpc::send<RaftProtocol>(sched, sid, resp);
}
```

#### 13.1.5 Leader 日志复制

```cpp
// Leader 向单个 Follower 发送 AppendEntries
auto replicate_to(session_scheduler_t* sched, session_id_t peer_sid,
                  raft_state& state, uint32_t peer_idx)
{
    return forever()
        >> flat_map([sched, peer_sid, &state, peer_idx](...) {
            auto& next_idx = state.next_index[peer_idx];
            append_request req {
                .request_id = 0,  // RPC 层自动分配
                .term = state.current_term,
                .leader_id = state.self_id,
                .prev_log_index = next_idx - 1,
                .prev_log_term = state.log_term(next_idx - 1),
                .leader_commit = state.commit_index,
                .entry_count = state.log_count_from(next_idx),
                .entries_data = state.log_data_from(next_idx),
                .entries_len = state.log_size_from(next_idx)
            };
            return rpc::call<RaftProtocol>(sched, peer_sid, req)
                >> visit()   // D-11: 直接解出具体响应类型
                >> then([&state, peer_idx](auto&& resp) {
                    // D-12: handler 返回 sender
                    if constexpr (std::is_same_v<__typ__(resp), append_response>) {
                        if (resp.success) {
                            state.next_index[peer_idx] = resp.match_index + 1;
                            state.match_index[peer_idx] = resp.match_index;
                            state.try_advance_commit();
                        } else {
                            state.next_index[peer_idx]--;
                        }
                        return just();
                    } else {
                        return just();  // 忽略非 append_response
                    }
                });
        });
}

// Leader 并行复制到所有 Follower
auto leader_replication(session_scheduler_t* sched,
                        const std::vector<session_id_t>& peer_sids,
                        raft_state& state)
{
    return generate(peer_sids)
        >> flat_map([sched, &state, idx = 0u](session_id_t peer_sid) mutable {
            return replicate_to(sched, peer_sid, state, idx++);
        })
        >> concurrent(peer_sids.size());  // 并行复制
}
```

#### 13.1.6 完整 Raft 节点启动

```cpp
// 一个 Raft 节点同时是 Candidate/Follower/Leader
// 连接建立后根据角色切换行为

auto raft_node(const runtime_schedulers_t* rs, raft_state& state)
{
    auto* accept_sched = rs->template get_schedulers<accept_scheduler_t>()[0];

    return scheduler::net::wait_connection(accept_sched)
        >> flat_map([rs, &state](session_id_t sid) {
            auto* session_sched = pick_session_scheduler(rs, sid);

            // 每个连入的 peer 启动独立的 follower 处理循环
            follower_loop(session_sched, sid, state)
                >> submit(core::make_root_context());

            return just();
        })
        >> concurrent(16);  // 最多 16 个并发 peer 连接
}
```

### 13.2 MQ（消息队列）协议

MQ 协议涵盖发布-订阅、服务端推送、单向确认等模式（参见 7.3 节）。

#### 13.2.1 消息定义

```cpp
// --- MQ 消息类型 ---

struct mq_header {
    uint32_t total_len;
    uint32_t request_id;    // PUBLISH/SUBSCRIBE 使用；ACK 为 0（单向）
    uint8_t  msg_type;      // 0x01=PUBLISH, 0x02=PUBLISH_ACK,
                            // 0x03=SUBSCRIBE, 0x04=DELIVER (server push),
                            // 0x05=ACK (单向确认)
    uint8_t  reserved[3];
};

// 客户端 → Broker
struct publish_request {
    uint32_t request_id;
    std::string_view topic;     // 零拷贝引用 packet_buffer
    std::string_view payload;   // 零拷贝引用 packet_buffer
};

struct publish_ack {
    uint32_t request_id;
    uint64_t message_id;        // Broker 分配的全局消息 ID
    bool     accepted;
};

struct subscribe_request {
    uint32_t request_id;
    std::string_view topic;
    std::string_view consumer_group;
};

// Broker → 消费者（服务端推送）
struct deliver_message {
    uint64_t message_id;
    std::string_view topic;
    std::string_view payload;
};

// 消费者 → Broker（单向确认，无响应）
struct ack_message {
    uint64_t message_id;
};

// D-11: 扁平化的消息类型，所有命令类型在同一层 variant 中
using mq_message = std::variant<
    publish_request, subscribe_request, ack_message,  // 请求/单向类
    publish_ack, deliver_message                      // 响应/推送类
>;
```

#### 13.2.2 协议特征定义

```cpp
struct MqProtocol {
    // D-11: 扁平化 message_type
    using message_type = mq_message;

    static frame_result try_parse_frame(const packet_buffer& buf) {
        constexpr size_t HEADER_SIZE = sizeof(mq_header);
        if (buf.used() < HEADER_SIZE)
            return { .st = incomplete };

        uint32_t total_len = buf.handle_data(sizeof(uint32_t),
            [](const char* d, size_t) { return read_u32(d); });

        if (buf.used() < total_len)
            return { .st = incomplete };

        return { .st = complete, .frame_offset = buf.head(), .frame_length = total_len };
    }

    // 解码：直接返回具体消息类型（D-11: 扁平 variant）
    static message_type
    decode(const packet_buffer& buf, size_t offset, size_t length) {
        auto msg_type = /* 读取 msg_type 字段 */;
        switch (msg_type) {
            case 0x01: return decode_publish(buf, offset, length);
            case 0x02: return decode_publish_ack(buf, offset, length);
            case 0x03: return decode_subscribe(buf, offset, length);
            case 0x04: return decode_deliver(buf, offset, length);
            case 0x05: return decode_ack(buf, offset, length);
            default:   throw rpc::protocol_error("unknown mq msg_type");
        }
    }

    // 编码：对每种具体消息类型提供重载（D-10）
    static size_t encode(const publish_request& msg, std::array<iovec, 8>& buf) {
        return do_encode(msg, buf);
    }
    static size_t encode(const publish_ack& msg, std::array<iovec, 8>& buf) {
        return do_encode(msg, buf);
    }
    static size_t encode(const subscribe_request& msg, std::array<iovec, 8>& buf) {
        return do_encode(msg, buf);
    }
    static size_t encode(const deliver_message& msg, std::array<iovec, 8>& buf) {
        return do_encode(msg, buf);
    }
    static size_t encode(const ack_message& msg, std::array<iovec, 8>& buf) {
        return do_encode(msg, buf);
    }

    // request_id 提取 —— 对 message_type variant 操作
    static uint32_t get_request_id(const message_type& msg) {
        return std::visit([](const auto& m) -> uint32_t {
            if constexpr (requires { m.request_id; }) return m.request_id;
            else return 0;  // ack_message/deliver_message 无 request_id
        }, msg);
    }

    static message_kind classify(const packet_buffer& buf, size_t offset, size_t length) {
        auto msg_type = /* 读取 msg_type 字段 */;
        switch (msg_type) {
            case 0x01: case 0x03: return message_kind::request;
            case 0x02:            return message_kind::response;
            case 0x04:            return message_kind::push;      // 服务端推送
            case 0x05:            return message_kind::request;   // 单向，归为 request
            default:              return message_kind::request;
        }
    }
};
```

#### 13.2.3 生产者：发布消息

```cpp
// 生产者向 Broker 发布消息并等待确认
auto publish(session_scheduler_t* sched, session_id_t broker_sid,
             std::string_view topic, std::string_view payload)
{
    publish_request req {
        .request_id = 0,  // RPC 层自动分配
        .topic = topic,
        .payload = payload
    };

    return rpc::call<MqProtocol>(sched, broker_sid, req)
        >> visit()   // D-11: 直接解出具体消息类型
        >> then([](auto&& resp) {
            // D-12: handler 返回 sender
            if constexpr (std::is_same_v<__typ__(resp), publish_ack>) {
                if (!resp.accepted)
                    throw rpc::application_error("publish rejected");
                return just(resp.message_id);
            } else {
                throw rpc::protocol_error("unexpected response to publish");
                return just(uint64_t{0});  // 不会执行，仅为类型推导
            }
        });
}

// 批量发布：连续发送多条消息，利用 concurrent 并行等待
auto batch_publish(session_scheduler_t* sched, session_id_t broker_sid,
                   const std::vector<std::pair<std::string, std::string>>& messages)
{
    return generate(messages)
        >> flat_map([sched, broker_sid](const auto& msg) {
            return publish(sched, broker_sid, msg.first, msg.second);
        })
        >> concurrent(64)   // 最多 64 条 in-flight 消息
        >> count();          // 统计成功发布数量
}
```

#### 13.2.4 消费者：订阅与接收推送

```cpp
// 消费者订阅 topic 后，持续接收 Broker 推送的消息
auto consume(session_scheduler_t* sched, session_id_t broker_sid,
             std::string_view topic, std::string_view group,
             auto&& message_handler)
{
    // 第一步：发送 SUBSCRIBE 请求
    subscribe_request sub_req {
        .request_id = 0,
        .topic = topic,
        .consumer_group = group
    };

    return rpc::send<MqProtocol>(sched, broker_sid, sub_req)
        // 第二步：持续接收推送消息
        >> forever()
        >> flat_map([sched, broker_sid, handler = __fwd__(message_handler)](...) {
            return rpc::recv<MqProtocol>(sched, broker_sid)
                >> visit()   // D-11: 直接解出具体消息类型
                >> then([sched, broker_sid, &handler](auto&& msg) {
                    // D-12: handler 返回 sender
                    if constexpr (std::is_same_v<__typ__(msg), deliver_message>) {
                        handler(msg);
                        // 发送 ACK（单向，不等响应）
                        return rpc::send<MqProtocol>(sched, broker_sid,
                            ack_message{ msg.message_id });
                    } else {
                        return just();  // 忽略非推送消息
                    }
                });
        })
        >> any_exception([](std::exception_ptr e) {
            std::println("Consumer disconnected");
            return just();
        });
}
```

#### 13.2.5 Broker：处理多种客户端请求

```cpp
// Broker 端：使用 recv + visit() 处理所有客户端请求
// D-11: visit() 直接解出具体命令类型，无需分层解包
auto broker_session(session_scheduler_t* sched, session_id_t client_sid,
                    broker_state& state)
{
    return scheduler::net::join(sched, client_sid)
        >> forever()
        >> flat_map([sched, client_sid, &state](...) {
            return rpc::recv<MqProtocol>(sched, client_sid)
                >> visit()   // D-11: 直接解出具体命令类型
                >> then([sched, client_sid, &state](auto&& msg) {
                    // D-12: 每个 handler 统一返回 sender
                    if constexpr (std::is_same_v<__typ__(msg), publish_request>) {
                        return handle_publish(sched, client_sid, state, msg);
                    } else if constexpr (std::is_same_v<__typ__(msg), subscribe_request>) {
                        return handle_subscribe(sched, client_sid, state, msg);
                    } else if constexpr (std::is_same_v<__typ__(msg), ack_message>) {
                        return handle_ack(sched, client_sid, state, msg);
                    } else {
                        return just();  // 忽略非请求消息
                    }
                });
        })
        >> any_exception([client_sid, &state](std::exception_ptr e) {
            state.remove_subscriber(client_sid);
            return just();
        });
}

// --- D-12: Broker handler 统一返回 sender ---

auto handle_publish(session_scheduler_t* sched, session_id_t client_sid,
                    broker_state& state, const publish_request& req)
{
    auto msg_id = state.store_message(req.topic, req.payload);

    // 响应生产者
    publish_ack ack {
        .request_id = req.request_id,
        .message_id = msg_id,
        .accepted = true
    };
    auto send_ack = rpc::send<MqProtocol>(sched, client_sid, ack);

    // 推送给该 topic 的所有订阅者
    auto& subscribers = state.get_subscribers(req.topic);
    auto fan_out = generate(subscribers)
        >> flat_map([sched, &state, msg_id, &req](session_id_t sub_sid) {
            deliver_message deliver {
                .message_id = msg_id,
                .topic = req.topic,
                .payload = req.payload
            };
            return rpc::send<MqProtocol>(sched, sub_sid, deliver);
        })
        >> concurrent(subscribers.size());

    return when_all(send_ack, fan_out);
}

auto handle_subscribe(session_scheduler_t* sched, session_id_t client_sid,
                      broker_state& state, const subscribe_request& req)
{
    state.add_subscriber(req.topic, req.consumer_group, client_sid);
    return just();  // SUBSCRIBE 无需响应，推送通过 deliver 进行
}

auto handle_ack(session_scheduler_t* sched, session_id_t client_sid,
                broker_state& state, const ack_message& ack)
{
    state.mark_acknowledged(ack.message_id, client_sid);
    return just();  // 单向确认，无需响应（D-6）
}
```

#### 13.2.6 完整 Broker 启动

```cpp
auto run_broker(const runtime_schedulers_t* rs, broker_state& state)
{
    auto* accept_sched = rs->template get_schedulers<accept_scheduler_t>()[0];

    return scheduler::net::wait_connection(accept_sched)
        >> flat_map([rs, &state](session_id_t sid) {
            auto* session_sched = pick_session_scheduler(rs, sid);

            broker_session(session_sched, sid, state)
                >> submit(core::make_root_context());

            return just();
        })
        >> concurrent(1024);  // 支持大量客户端并发连接
}
```

### 13.3 示例对比总结

| 特性 | Echo（12.6） | Raft（13.1） | MQ（13.2） |
|------|------------|------------|-----------|
| 通信模式 | 请求-响应 | 请求-响应 | 请求-响应 + 推送 + 单向 |
| 消息多样性 | 单一类型 | 2 种请求 + 2 种响应 | 3 种请求 + 2 种响应 |
| variant 结构 | 无（D-11） | 扁平 variant（4 种类型） | 扁平 variant（5 种类型） |
| handler 返回 | sender（D-12） | sender（D-12） | sender（D-12） |
| `rpc::serve` | ✓（简单场景） | ✗（需 visit 分发） | ✗（需 visit 分发 + 推送） |
| `rpc::call` | ✗ | ✓（投票、复制） | ✓（发布） |
| `rpc::send` | ✗ | ✓（响应发回） | ✓（ACK 单向、推送） |
| `visit()` 算子 | ✗ | ✓ | ✓ |
| `concurrent` | ✗ | ✓（并行投票/复制） | ✓（fan-out 推送） |
| `when_all` | ✗ | ✗ | ✓（ACK + fan-out） |
| 零拷贝 | ✓ | ✓（entries_data） | ✓（topic/payload string_view） |

---

## 十四、待确认问题

> 已确认的问题已转入第十一节设计决策记录（D-7 ~ D-12）。当前无待确认问题。
