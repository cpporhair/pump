# Net模块 & Echo示例 代码审查

## 审查范围
- `src/env/scheduler/net/` 全部文件
- `apps/example/echo/echo.cc`

---

## A. 严重BUG（UB / 数据损坏 / 崩溃）

### A1. net.hh send() 长度双重叠加 — 缓冲区越界读

**文件**: `src/env/scheduler/net/net.hh:49-55`

**问题**: `send()` 创建 `net_frame` 时错误地叠加了 `sizeof(uint16_t)`：

```cpp
// net.hh:53
net_frame(static_cast<char *>(data), len + sizeof(common::net_frame::_len))
```

接着 `send_req::prepare_frame()` 又加了一次：

```cpp
// struct.hh:268
_frame_len = static_cast<uint16_t>(frame._len + sizeof(uint16_t));
_send_vec[1] = {frame._data, frame._len};  // 发送 frame._len 字节
```

调用链（以echo为例）：
1. recv 得到 `net_frame(data, payload_len)` — data 是 `new char[payload_len]`
2. echo 调 `send(sched, sid, frame._data, frame._len)` — `len = payload_len`
3. net.hh 创建 `net_frame(data, payload_len + 2)` — frame._len 膨胀了2
4. prepare_frame: `_send_vec[1] = {data, payload_len + 2}` — **越界读2字节**
5. wire: `[payload_len+4][payload_len+2 bytes]` — **协议长度也错了**

每次 echo 往返，帧长度增长2字节，持续越界读。

**修复**: `net.hh::send()` 不应叠加 sizeof，由 `prepare_frame()` 负责协议封帧：

```cpp
// net.hh:50-55 修改为
return senders::send::sender<scheduler_t>(
    scheduler, sid,
    common::net_frame(static_cast<char *>(data), len)
);
```

---

### A2. io_uring accept_scheduler 栈局部地址传给异步操作 — 内核写入已释放栈

**文件**: `src/env/scheduler/net/io_uring/scheduler.hh:105-131`

**问题**: `handle_io_uring()` 在栈上创建 `sockaddr_in` 和 `socklen_t`，传指针给 `io_uring_prep_accept`：

```cpp
auto handle_io_uring() {
    sockaddr_in client_addr{};          // 栈变量
    socklen_t client_addr_len = sizeof(client_addr);  // 栈变量
    add_accept_request(server_socket, &client_addr, &client_addr_len);
    // ... 函数返回，栈帧销毁
    // io_uring 异步 accept 在未来某个时刻写入已释放的栈地址
}
```

**严重性**: 未定义行为。内核在连接到来时写入已释放的栈内存，可能损坏其他数据、导致崩溃。

**修复**: 将 `client_addr` 和 `client_addr_len` 改为成员变量（或动态分配并关联到请求）：

```cpp
struct accept_scheduler {
    // ...
    sockaddr_in _accept_addr{};
    socklen_t _accept_addr_len = sizeof(sockaddr_in);
    // handle_io_uring 中使用成员变量
};
```

---

### A3. io_uring accept_scheduler 每次 advance 都提交新 accept SQE — ring 溢出

**文件**: `src/env/scheduler/net/io_uring/scheduler.hh:105-131`

**问题**: `handle_io_uring()` 每次调用都无条件执行 `add_accept_request()`，而 `advance()` 在主循环中被反复调用。结果是 ring 中堆积大量未完成的 accept SQE，直到 ring 满（`io_uring_get_sqe` 返回 NULL 未检查）。

```cpp
auto handle_io_uring() {
    // ...
    add_accept_request(server_socket, &client_addr, &client_addr_len);  // 每次都提交
    while (::io_uring_peek_cqe(...)) { ... }  // 只消费完成的
}
```

**修复**: ring 中始终保持恰好一个 accept SQE。`init()` 提交第一个，每次 CQE 完成后立即重新提交。连接被内核接受后放入 `conn_fd_q`，`schedule(conn_req*)` 直接从队列取——无需等待、无需攒请求：

