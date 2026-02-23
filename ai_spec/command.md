# PUMP Net 客户端功能需求文档

## 一、背景与目标

### 1.1 现状

当前 `src/env/scheduler/net/` 下的代码仅支持**服务端**功能，核心架构如下：

| 组件 | 职责 | 关键操作 |
|------|------|----------|
| `accept_scheduler` | 监听端口，接受连接 | `bind` → `listen` → `accept` → 产生 `session_id_t` |
| `session_scheduler` | 管理已建立连接的 IO | `join` / `recv` / `send` / `stop` |

服务端使用模式（echo.cc）：
```
wait_connection()                          // accept_scheduler 接受连接
  >> join(session_sched, sid)              // session_scheduler 注册会话
  >> recv(session_sched, sid)              // 异步接收数据
  >> send(session_sched, sid, vec, cnt)    // 异步发送数据
  >> stop(session_sched, sid)              // 关闭会话
```

### 1.2 目标

加入**客户端**功能，使 PUMP 框架同时支持服务端和客户端网络编程。客户端功能需要：

1. **与服务端对等的使用体验**：声明式异步管道组合，如 `connect() >> send()/recv() >> then()`
2. **极致的效率**：零拷贝、无锁、单线程 scheduler 设计，与服务端保持一致
3. **对异步操作的抽象**：复用 Sender/Op 语义，客户端操作同样是可组合的 sender
4. **双后端支持**：同时支持 epoll 和 io_uring 后端

---

## 二、架构设计

### 2.1 核心思路：connect_scheduler 替代 accept_scheduler

客户端与服务端的唯一本质差异在于**连接建立方式**：

| | 服务端 | 客户端 |
|---|---|---|
| 连接建立 | `accept_scheduler`：被动等待连接 | `connect_scheduler`：主动发起连接 |
| 会话 IO | `session_scheduler`：join/recv/send/stop | **完全复用** `session_scheduler` |

因此，客户端架构 = **新增 `connect_scheduler`** + **完全复用现有 `session_scheduler`**。

### 2.2 组件拓扑

```
┌─────────────────────────────────────────────────────────────────────┐
│                    客户端 runtime_schedulers                         │
├──────────────────────────┬──────────────────────────────────────────┤
│        Core 0            │       Core 1 ... Core N-1               │
│  ┌──────────────────┐    │  ┌────────────────┐                     │
│  │ task_scheduler    │    │  │ task_scheduler  │                    │
│  └──────────────────┘    │  └────────────────┘                     │
│  ┌──────────────────┐    │                                         │
│  │connect_scheduler  │    │       nullptr                          │
│  │ (主动连接)        │    │                                         │
│  │ io_uring/epoll    │    │                                         │
│  └──────────────────┘    │                                         │
│  ┌──────────────────┐    │  ┌────────────────┐                     │
│  │session_scheduler  │    │  │session_scheduler│                    │
│  │ io_uring/epoll    │    │  │ io_uring/epoll  │                    │
│  └──────────────────┘    │  └────────────────┘                     │
├──────────────────────────┴──────────────────────────────────────────┤
│  connect_scheduler 仅在一个核心上运行（类似 accept_scheduler）       │
│  session_scheduler 在所有核心上运行，按 sid % N 分配                 │
└─────────────────────────────────────────────────────────────────────┘
```

### 2.3 与服务端架构的对称性

| 服务端 | 客户端 | 说明 |
|--------|--------|------|
| `accept_scheduler<conn_op_t>` | `connect_scheduler<conn_op_t>` | 模板参数一致，复用 `conn_op_t` |
| `wait_connection(accept_sched)` | `connect(connect_sched, addr, port)` | 返回类型相同：`session_id_t` |
| `session_scheduler<join_op, recv_op, send_op, stop_op>` | **完全复用** | 无需任何修改 |
| `net::join / recv / send / stop` | **完全复用** | 无需任何修改 |

---

## 三、新增组件详细设计

### 3.1 `connect_scheduler`（epoll 后端）

**文件位置**：`src/env/scheduler/net/epoll/scheduler.hh`（在现有文件中新增，或独立文件 `connect_scheduler.hh`）

**类定义**：
```cpp
namespace pump::scheduler::net::epoll {
    template <template<typename> class conn_op_t>
    struct connect_scheduler {
        // 与 accept_scheduler 相同的模板签名，便于泛型代码统一处理
    };
}
```

