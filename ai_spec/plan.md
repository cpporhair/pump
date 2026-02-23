# PUMP Net 客户端功能实现计划

基于 `command.md` 需求文档，本计划详细描述客户端网络功能的实现步骤。

---

## 一、新增 `connect_req` 结构体

**文件**：`src/env/scheduler/net/common/struct.hh`

**位置**：在 `conn_req` 结构体之后（约第 206 行后）新增

**内容**：
```cpp
struct connect_req {
    const char* address;       // 目标地址（如 "127.0.0.1"）
    uint16_t port;             // 目标端口
    std::move_only_function<void(session_id_t)> cb;  // 连接完成回调
};
```

**设计要点**：
- 回调签名 `void(session_id_t)` 与 `conn_req` 完全一致，确保下游管道统一处理服务端/客户端连接
- 连接失败时回调空 `session_id_t{}`（与 `accept_scheduler` 的错误处理方式一致）
- `address` 和 `port` 作为请求参数传入，而非在 scheduler init 时固定（客户端可能连接不同目标）

---

## 二、新增 `connect` sender

**新建文件**：`src/env/scheduler/net/senders/connect.hh`

完全对称于 `senders/conn.hh` 的结构，包含四部分：

### 2.1 `op` 结构体

```cpp
namespace pump::scheduler::net::senders::connect {
    template <typename scheduler_t>
    struct op {
        using request_t = common::connect_req;
        constexpr static bool net_sender_connect_op = true;  // 类型标记
        scheduler_t* scheduler;
        const char* address;
        uint16_t port;

        op(scheduler_t* s, const char* addr, uint16_t p)
            : scheduler(s), address(addr), port(p) {}

        op(op&& rhs) noexcept
            : scheduler(rhs.scheduler), address(rhs.address), port(rhs.port) {}

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
}
```

**对比 `conn::op`**：
- 额外持有 `address` 和 `port` 字段（conn::op 不需要，因为 accept 是被动的）
- 使用 `connect_req` 替代 `conn_req`
- 类型标记为 `net_sender_connect_op`（conn::op 为 `net_sender_conn_op`）

### 2.2 `sender` 结构体

```cpp
namespace pump::scheduler::net::senders::connect {
    template<typename scheduler_t>
    struct sender {
        scheduler_t* scheduler;
        const char* address;
        uint16_t port;

        sender(scheduler_t* s, const char* addr, uint16_t p)
            : scheduler(s), address(addr), port(p) {}

        sender(sender &&rhs) noexcept
            : scheduler(rhs.scheduler), address(rhs.address), port(rhs.port) {}

        auto make_op() {
            return op<scheduler_t>(scheduler, address, port);
        }

        template<typename context_t>
        auto connect() {
            return core::builder::op_list_builder<0>().push_back(make_op());
        }
    };
}
```

### 2.3 `op_pusher` 特化

```cpp
namespace pump::core {
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
    && (get_current_op_type_t<pos, scope_t>::net_sender_connect_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename context_t>
        static void push_value(context_t &context, scope_t &scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(context, scope);
        }
    };
}
```

- 与 `conn.hh` 中的 `op_pusher` 特化结构完全一致
- 通过 `net_sender_connect_op` 约束匹配

### 2.4 `compute_sender_type` 特化

```cpp
namespace pump::core {
    template <typename context_t, typename scheduler_t>
    struct compute_sender_type<context_t, scheduler::net::senders::connect::sender<scheduler_t>> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<scheduler::net::common::session_id_t>{};
        }
    };
}
```

- 返回类型为 `session_id_t`，与 `conn` sender 完全一致
- 确保下游管道可以无差异对待服务端和客户端连接

---

## 三、更新 `net.hh` API 层

**文件**：`src/env/scheduler/net/net.hh`

### 3.1 新增 include

在现有 include 区域（约第 9-13 行）添加：
```cpp
#include "./senders/connect.hh"
```

### 3.2 新增 `connect()` API 函数

在 `namespace pump::scheduler::net` 中添加两个重载：

```cpp
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
```

**放置位置**：建议在 `wait_connection()` 之后、`join()` 之前，保持"连接建立 → 会话管理"的逻辑顺序。

---

## 四、实现 epoll 后端 `connect_scheduler`

**新建文件**：`src/env/scheduler/net/epoll/connect_scheduler.hh`

独立文件，避免膨胀现有 `scheduler.hh`。

### 4.1 类定义

