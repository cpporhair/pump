# UDP 层设计文档

> 基于 `ai_context/udp_requirements.md` 的实现设计。遵循 TCP 层已有的代码模式和 PUMP 框架约定。

## 1. 架构总览

```
用户 API (udp::recv, udp::send)
    |
Sender/Op 层 (senders/recv.hh, senders/send.hh)
    |
Scheduler 层 (io_uring/scheduler.hh)
    |
公共层 (common/struct.hh) -- endpoint, datagram, req 类型
    |
内核 (io_uring recvmsg/sendmsg + UDP socket)
```

与 TCP 的架构差异：
- 无 accept_scheduler / session_scheduler 分离 — 只有一个 scheduler
- 无 conn/join/stop sender — 无连接生命周期
- 无 packet_buffer / ring buffer — 数据报天然有消息边界
- 无 session / session_id — 用 endpoint 标识对端

## 2. 类型定义

### 2.1 endpoint

```cpp
// common/struct.hh
namespace pump::scheduler::udp::common {

    struct endpoint {
        sockaddr_in _addr{};

        constexpr endpoint() = default;

        endpoint(const char* ip, uint16_t port) {
            _addr.sin_family = AF_INET;
            _addr.sin_port = htons(port);
            inet_pton(AF_INET, ip, &_addr.sin_addr);
        }

        explicit endpoint(const sockaddr_in& addr) : _addr(addr) {}

        [[nodiscard]] uint16_t port() const { return ntohs(_addr.sin_port); }
        [[nodiscard]] uint32_t ip_raw() const { return _addr.sin_addr.s_addr; }
        [[nodiscard]] const sockaddr_in& raw() const { return _addr; }
        [[nodiscard]] sockaddr_in& raw() { return _addr; }

        bool operator==(const endpoint& o) const {
            return _addr.sin_port == o._addr.sin_port
                && _addr.sin_addr.s_addr == o._addr.sin_addr.s_addr;
        }
    };
}
```

### 2.2 datagram

复用 `tcp::common::net_frame` 的设计（move-only RAII buffer）。如果后续提取公共基类再改，初期先独立定义以避免引入 TCP 依赖。

```cpp
// common/struct.hh
namespace pump::scheduler::udp::common {

    struct datagram {
        char* _data;
        uint32_t _len;

        datagram() : _data(nullptr), _len(0) {}
        datagram(char* data, uint32_t len) : _data(data), _len(len) {}

        datagram(datagram&& rhs) noexcept : _data(rhs._data), _len(rhs._len) {
            rhs._data = nullptr; rhs._len = 0;
        }
        datagram& operator=(datagram&& rhs) noexcept {
            if (this != &rhs) {
                delete[] _data;
                _data = rhs._data; _len = rhs._len;
                rhs._data = nullptr; rhs._len = 0;
            }
            return *this;
        }

        datagram(const datagram&) = delete;
        datagram& operator=(const datagram&) = delete;

        ~datagram() { delete[] _data; }

        [[nodiscard]] char* release() noexcept {
            auto* p = _data; _data = nullptr; _len = 0; return p;
        }
        [[nodiscard]] const char* data() const { return _data; }
        [[nodiscard]] uint32_t size() const { return _len; }

        template <typename T>
        [[nodiscard]] const T* as() const { return reinterpret_cast<const T*>(_data); }
    };
}
```

### 2.3 请求类型

```cpp
// common/struct.hh
namespace pump::scheduler::udp::common {

    struct recv_req {
        std::move_only_function<void(std::variant<
            std::pair<datagram, endpoint>,
            std::exception_ptr
        >)> cb;
    };

    struct send_req {
        endpoint target;
        datagram frame;
        std::move_only_function<void(bool)> cb;

        // sendmsg 所需结构（在 scheduler 中填充）
        msghdr _msg{};
        iovec _iov{};

        void prepare() {
            _iov = {frame._data, frame._len};
            _msg.msg_name = &target.raw();
            _msg.msg_namelen = sizeof(sockaddr_in);
            _msg.msg_iov = &_iov;
            _msg.msg_iovlen = 1;
        }
    };

    struct scheduler_config {
        const char* address = "0.0.0.0";
        uint16_t port = 0;
        unsigned queue_depth = 256;
        uint32_t max_datagram_size = 65536;
        uint32_t recv_depth = 32;       // 同时 in-flight 的 recvmsg 数
    };
}
```

## 3. Scheduler 设计

### 3.1 核心状态