**核心成员**：

| 成员 | 类型 | 说明 |
|------|------|------|
| `conn_request_q` | `mpsc::queue<common::connect_req*>` | 连接请求队列 |
| `session_q` | `mpmc::queue<session_id_t>` | 已建立连接的会话队列（与 accept_scheduler 对称） |
| `poller` | `detail::poller_epoll` | epoll 实例，监听 connect 完成事件 |
| `_recv_buffer_size` | `size_t` | 接收缓冲区大小 |
| `_shutdown` | `atomic<bool>` | 关闭标志 |

**核心方法**：

| 方法 | 签名 | 说明 |
|------|------|------|
| `init` | `int init(const scheduler_config& cfg)` | 初始化（不需要 bind/listen） |
| `schedule` | `void schedule(common::connect_req* req)` | 接收连接请求，发起非阻塞 connect |
| `handle_connect_completion` | `void handle_connect_completion(epoll_event* e)` | 处理 connect 完成事件 |
| `create_internal_session` | `auto create_internal_session(int fd)` | 连接成功后创建 session（与 accept_scheduler 相同） |
| `advance` | `auto advance()` | 事件循环推进（处理 connect 完成 + 派发请求） |
| `shutdown` | `void shutdown()` | 关闭调度器 |
| `drain_on_shutdown` | `void drain_on_shutdown()` | 清理待处理请求 |

**connect 流程**：
```
schedule(connect_req) 
  → socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK)
  → ::connect(fd, addr, ...) 
  → 如果返回 EINPROGRESS：注册 EPOLLOUT 到 poller，等待可写事件
  → 如果立即成功：直接 create_internal_session(fd)
  → advance() 中 epoll_wait 检测到 EPOLLOUT
  → getsockopt(SO_ERROR) 检查连接是否真正成功
  → 成功：create_internal_session(fd)，回调 session_id_t
  → 失败：回调错误（异常或空 session_id_t）
```

### 3.2 `connect_scheduler`（io_uring 后端）

**文件位置**：`src/env/scheduler/net/io_uring/scheduler.hh`（在现有文件中新增，或独立文件）

**核心差异**（相比 epoll 后端）：
- 使用 `io_uring_prep_connect` 提交异步 connect 请求
- 通过 CQE 获取 connect 完成通知
- 新增 `uring_event_type::CONNECT` 枚举值

**connect 流程**：
```
schedule(connect_req)
  → socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK)
  → io_uring_prep_connect(sqe, fd, addr, addrlen)
  → io_uring_sqe_set_data(sqe, io_uring_request{CONNECT, ...})
  → io_uring_submit()
  → advance() 中 io_uring_peek_cqe / io_uring_wait_cqe 获取 CQE
  → CQE res == 0：连接成功，create_internal_session(fd)
  → CQE res < 0：连接失败，回调错误
```

### 3.3 新增请求结构体 `connect_req`

**文件位置**：`src/env/scheduler/net/common/struct.hh`

```cpp
struct connect_req {
    const char* address;       // 目标地址
    uint16_t port;             // 目标端口
    std::move_only_function<void(session_id_t)> cb;  // 回调，与 conn_req 签名一致
};
```

**设计要点**：
- 回调签名与 `conn_req` 完全一致（都返回 `session_id_t`），确保下游管道可以无差异对待服务端和客户端连接
- 连接失败时回调空 `session_id_t{}`（与 accept_scheduler 的错误处理方式一致）

### 3.4 新增 Sender：`connect` sender

**文件位置**：`src/env/scheduler/net/senders/connect.hh`（新文件）

**结构**（与 `conn.hh` 对称）：

```cpp
namespace pump::scheduler::net::senders::connect {
    template <typename scheduler_t>
    struct op {
        using request_t = common::connect_req;
        constexpr static bool net_sender_connect_op = true;
        scheduler_t* scheduler;
        const char* address;
        uint16_t port;

        template<uint32_t pos, typename context_t, typename scope_t>
        auto start(context_t &context, scope_t &scope) {
            return scheduler->schedule(
                new common::connect_req{
                    address, port,
                    [context = context, scope = scope](common::session_id_t sid) mutable {
                        core::op_pusher<pos + 1, scope_t>::push_value(context, scope, sid);
                    }
                }
            );
        }
    };

    template<typename scheduler_t>
    struct sender {
        scheduler_t* scheduler;
        const char* address;
        uint16_t port;
        // ... make_op(), connect() 方法
    };
}
```