```cpp
// 成员变量: sockaddr 必须是成员（配合 A2 修复）
sockaddr_in _accept_addr{};
socklen_t _accept_addr_len = sizeof(sockaddr_in);

int init(...) {
    // ... socket, bind, listen, ring init ...
    // init 时提交第一个 accept
    submit_accept();
    return 0;
}

void submit_accept() {
    ::io_uring_sqe *sqe = ::io_uring_get_sqe(&ring);
    ::io_uring_prep_accept(sqe, server_socket,
        reinterpret_cast<sockaddr*>(&_accept_addr), &_accept_addr_len, 0);
    auto *req = new io_uring_request{uring_event_type::accept, nullptr};
    ::io_uring_sqe_set_data(sqe, req);
    ::io_uring_submit(&ring);
}

auto handle_io_uring() {
    ::io_uring_cqe *cqe;
    while (::io_uring_peek_cqe(&ring, &cqe) == 0) {
        if (cqe->res < 0) {
            // accept 失败，重新提交
            ::io_uring_cqe_seen(&ring, cqe);
            submit_accept();
            continue;
        }
        auto *uring_req = reinterpret_cast<io_uring_request*>(cqe->user_data);
        // 处理已接受的连接
        if (!maybe_accept(cqe->res)) {
            // 没有 pending conn_req → 入队等取
            const int flags = fcntl(cqe->res, F_GETFL, 0);
            fcntl(cqe->res, F_SETFL, flags | O_NONBLOCK);
            auto s = new session_t(cqe->res, new common::detail::recv_cache(_recv_buffer_size));
            if (!conn_fd_q.try_enqueue(common::session_id_t::encode(s))) {
                // 队列满，关闭连接避免泄漏
                s->close();
                delete s;
            }
        }
        delete uring_req;
        ::io_uring_cqe_seen(&ring, cqe);
        // 立即重新提交，保持 ring 中始终有一个 accept
        submit_accept();
    }
}
```

两条路径互为对偶：
- **连接先到**: CQE 完成 → `maybe_accept` 查 `request_q` 无人等 → 连接入 `conn_fd_q`。之后 `schedule(conn_req*)` 从 `conn_fd_q` 直接取。
- **请求先到**: `schedule(conn_req*)` 查 `conn_fd_q` 为空 → `conn_req` 入 `request_q`。之后 CQE 完成 → `maybe_accept` 从 `request_q` 取出 `conn_req` 直接交付。

---

### A4. net_frame 析构函数不释放内存 — 系统性内存泄漏

**文件**: `src/env/scheduler/net/common/struct.hh:67`

**问题**: `net_frame::~net_frame() = default;` 但 `_data` 是 `new char[]` 分配的。所有被丢弃的 net_frame 都会泄漏内存。

涉及场景：
- `recv_cache::release()` 手动 `delete[] opt.value()->_data`（绕过了析构）
- echo 手动 `delete[] data`（绕过了析构）
- 任何异常路径中丢弃的 net_frame 直接泄漏
- `schedule(recv_req*)` 中 `copy_out_frame` 生成的临时 frame 如果不被消费就泄漏

**根因**: net_frame 的所有权语义不明确 — 有时拥有数据（recv 路径 new），有时不拥有（send 路径借用用户指针）。

**修复方案**: 让 net_frame 明确拥有数据，析构时释放：

```cpp
~net_frame() {
    delete[] _data;
    _data = nullptr;
}
```

同时移除所有手动 delete[] 调用。send 路径需要拷贝数据或使用不同类型。

**影响范围**:
- `common/detail.hh:159` — recv_cache::release() 的手动 delete
- `echo.cc:90` — echo 中的手动 delete
- `common/detail.hh:135-137` — copy_out_frame 返回的 frame
- send 路径: `net.hh:53` 直接使用外部指针，需要改为拷贝或引用语义

---

## B. 资源泄漏

### B1. Session 对象泄漏 — 错误/EOF 路径不 delete session

**文件**:
- `epoll/scheduler.hh:211-227` (handle_read_event: EOF 和 error 分支)
- `io_uring/scheduler.hh:327-334` (close_session)
- `io_uring/scheduler.hh:340-348` (process_err: read case)

**问题**: `close_session(s)` 调用 `s->close()` 关闭 fd 和释放 cache，但不 `delete s`。Session 结构本身泄漏。

对比 epoll `handle_stop_req()` (line 183-188) 正确地执行了 `close_session(s); delete s;`。

**修复**: 所有关闭 session 的路径都需要 `delete s`：

```cpp
void close_session(session_t* s) {
    s->close();
    delete s;
}
```

注意：delete 后 session_id decode 可能返回已释放指针。需要配合 generation 检查或清零。

---

### B2. epoll_event 堆分配泄漏

**文件**:
- `epoll/scheduler.hh:158-162` (handle_join_req: `new epoll_event`)
- `epoll/scheduler.hh:416-419` (accept_scheduler::init: `new epoll_event`)

**问题**: `epoll_ctl(EPOLL_CTL_ADD)` 内部会拷贝 `epoll_event` 数据，不保留用户指针。所以 `new epoll_event` 后传给 `add_event()`，指针永远不会被释放。