```cpp
namespace pump::scheduler::net::epoll {
    template <template<typename> class conn_op_t>
    struct connect_scheduler {
        friend struct conn_op_t<connect_scheduler>;
    };
}
```

- 模板签名与 `accept_scheduler` 完全一致：`template <template<typename> class conn_op_t>`
- 便于泛型代码统一处理

### 4.2 核心成员

| 成员 | 类型 | 说明 | 对比 accept_scheduler |
|------|------|------|----------------------|
| `conn_request_q` | `mpsc::queue<connect_req*, 2048>` | 连接请求队列 | 类型从 `conn_req*` 变为 `connect_req*` |
| `session_q` | `mpmc::queue<session_id_t, 2048>` | 已建立连接的会话队列 | 完全相同 |
| `poller` | `detail::poller_epoll` | epoll 实例 | 完全相同 |
| `_recv_buffer_size` | `size_t` | 接收缓冲区大小 | 完全相同 |
| `_shutdown` | `atomic<bool>` | 关闭标志 | 完全相同 |
| `pending_connects` | `std::list<pending_connect_info>` | 正在进行中的连接信息 | **新增**，跟踪 EINPROGRESS 状态的 fd |

**新增辅助结构**（类内私有）：
```cpp
struct pending_connect_info {
    int fd;
    common::connect_req* req;  // 保留原始请求，用于连接完成后回调或失败处理
};
```

### 4.3 核心方法实现

#### `init(const scheduler_config& cfg)` / `init()`
- **不需要** bind/listen（与 accept_scheduler 的核心差异）
- 只初始化 epoll poller 和 `_recv_buffer_size`
- 返回 0 表示成功

```cpp
int init(const common::scheduler_config& cfg) {
    _recv_buffer_size = cfg.recv_buffer_size;
    return 0;  // 无需 bind/listen
}
```

#### `schedule(connect_req* req)`
- 与 accept_scheduler 的 `schedule(conn_req*)` 模式对称
- 先检查 session_q 是否有已完成的连接；若有直接回调
- 否则发起新的非阻塞 connect

```cpp
auto schedule(common::connect_req* req) {
    if (auto opt = session_q.try_dequeue(); opt) {
        req->cb(opt.value());
        delete req;
    } else {
        initiate_connect(req);
    }
}
```

#### `initiate_connect(connect_req* req)` — **新增方法**
- 创建非阻塞 socket：`socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)`
- 构造 `sockaddr_in`，调用 `::connect(fd, ...)`
- 如果 `connect` 返回 0（立即成功，极少见）：直接 `create_internal_session(fd)`，回调 `req->cb(sid)`
- 如果 `connect` 返回 -1 且 `errno == EINPROGRESS`（正常的非阻塞连接）：
  - 注册 `EPOLLOUT` 到 poller，监听可写事件
  - 将 `{fd, req}` 存入 `pending_connects` 列表
- 如果其他错误：回调 `req->cb(session_id_t{})`，关闭 fd，`delete req`

#### `handle_connect_completion(epoll_event* e)` — **新增方法**
- 从 `pending_connects` 中找到对应 fd 的条目
- 调用 `getsockopt(fd, SOL_SOCKET, SO_ERROR, ...)` 检查连接结果
- 如果 `so_error == 0`（连接成功）：
  - 从 poller 移除该 fd 的 EPOLLOUT 监听
  - `create_internal_session(fd)` 创建 session
  - 回调 `req->cb(sid)`
- 如果 `so_error != 0`（连接失败）：
  - 回调 `req->cb(session_id_t{})`
  - 关闭 fd
- 从 `pending_connects` 中移除该条目
- `delete req`

#### `create_internal_session(int fd)`
- 与 `accept_scheduler::create_internal_session` **完全相同**的逻辑
- 设置 O_NONBLOCK（如果尚未设置）
- 创建 `session_t` + `recv_cache` + `epoll_send_cache`
- 编码为 `session_id_t`
- 尝试匹配等待中的 conn_request_q 请求，否则放入 session_q

```cpp
auto create_internal_session(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    auto* s = new session_t(fd, new common::detail::recv_cache(_recv_buffer_size), new epoll_send_cache());
    auto sid = common::session_id_t::encode(s);
    if (const auto opt = conn_request_q.try_dequeue(); opt) {
        opt.value()->cb(sid);
        delete opt.value();
    } else {
        session_q.try_enqueue(sid);
    }
}
```