**op_pusher 特化**和 **compute_sender_type 特化**：
- 与 `conn.hh` 中的模式完全一致
- `compute_sender_type::get_value_type_identity()` 返回 `session_id_t`（与 conn 相同）

### 3.5 API 层：`net.hh` 新增

**文件位置**：`src/env/scheduler/net/net.hh`

```cpp
namespace pump::scheduler::net {
    // 显式 scheduler 版本
    template <typename scheduler_t>
    inline auto
    connect(scheduler_t* sche, const char* address, uint16_t port) {
        return senders::connect::sender<scheduler_t>(sche, address, port);
    }

    // flat_map 版本（从上下文获取 scheduler）
    inline auto
    connect(const char* address, uint16_t port) {
        return pump::sender::flat_map([address, port]<typename scheduler_t>(scheduler_t* sche) {
            return connect(sche, address, port);
        });
    }
}
```

---

## 四、使用示例

### 4.1 基本客户端用法

```cpp
// 连接到服务器并发送数据
connect(connect_sched, "127.0.0.1", 8080)
    >> then([session_sched](session_id_t sid) {
        return join(session_sched, sid)
            >> send(session_sched, sid, vec, cnt)
            >> recv(session_sched, sid)
            >> then([](packet_buffer* buf) {
                // 处理响应
            })
            >> stop(session_sched, sid);
    })
    >> submit(core::make_root_context());
```

### 4.2 Echo 客户端示例（与 echo.cc 服务端对应）

```cpp
template <typename connect_scheduler_t, typename session_scheduler_t>
void run_echo_client() {
    using rs_t = runtime_schedulers<connect_scheduler_t, session_scheduler_t>;
    just()
        >> get_context<rs_t*>()
        >> then([](rs_t* rs) {
            auto* connect_sched = rs->template get_schedulers<connect_scheduler_t>()[0];
            return scheduler::net::connect(connect_sched, "127.0.0.1", 8080)
                >> then([rs](scheduler::net::common::session_id_t sid) {
                    client_session_proc(rs, sid)
                        >> submit(core::make_root_context());
                })
                >> submit(core::make_root_context(rs));
        })
        >> get_context<rs_t*>()
        >> then([](rs_t* rs) {
            env::runtime::start(rs->schedulers_by_core);
        })
        >> submit(core::make_root_context(
            create_client_runtime_schedulers<connect_scheduler_t, session_scheduler_t>()
        ));
}
```

### 4.3 连接池模式（多连接）

```cpp
// 建立 N 个连接
just()
    >> forever()    // 或 repeat(N)
    >> flat_map([connect_sched, addr, port](...) {
        return scheduler::net::connect(connect_sched, addr, port);
    })
    >> then([rs](session_id_t sid) {
        // 每个连接独立的会话处理管道
        client_session_proc(rs, sid)
            >> submit(core::make_root_context());
    })
    >> count()
    >> submit(core::make_root_context(rs));
```

### 4.4 客户端与服务端对称性展示

```cpp
// 服务端
wait_connection(accept_sched)           >> then([](session_id_t sid) { ... });

// 客户端
connect(connect_sched, addr, port)      >> then([](session_id_t sid) { ... });

// 连接建立之后，服务端和客户端的会话处理代码完全相同：
join(session_sched, sid)
    >> recv(session_sched, sid)
    >> send(session_sched, sid, vec, cnt)
    >> stop(session_sched, sid)
```

---

## 五、实现计划与优先级

### P0 — 核心基础设施

1. **新增 `connect_req` 结构体**（`common/struct.hh`）
2. **新增 `connect` sender**（`senders/connect.hh`），包含 op、sender、op_pusher 特化、compute_sender_type 特化
3. **更新 `net.hh`**，新增 `connect()` API 函数，新增 `#include "./senders/connect.hh"`

### P1 — 后端实现