**修复**: 使用栈变量替代堆分配：

```cpp
epoll_event event{};
event.data.ptr = s;
event.events = EPOLLIN | EPOLLOUT | EPOLLET;
poller.add_event(s->fd, &event);
```

---

### B3. io_uring handle_stop_request 不调用 release()

**文件**: `src/env/scheduler/net/io_uring/scheduler.hh:457-469`

**问题**:
```cpp
void handle_stop_request() {
    // ...
    s->status.store(session_status::closed);
    ::close(s->fd);
    // 没有 s->release() — recv_cache 泄漏
    // 没有 delete s — session 泄漏
}
```

对比 `internal_session::close()` 做了: `close(fd) + release() + set status`。

**修复**: 调用 `s->close()` 替代手动操作，然后 `delete s`：

```cpp
void handle_stop_request() {
    // ...
    s->close();
    delete s;
    opt.value()->cb(true);
    delete opt.value();
}
```

但需注意：如果有 pending read SQE 引用该 session，close fd 后 CQE 会返回错误，此时 session 已被 delete。需要先取消或等待 pending IO 完成。

---

### B4. Echo 不调用 stop() — 断开后会话资源不回收

**文件**: `apps/example/echo/echo.cc:93-98`

**问题**: 异常处理只设置 `closed = true`，session 循环退出后没有 stop 清理：

```cpp
>> any_exception([](std::exception_ptr e) {
    return just()
        >> get_context<session_data<session_scheduler_t>>()
        >> then([](session_data<session_scheduler_t> &sd) {
            sd.closed.store(true);
            // 没有 stop(sd.scheduler, sd.id)
        });
})
```

**影响**: epoll 注册不移除、session 对象不释放、recv_cache/send_cache 不释放。

**修复**: 在 session 循环结束后添加 stop 调用。需要在 `with_context(...)` 之后链接 stop 操作，或在异常处理中触发。

---

### B5. _recv_pkt_getter 分配 iovec 不释放 — 死代码

**文件**: `src/env/scheduler/net/common/detail.hh:51-67`

**问题**: `_recv_pkt_getter` 和 `get_recv_pkt()` 用 `new iovec[...]` 分配内存，但返回的 `pkt_iovec` 没有析构函数，也没有调用方负责释放。

全局搜索发现 `get_recv_pkt()` 在当前代码中未被调用（已被 `copy_out_frame()` 取代）。

**修复**: 删除 `_recv_pkt_getter`、`recv_pkt_getter`、`get_recv_pkt()`、`pkt_iovec` 等死代码。

---

## C. 数据正确性

### C1. epoll EPOLLET 模式下 handle_read_event 只调用一次 readv

**文件**: `src/env/scheduler/net/epoll/scheduler.hh:192-228`

**问题**: Edge-triggered 模式要求读到 EAGAIN 为止。当前代码只做一次 `readv()`：

```cpp
if (auto res = readv(s->fd, buf.iov(), iovcnt); res > 0) {
    buf.forward_tail(res);
    // 提取帧...
}
// 没有循环读到 EAGAIN
```

如果内核缓冲区数据量大于 recv buffer 的可用空间，剩余数据不会被读取。边缘触发模式下，如果没有新数据到来，epoll 不会再次通知，数据永远丢失。

**修复**: 添加读循环：

```cpp
while (true) {
    int iovcnt = buf.make_iovec();
    if (buf.available() == 0) break;  // 缓冲区满
    auto res = readv(s->fd, buf.iov(), iovcnt);
    if (res > 0) {
        buf.forward_tail(res);
        // 提取帧...
    } else if (res == 0) {
        // EOF
        break;
    } else {
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        // error
        break;
    }
}
```

---

### C2. epoll handle_write_event 不处理 partial write

**文件**: `src/env/scheduler/net/epoll/scheduler.hh:232-251`

**问题**: `writev()` 可能只写入部分数据（partial write），但代码不检查写入量：

```cpp
auto res = writev(s->fd, r->_send_vec, r->_send_cnt);
if (res < 0) { ... error ... }
r->cb(true);  // 假设全部写入成功
```

Non-blocking socket 的 writev 返回值可能小于请求的总长度。剩余数据被静默丢弃。

**修复**: 检查返回值，对 partial write 进行重试或缓存剩余数据。

---

### C3. io_uring on_write_event 不检查 CQE 结果

**文件**: `src/env/scheduler/net/io_uring/scheduler.hh:319-324`

**问题**:
```cpp
void on_write_event(const io_uring_request *iur) {
    auto r = static_cast<common::send_req*>(iur->user_data);
    r->cb(true);   // 无条件报告成功
    delete r;
}
```