注意：这里 `conn_request_q` 的类型是 `mpsc::queue<connect_req*, 2048>`，需要相应调整回调调用方式（`opt.value()->cb(sid)` 中 `cb` 字段名相同）。

#### `advance()`
- 事件循环推进，与 accept_scheduler 类似
- `epoll_wait` 检测事件
- 对于收到的 `EPOLLOUT` 事件调用 `handle_connect_completion`
- 检查 `_shutdown` 标志

```cpp
auto advance() {
    if (_shutdown.load(std::memory_order_relaxed)) {
        drain_on_shutdown();
        return false;
    }
    poller.poll([this](epoll_event* e) {
        handle_connect_completion(e);
    });
    return true;
}
```

#### `shutdown()` / `drain_on_shutdown()`
- 与 accept_scheduler 对称
- `shutdown()` 设置 `_shutdown = true`
- `drain_on_shutdown()` 清理 `conn_request_q` 中的待处理请求（回调空 session_id_t）
- 关闭所有 `pending_connects` 中的 fd

#### `advance(const runtime_t&)` 模板重载
- 与 accept_scheduler 一致，简单委托到 `advance()`

### 4.4 需要引入的头文件

```cpp
#include <list>
#include <bits/move_only_function.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <cerrno>

#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"

#include "../common/struct.hh"
#include "../common/detail.hh"
#include "../common/error.hh"
#include "./epoll.hh"
```

---

## 五、实现 io_uring 后端 `connect_scheduler`

**新建文件**：`src/env/scheduler/net/io_uring/connect_scheduler.hh`

### 5.1 与 epoll 后端的核心差异

| 方面 | epoll 后端 | io_uring 后端 |
|------|-----------|--------------|
| connect 提交 | `::connect()` + `EPOLLOUT` 监听 | `io_uring_prep_connect(sqe, ...)` |
| 完成通知 | `epoll_wait` + `getsockopt(SO_ERROR)` | CQE `res == 0` 表示成功 |
| 事件类型 | 无（用 fd 匹配） | 需新增 `uring_event_type::CONNECT` |

### 5.2 新增事件类型

在 `io_uring/scheduler.hh` 的 `uring_event_type` 枚举中新增：

```cpp
enum uring_event_type {
    READ = 0,
    WRITE = 1,
    ACCEPT = 2,
    CONNECT = 3    // 新增
};
```

或者在新文件中定义独立的事件类型，避免修改现有枚举。推荐直接在现有枚举中添加，因为 `process_cqe` 等逻辑需要统一处理。

### 5.3 类定义与成员

```cpp
namespace pump::scheduler::net::io_uring {
    template <template<typename> class conn_op_t>
    struct connect_scheduler {
        friend struct conn_op_t<connect_scheduler>;
    private:
        core::mpsc::queue<common::connect_req*, 2048> conn_request_q;
        core::mpmc::queue<common::session_id_t, 2048> session_q;
        struct io_uring ring;
        size_t _recv_buffer_size = 4096;
        std::atomic<bool> _shutdown{false};

        // 跟踪进行中的连接
        struct pending_connect_info {
            int fd;
            sockaddr_in addr;           // 保存地址信息（io_uring_prep_connect 需要地址在提交期间有效）
            io_uring_request uring_req;  // 关联的 io_uring 请求
        };
    };
}
```

### 5.4 核心方法实现

#### `init(const scheduler_config& cfg)` / `init(unsigned queue_depth)`
- 初始化 `io_uring ring`：`io_uring_queue_init(queue_depth, &ring, 0)`
- 设置 `_recv_buffer_size`
- **不需要** bind/listen

#### `schedule(connect_req* req)`
- 与 epoll 版本相同的请求匹配逻辑

#### `initiate_connect(connect_req* req)`
- 创建非阻塞 socket
- 分配 `pending_connect_info`，填充 `sockaddr_in`
- 获取 sqe：`io_uring_get_sqe(&ring)`
- 提交异步连接：`io_uring_prep_connect(sqe, fd, addr, addrlen)`
- 设置 user_data：`io_uring_sqe_set_data(sqe, &pending.uring_req)`，其中 `uring_req.type = CONNECT`
- `io_uring_submit(&ring)`

#### `handle_io_uring()`
- 类似 accept_scheduler 的 `handle_io_uring()`
- 遍历 CQE
- 对于 `CONNECT` 类型的 CQE：
  - `cqe->res == 0`：连接成功，`create_internal_session(fd)`
  - `cqe->res < 0`：连接失败，回调 `req->cb(session_id_t{})`
