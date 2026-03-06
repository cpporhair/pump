# UDP 层需求文档

> PUMP 框架 UDP 传输层实现需求。与 TCP 层平级，位于 `src/env/scheduler/udp/`。

## 1. 设计原则

| 原则 | 说明 |
|------|------|
| 真正无连接 | 不模拟 session/connection，不引入 accept/join/stop |
| 数据报语义 | 每次 recv 返回完整数据报 + 来源端点，每次 send 指定目标端点 |
| 与 TCP 层对等 | 独立模块，不依赖 TCP 层代码，共享框架基础设施（per_core queue、op_pusher 等） |
| 后端可替换 | 先实现 io_uring 后端，预留 epoll 和未来 DPDK 后端的扩展位 |

## 2. 与 TCP 层的核心差异

| 维度 | TCP | UDP |
|------|-----|-----|
| 连接模型 | 有连接（accept → session → join/stop） | 无连接 |
| 标识符 | `session_id_t`（标识一条连接） | `endpoint`（标识一个对端地址） |
| recv 语义 | `recv(sche, session_id)` → `net_frame` | `recv(sche)` → `(datagram, endpoint)` |
| send 语义 | `send(sche, session_id, data, len)` | `send(sche, endpoint, data, len)` |
| 帧边界 | 流式，需要 length-prefix framing | 天然消息边界，无需 framing |
| 内部缓冲 | `packet_buffer` 环形缓冲区 + 帧拆分 | 不需要，每个 recvmsg 就是一个完整数据报 |
| scheduler 种类 | accept_scheduler + session_scheduler | 单一 scheduler |

## 3. 公共类型

### 3.1 endpoint

对端地址标识，封装 `sockaddr_in`（先支持 IPv4，结构上预留 IPv6）。

```cpp
namespace pump::scheduler::udp::common {
    struct endpoint {
        sockaddr_in addr;   // IPv4

        // 构造
        endpoint() = default;
        endpoint(const char* ip, uint16_t port);
        endpoint(sockaddr_in addr);

        // 访问
        uint16_t port() const;
        uint32_t ip() const;       // network byte order
        std::string ip_str() const;

        bool operator==(const endpoint&) const = default;
    };
}
```

### 3.2 datagram

接收到的数据报。类似 TCP 的 `net_frame`，但语义是完整数据报而非流分帧。

```cpp
struct datagram {
    char* _data;
    uint32_t _len;

    // move-only，RAII 释放，接口与 net_frame 一致
    // release()、data()、size()、as<T>()
};
```

**设计决策**：`datagram` 可以直接复用 `net_frame` 的实现（或提取公共基类），也可以独立定义。实现时再根据实际情况决定。

## 4. 公共 API

```cpp
namespace pump::scheduler::udp {
    // 接收数据报 — 返回 (datagram, endpoint)
    recv(scheduler* sche)

    // 发送数据报 — 返回 bool (成功/失败)
    send(scheduler* sche, endpoint ep, void* data, uint32_t len)
}
```

### 4.1 recv

- 输出类型：`(datagram, endpoint)` — 两个值传递给下游
- 无需指定 session/endpoint，从 socket 收任意来源的数据报
- scheduler 内部通过 `recvmsg` 获取数据和源地址

### 4.2 send

- 输入：目标 endpoint + 数据
- 输出：`bool`（发送是否成功提交）
- scheduler 内部通过 `sendmsg` 发送到指定地址

## 5. Scheduler

### 5.1 单一 scheduler 类型

TCP 需要 accept_scheduler（监听）+ session_scheduler（连接 I/O），UDP 只需一个 scheduler：

```cpp
namespace pump::scheduler::udp::io_uring {
    struct scheduler {
        // 初始化：绑定地址和端口
        int init(const char* address, uint16_t port, unsigned queue_depth = 256);

        // 请求队列
        core::per_core::queue<recv_req*> recv_q;
        core::per_core::queue<send_req*> send_q;

        void schedule(recv_req*);
        void schedule(send_req*);

        // 主循环
        bool advance();
        template<typename runtime_t> bool advance(const runtime_t&);
    };
}
```

### 5.2 请求类型

```cpp
struct recv_req {
    std::move_only_function<void(std::variant<
        std::pair<datagram, endpoint>,  // 成功：数据 + 来源
        std::exception_ptr              // 失败
    >)> cb;
};

struct send_req {
    endpoint target;
    datagram frame;     // 数据所有权转移给 scheduler
    std::move_only_function<void(bool)> cb;
};
```

### 5.3 init 行为

- 创建 `SOCK_DGRAM` socket（非阻塞）
- 绑定到指定 address:port
- 设置 `SO_REUSEADDR | SO_REUSEPORT`
- 初始化 io_uring ring
- 提交首个 recvmsg SQE（开始接收）

### 5.4 advance 行为

```
advance():
    1. drain recv_q — 将新的 recv_req 加入等待列表
    2. drain send_q — 为每个 send_req 提交 sendmsg SQE
    3. handle_io — peek CQE：
       - recvmsg 完成 → 匹配 pending recv_req → cb(datagram, endpoint)
                        若无 pending → 缓存到 ready_q
                        重新提交 recvmsg SQE
       - sendmsg 完成 → cb(成功/失败)
```