`process_cqe` 只在 `cqe->res >= 0` 时调用 `on_write_event`，但 `res >= 0` 不代表全部写入。`res` 可能小于请求的总长度（short write），也可能是 0。

**修复**: 将 cqe->res 传入 on_write_event 并校验：

```cpp
void on_write_event(const io_uring_request *iur, int res) {
    auto r = static_cast<common::send_req*>(iur->user_data);
    size_t expected = r->_send_vec[0].iov_len + r->_send_vec[1].iov_len;
    r->cb(static_cast<size_t>(res) == expected);
    delete r;
}
```

---

## D. 设计问题

### D1. net_frame 所有权语义不明确

**现状**:
- recv 路径: `copy_out_frame` 用 `new char[]` 创建数据 → frame 拥有数据
- send 路径: `net.hh::send()` 直接使用外部指针 → frame 不拥有数据
- 析构: `= default` → 永不释放
- 清理方式: 调用方自行 `delete[]`（echo.cc:90, detail.hh:159）

**问题**: 同一类型既可拥有数据也可借用数据，谁负责释放完全靠约定。任何异常路径、新增使用场景都可能导致泄漏或 double-free。

**建议**: 分离拥有和借用语义：
- 方案 A: net_frame 始终拥有数据（析构释放），send 路径拷贝
- 方案 B: 两种类型 — `owned_frame`（析构释放）和 `frame_view`（只引用）
- 方案 C: 使用 `std::unique_ptr<char[]>` 或 `std::vector<char>` 管理数据

---

### D2. recv 调度直接访问 session 数据 — 隐含单线程假设 ✅ 已修复

**文件**: epoll/scheduler.hh, io_uring/scheduler.hh

**问题**: `schedule(recv_req*)` 通过 `copy_out_frame` 操作 `packet_buffer._head`（消费者端），而 advance 线程的 `handle_read_event`/`on_read_event` 也调用 `copy_out_frame`。如果 schedule 调用者与 advance 线程不同，`_head` 的 load-add-store 会竞争（SPSC 不变量被打破）。

**修复**: 从 `schedule(recv_req*)` 中移除 `copy_out_frame` 调用。schedule 只走两条路径：
1. `ready_q.try_dequeue()` — SPSC consumer 角色
2. `recv_q.try_enqueue(req)` — SPSC producer 角色

`packet_buffer` 的消费者（`_head` 写者）现在只有 advance 线程。advance 的 `copy_out_frame` 循环已提取所有完整帧放入 `ready_q`，schedule 的 `copy_out_frame` 本就几乎不可能命中，移除无性能损失。

---

### D3. packet_buffer 假设 size 为 2 的幂但不校验

**文件**: `src/env/scheduler/net/common/struct.hh:111,117`

```cpp
size_t head() const { return _head.load(...) & (_size - 1); }
size_t tail() const { return _tail.load(...) & (_size - 1); }
```

`& (_size - 1)` 只在 `_size` 是 2 的幂时等价于 `% _size`。构造函数不做任何校验。

**修复**: 在构造函数中添加断言：

```cpp
explicit packet_buffer(const uint32_t size)
    : _data(new char[size]), _size(size), _head(0), _tail(0), _iov{} {
    assert((size & (size - 1)) == 0 && "packet_buffer size must be power of 2");
}
```

---

### D4. session_id_t generation 只有 16 位 — ABA 风险（by design，风险可接受）

**文件**: `src/env/scheduler/net/common/struct.hh:196-227`

16 位 generation 在 65536 次 session 创建后回绕。ABA 触发需要同时满足：
1. generation 恰好回绕到相同值（间隔 65536 次创建）
2. `new session_t(...)` 返回与旧 session 完全相同的内存地址

两者同时命中的概率极低。x86-64 用户空间地址需要 48 位，留给 generation 的只有 16 位，这是 64 位编码下的最优折中。扩大 generation 需要压缩指针位数（限制地址空间）或引入 map 查表（增加开销），均不符合零查表、无锁的设计目标。

**结论**: 当前设计合理，不修改。

---

### D5. connect_req 存储原始 const char* — 悬空指针风险

**文件**: `src/env/scheduler/net/common/struct.hh:243-248`

```cpp
struct connect_req {
    const char* address;
    uint16_t port;
    // ...
};
```

如果传入的地址字符串是临时的或来自栈上缓冲区，在异步处理时指针已悬空。

**修复**: 改为 `std::string` 或在构造时拷贝。

---

### D6. reader 结构体 — 遗留死代码