- `io_uring_cqe_seen(&ring, cqe)`

#### `create_internal_session(int fd)`
- 与 epoll 版本完全相同
- 注意：io_uring 版本中 session_t 的构造可能略有不同（无 epoll_send_cache），需参考 io_uring accept_scheduler 的实现

#### `advance()` / `shutdown()` / `drain_on_shutdown()`
- 与 epoll 版本对称，事件驱动方式改为 io_uring

### 5.5 需要引入的头文件

```cpp
#include <cstdint>
#include <list>
#include <bits/move_only_function.h>
#include <liburing.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <arpa/inet.h>

#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"

#include "../common/struct.hh"
#include "../common/detail.hh"
#include "../common/error.hh"
```

---

## 六、编写 echo 客户端示例

**新建目录**：`apps/example/echo_client/`

**新建文件**：`apps/example/echo_client/echo_client.cc`

### 6.1 runtime 类型定义

```cpp
// io_uring 后端
using connect_scheduler_t = scheduler::net::io_uring::connect_scheduler<scheduler::net::senders::conn::op>;
using session_scheduler_t = scheduler::net::io_uring::session_scheduler<
    scheduler::net::senders::join::op,
    scheduler::net::senders::recv::op,
    scheduler::net::senders::send::op,
    scheduler::net::senders::stop::op>;
using task_scheduler_t = scheduler::task::task_scheduler<>;
using runtime_schedulers = env::runtime::runtime_schedulers<
    task_scheduler_t, connect_scheduler_t, session_scheduler_t>;
```

注意：`conn_op_t` 模板参数复用 `senders::conn::op`（与 command.md 2.3 节一致）。

### 6.2 create_runtime_schedulers

```cpp
auto create_client_runtime_schedulers() {
    auto* rs = new runtime_schedulers();
    auto cfg = scheduler::net::common::scheduler_config{};

    // Core 0: task + connect + session
    auto* task_sched_0 = new task_scheduler_t();
    auto* connect_sched = new connect_scheduler_t();
    connect_sched->init(cfg);  // 不需要 address/port
    auto* session_sched_0 = new session_scheduler_t();
    session_sched_0->init(cfg);
    rs->add_core_schedulers(task_sched_0, connect_sched, session_sched_0);

    // Core 1..N-1: task + nullptr + session
    auto num_cores = std::thread::hardware_concurrency();
    for (unsigned i = 1; i < num_cores; ++i) {
        auto* task_sched = new task_scheduler_t();
        auto* session_sched = new session_scheduler_t();
        session_sched->init(cfg);
        rs->add_core_schedulers(task_sched, nullptr, session_sched);
    }

    return rs;
}
```

### 6.3 客户端会话处理管道

```cpp
auto client_session_proc(runtime_schedulers* rs, session_id_t sid) {
    auto session_count = rs->get_schedulers<session_scheduler_t>().size();
    auto core_idx = sid.raw() % session_count;
    auto* session_sched = rs->get_schedulers<session_scheduler_t>()[core_idx];

    return scheduler::net::join(session_sched, sid)
        >> scheduler::net::send(session_sched, sid, /* 发送数据 */)
        >> scheduler::net::recv(session_sched, sid)
        >> then([](auto* buf) {
            // 处理服务端回显数据
        })
        >> scheduler::net::stop(session_sched, sid);
}
```

### 6.4 main 函数结构

```cpp
int main() {
    just()
        >> get_context<runtime_schedulers*>()
        >> then([](runtime_schedulers* rs) {
            auto* connect_sched = rs->get_schedulers<connect_scheduler_t>()[0];
            scheduler::net::connect(connect_sched, "127.0.0.1", 8080)
                >> then([rs](session_id_t sid) {
                    client_session_proc(rs, sid)
                        >> submit(core::make_root_context());
                })
                >> submit(core::make_root_context(rs));
        })
        >> get_context<runtime_schedulers*>()
        >> then([](runtime_schedulers* rs) {
            env::runtime::start(rs->schedulers_by_core);
        })
        >> submit(core::make_root_context(
            create_client_runtime_schedulers()
        ));
}
```

### 6.5 更新 CMakeLists.txt

**文件**：`apps/CMakeLists.txt`（或相应的子目录 CMakeLists）

新增构建目标：
```cmake
add_executable(example.echo_client apps/example/echo_client/echo_client.cc)
target_link_libraries(example.echo_client PRIVATE uring)
```