```cpp
namespace pump::scheduler::udp::io_uring {

    struct recv_slot {
        char* buf;                  // 预分配 buffer
        uint32_t buf_size;
        sockaddr_in src_addr;       // recvmsg 填充源地址
        msghdr msg;
        iovec iov;
        io_uring_request* uring_req;

        void prepare(uint32_t max_size) {
            iov = {buf, max_size};
            msg = {};
            msg.msg_name = &src_addr;
            msg.msg_namelen = sizeof(src_addr);
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
        }
    };

    template <template<typename> class recv_op_t, template<typename> class send_op_t>
    struct scheduler {
        friend struct recv_op_t<scheduler>;
        friend struct send_op_t<scheduler>;
    private:
        int _fd = -1;
        struct ::io_uring _ring{};
        uint32_t _max_datagram_size = 65536;

        // 请求队列（跨线程投递）
        core::per_core::queue<common::recv_req*, 2048> recv_q;
        core::per_core::queue<common::send_req*, 2048> send_q;

        // 内部状态（单线程访问）
        core::local::queue<common::recv_req*> pending_recv;    // 等待数据的 recv_req
        core::local::queue<recv_slot*> pending_slots;          // 等待 recv_req 的已完成数据报
        std::vector<recv_slot> recv_slots;                     // 预分配的 recv buffer 池
        core::local::queue<recv_slot*> free_slots;             // 可复用的 slot

        std::atomic<bool> _shutdown{false};
    };
}
```

### 3.2 初始化

```cpp
int init(const common::scheduler_config& cfg) {
    return init(cfg.address, cfg.port, cfg.queue_depth,
                cfg.max_datagram_size, cfg.recv_depth);
}

int init(const char* address, uint16_t port,
         unsigned queue_depth = 256,
         uint32_t max_datagram_size = 65536,
         uint32_t recv_depth = 32) {
    // 1. 创建 UDP socket
    _fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

    // 2. SO_REUSEADDR | SO_REUSEPORT
    int opt = 1;
    setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));

    // 3. bind
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, address, &addr.sin_addr);
    bind(_fd, ...);

    // 4. 初始化 io_uring
    io_uring_queue_init(queue_depth, &_ring, 0);

    // 5. 预分配 recv_slots 并提交 recvmsg
    _max_datagram_size = max_datagram_size;
    recv_slots.resize(recv_depth);
    for (auto& slot : recv_slots) {
        slot.buf = new char[max_datagram_size];
        slot.buf_size = max_datagram_size;
        submit_recvmsg(&slot);
    }
    return 0;
}
```

### 3.3 advance 主循环

```
advance():
    1. drain_recv_q()    -- 新的 recv_req 入队
    2. drain_send_q()    -- 为每个 send_req 提交 sendmsg SQE
    3. handle_io()       -- 处理 CQE

drain_recv_q():
    recv_q.drain([](recv_req* req) {
        if (pending_slots 有已完成数据) {
            slot = pending_slots.dequeue()
            req->cb({datagram, endpoint})    // 立即交付
            recycle_slot(slot)               // 归还 slot，重新提交 recvmsg
        } else {
            pending_recv.enqueue(req)        // 等待
        }
    })

drain_send_q():
    send_q.drain([](send_req* req) {
        req->prepare()
        submit_sendmsg(req)
    })
    io_uring_submit(&_ring)   // 批量提交

handle_io():
    while (io_uring_peek_cqe == 0) {
        if (cqe->res < 0)
            handle_error(cqe)
        else
            process_cqe(cqe)
        io_uring_cqe_seen
    }

process_cqe(cqe):
    switch (uring_req->event_type) {
    case recvmsg:
        slot = uring_req->user_data
        len = cqe->res
        if (pending_recv 有等待的 req) {
            req = pending_recv.dequeue()
            // 从 slot 拷贝数据构造 datagram
            char* copy = new char[len]
            memcpy(copy, slot->buf, len)
            req->cb({datagram{copy, len}, endpoint{slot->src_addr}})
            delete req
            recycle_slot(slot)      // 归还 slot 并重新提交 recvmsg
        } else {
            // 无等待 recv_req，暂存数据
            // 从 slot 拷贝数据到新 buffer，slot 归还
            // 或：直接把 slot 加入 pending_slots（延迟归还）
            pending_slots.enqueue(slot)  // slot 暂存，不重新提交 recvmsg
        }
    case sendmsg:
        req = uring_req->user_data
        req->cb(cqe->res >= 0)
        delete req
        delete uring_req
    }

recycle_slot(slot):
    submit_recvmsg(slot)   // 重新提交 recvmsg SQE
```

### 3.4 recv buffer 管理策略

**核心思路**：固定数量的 `recv_slot`，每个 slot 含一个预分配 buffer。

