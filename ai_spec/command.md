# PUMP RPC 层需求文档

> **文档目的**：定义 RPC 层的需求，作为后续实现的指导依据  
> **文档范围**：仅关注需求定义（做什么），设计方案和实现细节见 `plan.md`（怎么做）  
> **前置依赖**：net 模块已实现底层网络收发（conn/connect/recv/send/join/stop）

---

## 一、分层架构定位

```
┌─────────────────────────────────────────────┐
│         应用协议层 (Application Protocol)     │
│   raft / resp / mq / 自定义协议 ...          │
│   （各模块独立开发，互不感知）                  │
├─────────────────────────────────────────────┤
│              RPC 层 (本文档)                  │
│   消息帧 / 编解码 / 请求-响应关联 / 服务分发   │
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
- 提供基于模板特化的服务注册与分发机制

**RPC 层不负责**：
- 具体应用协议的语义（如 raft 选举逻辑、resp 命令解析）
- 连接建立与生命周期管理（由 net 层负责）
- 底层字节收发（由 net 层负责）
- 连接池、连接选择策略、业务模块路由（由上层应用负责）
- 连接级收发调度（recv 分发、send 合并）（由上层应用负责）
- 服务发现、负载均衡等更上层关注点

**核心定位**：RPC 层只关心「用哪个 session 完成一次 RPC 调用」。session 是专用连接还是从连接池中选出的复用连接，由上层决定并传入。

### 1.2 设计原则

| 原则 | 说明 |
|------|------|
| **无锁** | 与 PUMP 框架一致，RPC 层所有组件无锁设计 |
| **单线程 scheduler** | RPC 层的状态管理限制在单个 scheduler 线程内 |
| **编译期多态** | 优先使用模板和 concepts 实现协议扩展 |
| **Sender 原生** | 所有异步操作以 sender 形式暴露，可接入 PUMP 管道组合 |
| **最小分配** | 减少运行时动态内存分配，优先使用预分配和对象池 |

---

## 二、核心抽象

### 2.1 消息帧（Frame）

RPC 层需要将 net 层的连续字节流切分为独立的消息帧：

- **F-FRAME-1**：支持从 `packet_buffer` 中识别和提取完整的消息帧
- **F-FRAME-2**：支持处理不完整帧（数据不足时等待更多数据到达）
- **F-FRAME-3**：支持处理单次 recv 中包含多个完整帧的情况（粘包）
- **F-FRAME-4**：分帧策略必须由上层协议自定义，RPC 层提供抽象接口
- **F-FRAME-5**：完整帧通过 copy-out 复制为独立的 `recv_frame`，上层通过 struct overlay 零开销访问帧数据

### 2.2 编解码（Codec）

在帧之上，RPC 层需要提供编解码抽象：

- **F-CODEC-1**：提供编码抽象——将结构化消息序列化为可发送的字节（iovec 数组）
- **F-CODEC-2**：提供解码抽象——将完整帧的字节反序列化为结构化消息
- **F-CODEC-3**：编解码过程中的错误应产生可传播的异常
- **F-CODEC-4**：编码结果应直接产出 `iovec` 数组，与 net 层 `send` 接口对齐
- **F-CODEC-5**：解码应支持 struct overlay 模式——通过 `reinterpret_cast` 直接引用 `recv_frame` 中的数据

### 2.3 请求-响应关联（Request-Response Correlation）

异步 RPC 需要将收到的响应与之前发出的请求正确匹配：

- **F-CORR-1**：每个请求必须携带唯一标识（request_id），响应必须携带对应的 request_id
- **F-CORR-2**：发送请求后，RPC 层应能异步等待对应的响应到达
- **F-CORR-3**：支持请求超时（本版暂不实现，见 plan.md D-2）
- **F-CORR-4**：支持取消等待中的请求（本版暂不实现）
- **F-CORR-5**：request_id 的生成和管理应在单个 session 范围内，不需要全局唯一
- **F-CORR-6**：连接断开时，所有未完成的等待请求应收到异常通知

RPC 层需要支持的通信模式：
- **请求-响应（Request-Response）**：发送一个请求等待一个响应
- **单向通知（One-way）**：只发送不期待响应
- **服务端推送（Server Push）**：服务端主动发送消息

### 2.4 协议扩展点（Protocol Trait）

上层协议通过实现一组协议特征接入 RPC 框架：

- **F-PROTO-1**：上层协议通过实现协议特征来定义帧格式、编解码规则和消息类型
- **F-PROTO-2**：协议特征应在编译期完全确定，不引入运行时多态开销
- **F-PROTO-3**：RPC 层的 sender API 应以协议特征为模板参数进行参数化
- **F-PROTO-4**：不同的连接/session 可以使用不同的协议特征

协议特征需要定义的内容：

| 特征项 | 说明 |
|--------|------|
| **分帧器** | 如何从字节流中切分帧 |
| **消息类型** | 请求/响应的 C++ 类型（扁平化 variant） |
| **编码器** | 消息 → 字节 |
| **解码器** | 字节 → 消息 |
| **请求ID提取** | 如何从消息中提取 request_id |
| **消息分类** | 判断一个帧是请求、响应还是推送 |
| **模块标识** | 该协议对应的 module_id |

### 2.5 服务注册与分发

RPC 层提供基于模板特化的服务注册和编译期分发机制：

- **F-SVC-1**：RPC 层提供 `service_type` 枚举和 `service<sid>` 基模板，应用层通过模板特化注册服务
- **F-SVC-2**：每个服务通过 `handle()` 静态方法的重载处理不同请求类型，handler 统一返回 sender
- **F-SVC-3**：RPC 层通过 concept 检测服务是否支持特定请求类型（`has_handle_concept`）
- **F-SVC-4**：RPC 层提供编译期 `dispatch` 机制，根据 service_id 将请求路由到对应服务的 handler
- **F-SVC-5**：新增服务只需添加 `service_type` 枚举值和对应的模板特化，无需修改 RPC 层代码

### 2.6 上层职责（非 RPC 层）

以下关注点由上层应用负责，RPC 层不涉及：

- **连接策略**：哪些业务独占连接、哪些共享连接池
- **连接池**：管理一组连接、选择策略、动态扩缩容
- **模块路由**：共享连接上按 `module_id` 将消息分发到不同业务模块
- **收发调度**：recv 分发、send 合并

---

## 三、Sender API 需求

### 3.1 核心 Sender

#### 3.1.1 `rpc::call` — 异步 RPC 调用

- **语义**：发送一个请求并异步等待对应的响应
- **输入**：session_id + 请求消息
- **输出**：响应消息
- **异常**：连接断开、编解码错误
- **需求编号**：F-API-1

#### 3.1.2 `rpc::send` — 单向发送

- **语义**：发送一条消息，不等待响应
- **输入**：session_id + 消息
- **输出**：发送完成确认
- **需求编号**：F-API-2

#### 3.1.3 `rpc::recv` — 接收下一条消息

- **语义**：从连接中接收并解码下一条完整消息
- **输入**：session_id
- **输出**：解码后的消息对象（扁平化 variant）
- **说明**：内部处理分帧和解码
- **需求编号**：F-API-3

#### 3.1.4 `rpc::serve` — 服务端请求处理循环

- **语义**：持续接收请求消息，根据 service_type 分发到对应 handler，将处理结果作为响应发回
- **输入**：session_id + 服务类型列表（模板参数）
- **输出**：流式处理（每个请求-响应对作为一个元素）
- **说明**：handler 只需返回响应 sender，不需感知连接 ID，响应原路返回
- **需求编号**：F-API-4

### 3.2 Sender 组合性需求

- **F-API-5**：所有 RPC sender 必须同时提供显式 scheduler 版本和 flat_map 版本
- **F-API-6**：RPC sender 必须可以与现有 PUMP sender 自由组合
- **F-API-7**：RPC sender 的类型推导必须满足 `compute_sender_type` 规范
- **F-API-8**：RPC sender 必须支持 `op_pusher` 的 position-based pushing 机制

### 3.3 使用示例

```
// 客户端：发送请求并获取响应
scheduler::net::connect(sched, addr, port)
    >> then([sched](session_id_t sid) {
        return rpc::call<MyProtocol>(sched, sid, request_msg)
            >> visit()
            >> then([](auto&& resp) { /* 处理响应 */ });
    })