4. **实现 epoll `connect_scheduler`**（`epoll/scheduler.hh` 或新文件 `epoll/connect_scheduler.hh`）
   - 非阻塞 connect + EPOLLOUT 监听
   - getsockopt(SO_ERROR) 验证连接结果
   - create_internal_session 复用 accept_scheduler 的 session 创建逻辑
5. **实现 io_uring `connect_scheduler`**（`io_uring/scheduler.hh` 或新文件）
   - io_uring_prep_connect 异步连接
   - 新增 `uring_event_type::CONNECT`
   - CQE 处理连接完成

### P2 — 示例与验证

6. **编写 echo 客户端示例**（`apps/example/echo_client/` 或在 echo.cc 中增加 `--client` 模式）
7. **验证**：echo 客户端连接 echo 服务端，发送数据并验证回显正确

### P3 — 健壮性

8. **连接超时处理**：connect_scheduler 内部可选超时机制
9. **连接失败重试**：可在 sender 管道层面通过 `repeat` + 异常处理实现，无需侵入 scheduler
10. **多目标地址连接**：DNS 解析后尝试多个地址（后续扩展，初期可不实现）

---

## 六、设计约束与注意事项

### 6.1 必须遵循的框架原则

| 原则 | 客户端实现要求 |
|------|---------------|
| **Single-Thread Scheduler** | `connect_scheduler` 只能被单个线程运行，所有 connect 事件在同一线程处理 |
| **Lock-Free** | 使用 `mpsc::queue` / `mpmc::queue` 做请求/结果传递，不引入锁 |
| **Flat OpTuple** | `connect` sender 的 op 必须支持被编译期平铺到 tuple 中 |
| **Position-based Pushing** | `connect` op 的 `start` 方法通过 `op_pusher<pos+1>` 推进 |
| **Scheduler Isolation** | connect_scheduler 与 session_scheduler 保持执行域隔离 |

### 6.2 session_scheduler 零修改

客户端连接建立后产生的 `session_id_t` 与服务端 accept 产生的 `session_id_t` 格式完全一致（ptr + generation 编码），因此 `session_scheduler` 的 `join / recv / send / stop` 操作**无需任何修改**即可服务客户端会话。

### 6.3 connect_req 与 conn_req 的关系

- `conn_req`：accept_scheduler 的接口，语义为"等待一个新连接"
- `connect_req`：connect_scheduler 的接口，语义为"主动建立一个连接到指定地址"
- 两者的回调签名相同（`void(session_id_t)`），确保下游管道处理的统一性
- `connect` sender 可复用 `conn_op_t` 模板参数的模式，也可定义独立的 `connect_op_t`

### 6.4 错误处理

客户端连接可能遇到的错误：

| 错误场景 | 处理方式 |
|----------|----------|
| 连接被拒绝（ECONNREFUSED） | 回调空 `session_id_t{}`，上层通过 `any_exception` 捕获 |
| 连接超时 | 由 connect_scheduler 内部定时器触发超时，回调错误 |
| 网络不可达（ENETUNREACH） | 同连接被拒绝 |
| socket 创建失败 | 同连接被拒绝 |

### 6.5 与 `scheduler_config` 的关系

现有 `scheduler_config` 中的 `address` 和 `port` 字段在服务端表示监听地址。对于客户端，这两个字段语义变为目标连接地址。可以考虑：
- 方案 A：复用 `scheduler_config`，语义由使用方决定（推荐，最小改动）
- 方案 B：新增 `client_config` 结构体（如果未来客户端需要额外配置项）

初期推荐方案 A。

---

## 七、文件变更清单

| 操作 | 文件路径 | 变更内容 |
|------|----------|----------|
| 修改 | `common/struct.hh` | 新增 `connect_req` 结构体 |
| 新增 | `senders/connect.hh` | `connect` 的 op、sender、op_pusher、compute_sender_type |
| 修改 | `net.hh` | 新增 `connect()` API，新增 `#include` |
| 修改或新增 | `epoll/scheduler.hh` 或 `epoll/connect_scheduler.hh` | epoll 后端 `connect_scheduler` |
| 修改或新增 | `io_uring/scheduler.hh` 或 `io_uring/connect_scheduler.hh` | io_uring 后端 `connect_scheduler` |
| 新增 | `apps/example/echo_client/` | echo 客户端示例 |
| 修改 | `apps/CMakeLists.txt` | 新增 echo_client 构建目标 |