```
状态流转：

  [提交 recvmsg]                [CQE 完成]
       |                            |
  slot 在 io_uring 中等待  --->  slot 含完整数据报
                                    |
                          有 pending_recv?
                         /               \
                       是                  否
                      /                     \
              拷贝数据给 req          slot 进入 pending_slots
              归还 slot               (暂不重新提交 recvmsg)
              重新提交 recvmsg         |
                                  recv_req 到来时取出
                                  拷贝数据给 req
                                  归还 slot
                                  重新提交 recvmsg
```

**要点**：
- `recv_depth` 个 slot = 最多 `recv_depth` 个 recvmsg 同时 in-flight
- 当数据到达无人消费时，slot 滞留在 pending_slots，in-flight 的 recvmsg 减少
- 当 recv_req 到来消费掉 pending_slots 中的 slot，slot 被归还，recvmsg 重新提交
- 天然的背压：消费者跟不上时，in-flight recvmsg 减少，内核丢包（UDP 本身无保证）

**内存**：`recv_depth * max_datagram_size`。默认 32 * 64KB = 2MB。若应用层只用小包（如 RPC），可将 `max_datagram_size` 设为 1500，则 32 * 1.5KB = 48KB。

### 3.5 io_uring 操作

```cpp
enum struct uring_event_type { recvmsg = 0, sendmsg = 1 };

struct io_uring_request {
    uring_event_type event_type;
    void* user_data;     // recvmsg: recv_slot*,  sendmsg: send_req*
};

void submit_recvmsg(recv_slot* slot) {
    slot->prepare(_max_datagram_size);
    auto* sqe = io_uring_get_sqe(&_ring);
    auto* uring_req = new io_uring_request{uring_event_type::recvmsg, slot};
    io_uring_prep_recvmsg(sqe, _fd, &slot->msg, 0);
    io_uring_sqe_set_data(sqe, uring_req);
    io_uring_submit(&_ring);
}

void submit_sendmsg(send_req* req) {
    auto* sqe = io_uring_get_sqe(&_ring);
    auto* uring_req = new io_uring_request{uring_event_type::sendmsg, req};
    io_uring_prep_sendmsg(sqe, _fd, &req->_msg, 0);
    io_uring_sqe_set_data(sqe, uring_req);
    // 不立即 submit — drain_send_q 结束后批量 submit
}
```

### 3.6 错误处理

```
recvmsg 失败 (cqe->res < 0):
    - 如果是 EAGAIN/EINTR → 重新提交 recvmsg（理论上 io_uring 不返回这些）
    - 其他错误 → 重新提交 recvmsg（UDP socket 级错误通常是瞬时的，如 ICMP unreachable）
    - 注意：不需要关闭 socket（与 TCP read 失败不同）

sendmsg 失败 (cqe->res < 0):
    - cb(false)
    - 不关闭 socket（发送失败是目标不可达，不影响后续操作）
```

### 3.7 shutdown

```cpp
void shutdown() {
    _shutdown.store(true, std::memory_order_release);
}

// advance 中检查
if (_shutdown.load(std::memory_order_acquire)) {
    drain_on_shutdown();
    return false;
}

void drain_on_shutdown() {
    // 失败所有 pending recv_req
    while (auto opt = pending_recv.try_dequeue()) {
        opt.value()->cb(std::make_exception_ptr(socket_closed_error()));
        delete opt.value();
    }
    recv_q.drain([](recv_req* req) {
        req->cb(std::make_exception_ptr(socket_closed_error()));
        delete req;
    });
    send_q.drain([](send_req* req) {
        req->cb(false);
        delete req;
    });
    // 释放 pending_slots 中滞留的 slot（数据丢弃）
    while (auto opt = pending_slots.try_dequeue()) {
        // slot 本身属于 recv_slots vector，不需要 delete
    }
    // 关闭 fd
    if (_fd >= 0) { ::close(_fd); _fd = -1; }
}
```

## 4. Sender/Op 层

严格遵循 TCP sender 的代码结构（六组件模式）。

### 4.1 recv sender