// 服务端：接受连接并启动服务分发
scheduler::net::wait_connection(accept_sched)
    >> concurrent(1024)
    >> flat_map([](session_id_t sid) {
        return rpc::serve<service_type>(sid);
    })
    >> all()

// 单向发送
rpc::send<MyProtocol>(sched, sid, notification_msg)

// 接收消息并按类型分发
rpc::recv<MyProtocol>(sched, sid)
    >> visit()
    >> then([](auto&& msg) {
        if constexpr (std::is_same_v<__typ__(msg), request_a>) {
            return handle_a(msg);
        } else {
            return handle_b(msg);
        }
    })
```

---

## 四、会话管理需求

- **F-SESSION-1**：每个 session 维护独立的 RPC 状态（待响应请求映射表、request_id 生成器）
- **F-SESSION-2**：session 的 RPC 状态必须在 session 关闭时自动清理
- **F-SESSION-3**：session 的 RPC 状态只能在其所属的 scheduler 线程中访问
- **F-SESSION-4**：首次对某个 session 调用 RPC API 时自动创建 RPC 状态（lazy bind），无需显式绑定

---

## 五、错误处理需求

### 5.1 错误分类

| 错误类别 | 示例 | 处理方式 |
|----------|------|----------|
| **传输错误** | 连接断开、发送失败 | 来自 net 层，直接传播为异常 |
| **帧错误** | 帧格式不合法、帧过大 | 由分帧器检测，产生异常 |
| **编解码错误** | 反序列化失败、类型不匹配 | 由编解码器检测，产生异常 |
| **协议错误** | 未知消息类型、版本不兼容 | 由协议特征检测，产生异常 |

### 5.2 功能需求

- **F-ERR-1**：所有错误必须通过 PUMP 的异常传播机制（`push_exception`）向下游传递
- **F-ERR-2**：错误类型应足够具体，使上层可以通过 `catch_exception<T>` 区分不同种类的错误
- **F-ERR-3**：帧错误不应导致连接关闭（除非协议特征明确要求），应允许跳过错误帧继续处理
- **F-ERR-4**：连接级错误应通知所有在该 session 上等待的 pending 请求

---

## 六、性能需求

- **P-LAT-1**：RPC 层引入的额外延迟（不含网络传输和业务处理）应在微秒级
- **P-LAT-2**：请求-响应关联的查找复杂度应为 O(1)
- **P-THR-1**：单 session 应支持大量 in-flight 请求的并发处理
- **P-THR-2**：消息编解码路径上不应有动态内存分配（热路径）
- **P-MEM-1**：pending 请求的存储应使用预分配或对象池
- **P-MEM-2**：编码产出的 iovec 数组应尽量复用或栈分配

---

## 七、协议扩展支持需求

### 7.1 扩展性功能需求

- **F-EXT-1**：协议特征的切换不应影响 RPC 层核心代码的编译
- **F-EXT-2**：新增一个协议只需实现协议特征，无需修改 RPC 层任何已有文件
- **F-EXT-3**：不同协议之间不应有编译依赖

### 7.2 协议支持考量

RPC 层的抽象应能支持以下典型协议模式：

| 协议 | 关键特点 |
|------|---------|
| **Raft** | 请求-响应模式，消息类型少但频率高，要求极低延迟 |
| **RESP** | 文本/二进制混合，`\r\n` 分隔，支持 pipeline |
| **MQ** | 服务端主动推送，单向确认，消息体可能很大 |

---

## 八、与现有架构的集成需求

- **F-INT-1**：RPC 层建立在 net 层之上，通过组合 net sender 实现功能
- **F-INT-2**：RPC 层使用 net 层的 `session_id_t` 标识连接
- **F-INT-3**：RPC 层使用 net 层的 `recv_frame` 作为数据载体
- **F-INT-4**：RPC 层不引入额外的逻辑 scheduler，复用 net 层的 session_scheduler
- **F-INT-5**：RPC sender 必须实现 `op` / `sender` / `op_pusher` / `compute_sender_type` 四件套
- **F-INT-6**：RPC sender 的 op 必须支持 `Flat OpTuple` 平铺
- **F-INT-7**：RPC sender 必须支持通过 `context` 传递跨步骤状态

---

## 九、非功能性需求

- **NF-ORG-1**：RPC 层代码应放置在独立的命名空间（如 `pump::scheduler::rpc`）
- **NF-ORG-2**：RPC 层的文件应独立于 net 层目录（如 `src/env/scheduler/rpc/`）
- **NF-ORG-3**：协议特征的实现应与 RPC 核心分离
- **NF-TEST-1**：RPC 层的分帧器、编解码器应可独立测试
- **NF-TEST-2**：请求-响应关联机制应可独立测试
- **NF-DOC-1**：提供协议特征的 concept 文档
- **NF-DOC-2**：提供至少一个完整的协议实现示例

---

## 十、需求优先级

### P0 — 核心（首期必须实现）

| 编号 | 需求 | 说明 |
|------|------|------|
| F-FRAME-1~5 | 消息分帧 | RPC 最基础的能力 |
| F-CODEC-1~4 | 编解码 | 消息结构化的基础 |
| F-PROTO-1~4 | 协议特征 | 可扩展性的基石 |
| F-SVC-1~5 | 服务注册与分发 | 服务端核心机制 |
| F-API-1~4 | call/send/recv/serve | 核心 API |
| F-API-5~8 | Sender 组合性 | 与 PUMP 框架集成 |
| F-ERR-1~2 | 异常传播 | 基本错误处理 |
| F-INT-1~7 | 架构集成 | 与 Net 层和 PUMP Sender 框架兼容 |

### P1 — 重要（核心完成后立即实现）

| 编号 | 需求 | 说明 |
|------|------|------|
| F-CORR-1~2 | 请求-响应关联 | 多路复用能力 |
| F-CORR-5~6 | request_id 管理 | 关联机制的基础 |
| F-SESSION-1~4 | 会话级状态 | 有状态 RPC |
| F-ERR-3~4 | 高级错误处理 | 健壮性 |

### P2 — 增强（按需实现）

| 编号 | 需求 | 说明 |
|------|------|------|
| F-CORR-3~4 | 超时与取消 | 本版暂不实现 |
| F-CODEC-5 | 零拷贝解码 | 性能优化 |
| P-* | 性能优化需求 | 需有基准测试支撑 |