## 6. Sender/Op 层

遵循 PUMP 自建 scheduler 六组件模式（见 CLAUDE.md）。

### 6.1 recv sender

```
组件清单：
- op:  constexpr static bool udp_recv_op = true
       start<pos>() 构造 recv_req，cb 中 push_value(datagram, endpoint)
- sender: make_op() + connect<ctx_t>()
- op_pusher 特化: requires udp_recv_op → 调用 op.start<pos>()
- compute_sender_type 特化: count_value() = 2, types = (datagram, endpoint)
```

### 6.2 send sender

```
组件清单：
- op:  constexpr static bool udp_send_op = true
       start<pos>() 构造 send_req，cb 中 push_value(bool)
- sender: make_op() + connect<ctx_t>()
- op_pusher 特化: requires udp_send_op → 调用 op.start<pos>()
- compute_sender_type 特化: count_value() = 1, type = bool
```

## 7. 目录结构

```
src/env/scheduler/udp/
├── udp.hh                    ← 公共 API（udp::recv, udp::send）
├── common/
│   └── struct.hh             ← endpoint, datagram, recv_req, send_req
├── senders/
│   ├── recv.hh               ← recv sender/op/op_pusher/compute_sender_type
│   └── send.hh               ← send sender/op/op_pusher/compute_sender_type
├── io_uring/
│   └── scheduler.hh          ← io_uring 后端
└── udp.cmake                 ← 构建配置
```

## 8. io_uring 后端实现要点

### 8.1 recvmsg

```cpp
// 预分配 recv buffer（每个 pending recvmsg 一个）
// msghdr + iovec + sockaddr_in 作为 recvmsg 参数
struct pending_recv {
    msghdr msg;
    iovec iov;
    sockaddr_in src_addr;
    char buf[MAX_DATAGRAM_SIZE];  // 或动态分配
};

// 提交：io_uring_prep_recvmsg(sqe, fd, &pending.msg, 0)
// 完成：从 pending.src_addr 构造 endpoint，从 pending.buf 构造 datagram
```

### 8.2 sendmsg

```cpp
// 从 send_req 构造 msghdr
// io_uring_prep_sendmsg(sqe, fd, &msg, 0)
// 完成：cb(res >= 0)
```

### 8.3 最大数据报大小

- UDP 理论最大 65535 字节（含 IP 头）
- 实际常用 MTU 内（~1472 字节避免分片）
- 建议提供配置项 `max_datagram_size`，默认 65536

### 8.4 与 TCP io_uring 后端的差异

| 维度 | TCP io_uring | UDP io_uring |
|------|-------------|-------------|
| socket 类型 | `SOCK_STREAM` | `SOCK_DGRAM` |
| 读操作 | `prep_readv` 到 ring buffer | `prep_recvmsg` 到独立 buffer |
| 写操作 | `prep_writev`（session fd） | `prep_sendmsg`（目标地址在 msghdr） |
| 帧处理 | ring buffer + length-prefix 拆帧 | 无需拆帧，每个 CQE = 一个完整数据报 |
| 连接管理 | accept + session 状态机 | 无 |
| fd 数量 | 每个 session 一个 fd | 整个 scheduler 一个 fd |

## 9. 示例用法

### 9.1 UDP Echo Server

```cpp
// 绑定端口
auto* sche = new udp::io_uring::scheduler();
sche->init("0.0.0.0", 9000);

// echo 循环
just()
    >> forever()
    >> flat_map([sche](...) {
        return udp::recv(sche);
    })
    >> flat_map([sche](datagram&& data, endpoint ep) {
        return udp::send(sche, ep, data.release(), data.size());
    })
    >> reduce()
    >> submit(ctx);
```

### 9.2 UDP Client

```cpp
auto* sche = new udp::io_uring::scheduler();
sche->init("0.0.0.0", 0);  // 绑定随机端口

endpoint server{"192.168.1.1", 9000};

// 发送请求
just()
    >> flat_map([sche, server]() {
        auto* buf = new char[4]{1, 2, 3, 4};
        return udp::send(sche, server, buf, 4);
    })
    >> flat_map([sche](...) {
        return udp::recv(sche);
    })
    >> then([](datagram&& data, endpoint from) {
        // 处理响应
    })
    >> submit(ctx);
```

## 10. 不在本次范围

| 项目 | 说明 |
|------|------|
| epoll 后端 | 后续按需添加，结构上预留 |
| DPDK 后端 | 远期目标 |
| RPC over UDP | 先完成 UDP 层，再考虑 RPC 抽象 |
| IPv6 | endpoint 用 `sockaddr_in`，后续按需扩展 `sockaddr_storage` |
| 多播/广播 | 不在初始范围，但 scheduler 的 socket 设置可以后续扩展 |
| 可靠性/重传 | UDP 层不做，由应用层按需实现 |
| 分片/重组 | 不做，建议应用层控制数据报大小在 MTU 以内 |