需参考现有 `example.echo` 目标的配置方式。

---

## 七、实现优先级与执行顺序

### P0 — 核心基础设施（必须先完成）

| 序号 | 任务 | 文件 | 依赖 |
|------|------|------|------|
| 1 | 新增 `connect_req` 结构体 | `common/struct.hh` | 无 |
| 2 | 新增 `connect` sender（op + sender + op_pusher + compute_sender_type） | `senders/connect.hh`（新建） | 1 |
| 3 | 更新 `net.hh` 添加 `connect()` API | `net.hh` | 2 |

### P1 — 后端实现

| 序号 | 任务 | 文件 | 依赖 |
|------|------|------|------|
| 4 | 实现 epoll `connect_scheduler` | `epoll/connect_scheduler.hh`（新建） | 1 |
| 5 | 实现 io_uring `connect_scheduler` | `io_uring/connect_scheduler.hh`（新建） | 1 |

### P2 — 示例与验证

| 序号 | 任务 | 文件 | 依赖 |
|------|------|------|------|
| 6 | 编写 echo 客户端示例 | `apps/example/echo_client/echo_client.cc`（新建） | 3, 4 或 5 |
| 7 | 更新 CMakeLists.txt | `apps/CMakeLists.txt` | 6 |
| 8 | 编译验证 | — | 7 |
| 9 | 功能验证：echo 客户端连接 echo 服务端 | — | 8 |

### P3 — 健壮性（后续扩展）

| 序号 | 任务 | 说明 |
|------|------|------|
| 10 | 连接超时处理 | connect_scheduler 内部可选超时机制 |
| 11 | 连接失败重试 | 在 sender 管道层面通过 `repeat` + 异常处理实现 |
| 12 | 多目标地址连接 | DNS 解析后尝试多个地址 |

---

## 八、验证计划

1. **编译验证**：完成 P0+P1 后，确保所有新文件编译通过（无模板实例化错误）
2. **启动验证**：启动 echo 服务端（`example.echo`），然后启动 echo 客户端（`example.echo_client`），确认客户端能正常连接
3. **功能验证**：客户端发送数据，验证服务端正确回显
4. **多连接验证**：客户端建立多个连接，验证 session 分配和数据隔离正确
5. **异常验证**：
   - 服务端未启动时客户端连接，验证 ECONNREFUSED 正确处理
   - 服务端异常断开，验证客户端 session 正确清理
6. **对称性验证**：确认连接建立后，客户端和服务端使用完全相同的 `join/recv/send/stop` 管道代码

---

## 九、文件变更清单

| 操作 | 文件路径 | 变更内容 |
|------|----------|----------|
| 修改 | `src/env/scheduler/net/common/struct.hh` | 新增 `connect_req` 结构体 |
| 新增 | `src/env/scheduler/net/senders/connect.hh` | `connect` 的 op、sender、op_pusher、compute_sender_type |
| 修改 | `src/env/scheduler/net/net.hh` | 新增 `connect()` API，新增 `#include` |
| 新增 | `src/env/scheduler/net/epoll/connect_scheduler.hh` | epoll 后端 `connect_scheduler` |
| 新增 | `src/env/scheduler/net/io_uring/connect_scheduler.hh` | io_uring 后端 `connect_scheduler` |
| 新增 | `apps/example/echo_client/echo_client.cc` | echo 客户端示例 |
| 修改 | `apps/CMakeLists.txt`（或子目录） | 新增 `example.echo_client` 构建目标 |

---

## 十、设计约束检查清单

实现时需逐项确认：

- [ ] `connect_scheduler` 只能被单个线程运行（Single-Thread Scheduler）
- [ ] 使用 `mpsc::queue` / `mpmc::queue` 做请求/结果传递，不引入锁（Lock-Free）
- [ ] `connect` sender 的 op 支持被编译期平铺到 tuple 中（Flat OpTuple）
- [ ] `connect` op 的 `start` 方法通过 `op_pusher<pos+1>` 推进（Position-based Pushing）
- [ ] connect_scheduler 与 session_scheduler 保持执行域隔离（Scheduler Isolation）
- [ ] session_scheduler 零修改（客户端连接产生的 session_id_t 格式完全一致）
- [ ] `connect_req` 回调签名与 `conn_req` 一致（`void(session_id_t)`）
- [ ] epoll 和 io_uring 双后端都已实现