```cpp
// senders/recv.hh
namespace pump::scheduler::udp::senders::recv {

    template <typename scheduler_t>
    struct op {
        constexpr static bool udp_sender_recv_op = true;
        scheduler_t* scheduler;

        op(scheduler_t* s) : scheduler(s) {}
        op(op&& rhs) noexcept : scheduler(rhs.scheduler) {}

        template<uint32_t pos, typename context_t, typename scope_t>
        auto start(context_t& context, scope_t& scope) {
            scheduler->schedule(
                new common::recv_req{
                    [context = context, scope = scope](auto&& res) mutable {
                        if (res.index() == 0) [[likely]] {
                            auto& [dg, ep] = std::get<0>(res);
                            core::op_pusher<pos + 1, scope_t>::push_value(
                                context, scope,
                                std::move(dg), std::move(ep)
                            );
                        } else {
                            core::op_pusher<pos + 1, scope_t>::push_exception(
                                context, scope, std::get<1>(res)
                            );
                        }
                    }
                }
            );
        }
    };

    template <typename scheduler_t>
    struct sender {
        scheduler_t* scheduler;

        sender(scheduler_t* s) : scheduler(s) {}
        sender(sender&& rhs) noexcept : scheduler(rhs.scheduler) {}

        auto make_op() { return op<scheduler_t>(scheduler); }

        template <typename context_t>
        auto connect() {
            return core::builder::op_list_builder<0>().push_back(make_op());
        }
    };
}

// op_pusher 特化
namespace pump::core {
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::udp_sender_recv_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename context_t>
        static void push_value(context_t& context, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(context, scope);
        }
    };

    // compute_sender_type 特化
    template <typename context_t, typename scheduler_t>
    struct compute_sender_type<context_t, scheduler::udp::senders::recv::sender<scheduler_t>> {
        consteval static uint32_t count_value() { return 2; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<
                std::pair<scheduler::udp::common::datagram,
                          scheduler::udp::common::endpoint>
            >{};
        }
    };
}
```

**注意**：`count_value() = 2`，下游 `then` 接收两个参数 `(datagram&&, endpoint)`。

`compute_sender_type` 的 `get_value_type_identity` 需要确认框架是否支持多返回值的 type_identity 表达方式。如果框架用 `count_value` + 逐个 `get_value_type_identity<N>` 的模式，需要相应调整。参照 TCP recv sender 的实现（`count_value() = 1`，返回单个 `net_frame`），UDP recv 返回两个值可能需要：
- 方案 A：包装成 `std::pair<datagram, endpoint>` 作为单个值（`count_value() = 1`）
- 方案 B：确认框架支持 `push_value(ctx, scope, val1, val2)` 多参数

TCP recv 用 `push_value(context, scope, std::move(std::get<0>(res)))` 传单个值。PUMP 框架的 `push_value` 支持变参 `V&&... v`，所以**方案 B 可行**。但 `compute_sender_type` 需要能表达两个输出类型。实现时需确认具体机制 — 如果不方便，退回方案 A 用 pair 包装。

### 4.2 send sender

```cpp
// senders/send.hh
namespace pump::scheduler::udp::senders::send {

    template <typename scheduler_t>
    struct op {
        constexpr static bool udp_sender_send_op = true;
        scheduler_t* scheduler;
        common::endpoint target;
        common::datagram frame;

        op(scheduler_t* s, common::endpoint ep, common::datagram&& f)
            : scheduler(s), target(ep), frame(std::move(f)) {}

        op(op&& rhs) noexcept
            : scheduler(rhs.scheduler)
            , target(rhs.target)
            , frame(std::move(rhs.frame)) {}

        template<uint32_t pos, typename context_t, typename scope_t>
        auto start(context_t& context, scope_t& scope) {
            scheduler->schedule(
                new common::send_req{
                    target,
                    std::move(frame),
                    [context = context, scope = scope](bool ok) mutable {
                        core::op_pusher<pos + 1, scope_t>::push_value(
                            context, scope, ok
                        );
                    }
                }
            );
        }
    };

    template <typename scheduler_t>
    struct sender {
        scheduler_t* scheduler;
        common::endpoint target;
        common::datagram frame;

        sender(scheduler_t* s, common::endpoint ep, common::datagram&& f)
            : scheduler(s), target(ep), frame(std::move(f)) {}

        sender(sender&& rhs) noexcept
            : scheduler(rhs.scheduler)
            , target(rhs.target)
            , frame(std::move(rhs.frame)) {}

        auto make_op() {
            return op<scheduler_t>(scheduler, target, std::move(frame));
        }

        template <typename context_t>
        auto connect() {
            return core::builder::op_list_builder<0>().push_back(make_op());
        }
    };
}

// op_pusher + compute_sender_type 特化（与 TCP send 完全同构）
// count_value() = 1, type = bool
```

## 5. 公共 API

```cpp
// udp.hh
namespace pump::scheduler::udp {

    template <typename scheduler_t>
    inline auto recv(scheduler_t* sche) {
        return senders::recv::sender<scheduler_t>(sche);
    }

    template <typename scheduler_t>
    inline auto send(scheduler_t* sche, common::endpoint ep, void* data, uint32_t len) {
        return senders::send::sender<scheduler_t>(
            sche, ep,
            common::datagram(static_cast<char*>(data), len)
        );
    }
}
```