**文件**: `src/env/scheduler/net/common/detail.hh:250-280`

`reader` 结构体和相关的 `packet_recv_status` 枚举不被任何活跃代码引用（已被 `recv_cache` + `packet_buffer` 取代）。

**修复**: 删除。

---

### D7. _frame_copier 参数类型 uint16_t 与 handle_data 传入的 size_t 不匹配

**文件**: `src/env/scheduler/net/common/detail.hh:98-120`

```cpp
net_frame operator()(const char* data, const uint16_t len) const noexcept { ... }
```

`handle_data` 以 `size_t` 调用 functor。隐式 `size_t → uint16_t` 缩窄转换在大多数编译器下会 warning。虽然帧长度不超过 uint16_t 范围，但类型应统一。

**修复**: 参数改为 `size_t`。

---

## E. 实施计划

### Phase 1: 修复严重 BUG（安全/正确性） ✅ 已完成

| 步骤 | 文件 | 内容 | 状态 |
|------|------|------|------|
| 1 | `net.hh` | 修复 send() 长度双重叠加 (A1) | ✅ |
| 2+3 | `io_uring/scheduler.hh` | 修复 accept: sockaddr 改为成员变量 (A2)；init 提交首个 accept，CQE 完成后重新提交，保持 ring 中恰好一个 (A3) | ✅ |
| 4 | `common/struct.hh` | net_frame 析构释放内存，添加 release() (A4) | ✅ |
| 5 | `echo.cc` + `echo_client.cc` + `detail.hh` | 适配 net_frame 新所有权 — send 用 release() 转移，移除所有手动 delete (A4 关联) | ✅ |

### Phase 2: 修复资源泄漏 ✅ 已完成

| 步骤 | 文件 | 内容 | 状态 |
|------|------|------|------|
| 6 | `epoll/scheduler.hh`, `io_uring/scheduler.hh`, `detail.hh` | close_session 加 delete；close() 幂等化(fd>=0检查)；epoll handle_events null ptr 防 write 回 use-after-free；io_uring write error 不再 close session (B1) | ✅ |
| 7 | `epoll/scheduler.hh` | epoll_event 堆分配改栈变量 (B2) | ✅ |
| 8 | `io_uring/scheduler.hh` | handle_stop_request 调用 s->close() 释放 recv_cache，pending read CQE handler 负责 delete (B3) | ✅ |
| 9 | `echo.cc` | session_proc 末尾链接 stop sender 清理 scheduler 资源 (B4) | ✅ |
| 10 | `struct.hh`, `detail.hh` | 删除 packet, pkt_iovec, _recv_pkt_getter, get_recv_pkt, pkt_vec, reader, packet_recv_status (B5, D6) | ✅ |

### Phase 3: 修复数据正确性 ✅ 已完成

| 步骤 | 文件 | 内容 | 状态 |
|------|------|------|------|
| 11 | `epoll/scheduler.hh` | handle_read_event 循环读到 EAGAIN — EPOLLET 要求读到 EAGAIN 才停止 (C1) | ✅ |
| 12 | `epoll/scheduler.hh` | handle_write_event 检查 writev 返回值 vs 预期总长度，partial write 报 cb(false) (C2) | ✅ |
| 13 | `io_uring/scheduler.hh` | on_write_event 接收 cqe->res 参数，校验 vs 预期写入量 (C3) | ✅ |

### Phase 4: 设计改进 ✅ 已完成

| 步骤 | 文件 | 内容 | 状态 |
|------|------|------|------|
| 14 | `common/struct.hh` | packet_buffer 构造函数 assert 校验 size 为 2 的幂 (D3) | ✅ |
| 15 | `common/detail.hh` | _frame_copier 参数类型 uint16_t → size_t，消除隐式缩窄 (D7) | ✅ |
| 16 | `common/struct.hh`, `connect_scheduler.hh` (epoll+io_uring) | connect_req address 改为 std::string，调用点加 .c_str() (D5) | ✅ |
| 17 | `epoll/scheduler.hh`, `io_uring/scheduler.hh` | schedule(recv_req*) 移除 copy_out_frame 调用，消除 packet_buffer 双消费者竞争 (D2) | ✅ |
| 18 | — | session_id_t generation 位宽 (D4) — by design，不修改 | ✅ |

### 依赖关系

- Step 4 (net_frame 析构) 和 Step 5 (send 适配) 必须同时进行
- Step 6 (delete session) 需要审计所有 session 引用路径，确认无 use-after-free
- Step 8 依赖 Step 6 的 close_session 策略
- Step 9 (echo stop) 依赖 Step 8 (stop 实际能正确工作)