## 6. 构建集成

### 6.1 udp.cmake

```cmake
# UDP 层复用 tcp.cmake 已找到的 liburing
# 如果 UDP 独立编译，需要自己 find liburing
```

UDP 与 TCP 共用 liburing。`CMakeLists.txt` 已有 `include(src/env/scheduler/tcp/tcp.cmake)` 找 liburing，新增 `include(src/env/scheduler/udp/udp.cmake)` 即可。如果 UDP 先于 TCP include，需要自己 find_library。

### 6.2 CMakeLists.txt 改动

```cmake
include(src/env/scheduler/tcp/tcp.cmake)
include(src/env/scheduler/udp/udp.cmake)    # 新增
```

## 7. 示例程序

`apps/example/udp_echo/` — 验证 UDP 层基本功能。

### 7.1 结构

```
apps/example/udp_echo/
├── udp_echo.cc          ← main，启动 runtime
├── server.hh            ← echo server pipeline
└── client.hh            ← echo client pipeline
```

### 7.2 server pipeline

```cpp
void start(uint32_t core) {
    auto* sche = new udp::io_uring::scheduler();
    sche->init("0.0.0.0", 9000);

    just()
        >> forever()
        >> flat_map([sche](...) {
            return udp::recv(sche);
        })
        >> flat_map([sche](datagram&& dg, endpoint ep) {
            auto len = dg.size();
            auto* data = dg.release();
            return udp::send(sche, ep, data, len);
        })
        >> reduce()
        >> submit(make_root_context());

    // 启动 runtime 循环 advance
}
```

### 7.3 client pipeline

```cpp
auto* sche = new udp::io_uring::scheduler();
sche->init("0.0.0.0", 0);  // 随机端口

endpoint server{"127.0.0.1", 9000};

for_each(std::views::iota(0, 100))
    >> flat_map([sche, server](int i) {
        auto* buf = new char[4];
        *reinterpret_cast<int*>(buf) = i;
        return udp::send(sche, server, buf, 4)
            >> flat_map([sche](...) { return udp::recv(sche); })
            >> then([i](datagram&& dg, endpoint from) {
                assert(*reinterpret_cast<int*>(dg.data()) == i);
            });
    })
    >> reduce()
    >> submit(ctx);
```

## 8. 与 TCP 层的代码对应关系

| TCP 组件 | UDP 对应 | 说明 |
|----------|---------|------|
| `tcp::common::session_id_t` | `udp::common::endpoint` | 对端标识 |
| `tcp::common::net_frame` | `udp::common::datagram` | 数据载体 |
| `tcp::common::packet_buffer` | 无 | UDP 不需要流拆帧 |
| `tcp::common::detail::recv_cache` | `recv_slot`（scheduler 内部） | 接收缓冲 |
| `tcp::common::conn_req` | 无 | UDP 无连接 |
| `tcp::common::join_req` | 无 | UDP 无连接 |
| `tcp::common::stop_req` | 无 | UDP 无连接 |
| `tcp::common::recv_req` | `udp::common::recv_req` | cb 类型不同（多了 endpoint） |
| `tcp::common::send_req` | `udp::common::send_req` | endpoint 替代 session_id，无 length prefix |
| `tcp::io_uring::accept_scheduler` | 无 | UDP 无监听/接受 |
| `tcp::io_uring::session_scheduler` | `udp::io_uring::scheduler` | 唯一的 scheduler |
| `tcp::wait_connection()` | 无 | UDP 无连接 |
| `tcp::connect()` | 无 | UDP 无连接 |
| `tcp::join()` | 无 | UDP 无连接 |
| `tcp::stop()` | 无 | UDP 无连接 |
| `tcp::recv(sche, sid)` | `udp::recv(sche)` | 无 session 参数，返回值多一个 endpoint |
| `tcp::send(sche, sid, data, len)` | `udp::send(sche, ep, data, len)` | endpoint 替代 session_id |

## 9. 实现顺序

1. `common/struct.hh` — endpoint, datagram, recv_req, send_req, scheduler_config
2. `senders/recv.hh` — recv op/sender/op_pusher/compute_sender_type
3. `senders/send.hh` — send op/sender/op_pusher/compute_sender_type
4. `udp.hh` — 公共 API
5. `io_uring/scheduler.hh` — scheduler 实现
6. `udp.cmake` — 构建配置
7. `CMakeLists.txt` — include udp.cmake
8. `apps/example/udp_echo/` — 示例 + 验证
