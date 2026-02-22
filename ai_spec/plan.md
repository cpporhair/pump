# scheduler/net 模块修复开发计划

> 基于 [command.md](./command.md) 问题清单制定  
> 涉及文件目录：`src/env/scheduler/net/`

---

## 开发原则

- **每个阶段完成后需编译验证**，确保不引入新的编译错误
- **修改顺序**：先修底层数据结构（common），再修 senders，再修 scheduler 实现，最后修 API 层和架构
- **依赖关系**：阶段二依赖阶段一的 session 统一；阶段三依赖阶段一、二的基础修复
- **测试**：每个阶段结束后运行现有测试，确保无回归

---

## 阶段一：common 层基础修复（common/struct.hh + common/detail.hh）

> 目标：修复底层数据结构的内存错误、逻辑错误，为上层 scheduler 和 sender 提供正确的基础设施。

### 1.1 packet::clear() 内存释放错误
- **文件**：`common/struct.hh`
- **问题**：`delete data` 应为 `delete[] data`
- **修改**：将 `delete data` 改为 `delete[] data`
- **验证**：编译通过

### 1.2 packet_buffer 改造为 SPSC ring buffer
- **文件**：`common/struct.hh`
- **问题**：`_head` 和 `_tail` 使用 `volatile` 而非 `atomic`，且 `packet_buffer` 的并发模型未明确定义
- **设计决策**：`packet_buffer` 是一个 **SPSC（Single-Producer Single-Consumer）ring buffer**：
  - **生产者**只有一个（网络 IO 写入端）
  - **消费者**在同一时刻只有一个（数据处理/读取端）
  - 因此不需要多生产者或多消费者的同步机制
- **修改**：
  - 将 `_head` 和 `_tail` 从 `volatile size_t` 改为 `std::atomic<size_t>`，使用 SPSC 适用的内存序：
    - 生产者更新 `_tail` 时使用 `std::memory_order_release`
    - 消费者读取 `_tail` 时使用 `std::memory_order_acquire`
    - 消费者更新 `_head` 时使用 `std::memory_order_release`
    - 生产者读取 `_head` 时使用 `std::memory_order_acquire`
  - 在类注释中明确标注 SPSC 语义，说明生产者/消费者的角色约定
- **验证**：编译通过，检查所有引用点

### 1.3 packet_buffer::make_iovec() 环形缓冲区回绕未处理
- **文件**：`common/struct.hh`
- **问题**：只填充了 `vec[0]`，未处理回绕到开头的 `vec[1]`
- **修改**：
  - 计算 tail 到缓冲区末尾的距离 `first_len = _size - _tail`
  - 若 `available() > first_len`，需要填充 `vec[1]` 为缓冲区开头到剩余长度
  - `vec[0].iov_len = min(available(), first_len)`
  - `vec[1].iov_base = _data; vec[1].iov_len = available() - first_len`（仅当回绕时）
  - 返回实际使用的 iovec 数量（1 或 2）
- **验证**：编译通过

### 1.4 packet_buffer::make_iovec() 堆分配改栈/成员分配
- **文件**：`common/struct.hh`
- **问题**：每次调用 `new iovec[2]`，低延迟场景不可接受
- **修改**：
  - 在 `packet_buffer` 中添加 `iovec _iov[2]` 成员变量
  - `make_iovec()` 返回 `_iov` 指针，不再每次 `new`
  - 同步修改所有调用 `make_iovec()` 的地方，移除对应的 `delete[]` 调用
- **验证**：编译通过，确认无内存泄漏

### 1.5 packet_buffer 添加析构函数
- **文件**：`common/struct.hh`
- **问题**：构造函数 `new char[_size]` 但无析构函数释放
- **修改**：添加 `~packet_buffer() { delete[] _data; }`
- **验证**：编译通过

### 1.6 handle_data 回绕分支参数错误
- **文件**：`common/struct.hh`
- **问题**：回绕分支第四个参数应为回绕部分的长度
- **修改**：将 `(start + len)` 改为 `len - (size() - head())` 或等价的正确计算
- **验证**：编译通过

### 1.7 has_full_pkt() / get_recv_pkt() 逻辑反转
- **文件**：`common/detail.hh`
- **问题**：`if (len != 0xffff)` 应为 `if (len == 0xffff)`
- **修改**：修正两个函数中的条件判断
- **验证**：编译通过

### 1.8 _read_pkt_len 跨缓冲区读取溢出
- **文件**：`common/detail.hh`
- **问题**：`need[2]` 只有2字节，`memcpy` 偏移错误导致溢出
- **修改**：
  - 确认 pkt_len 的字节数（假设2字节），修正 `memcpy` 偏移为 `&need[l1]` 而非 `&need[1]`
  - 或使用更安全的实现方式
- **验证**：编译通过

### 1.9 recv_cache::release() 实现
- **文件**：`common/detail.hh`
- **问题**：函数体为空
- **修改**：实现资源释放逻辑，清理 buf 和 pending 的 req
- **验证**：编译通过

### 1.10 统一 session 抽象（关键架构修改）
- **文件**：`common/detail.hh`
- **问题**：`internal_session` 和 `session<reader_t>` 两套设计并存，职责重叠
- **修改**：
  - 保留 `internal_session` 作为唯一的 session 实现
  - 移除或重构 `session<reader_t>`
  - 更新所有引用点（epoll/io_uring scheduler 中的类型转换）
- **依赖**：此修改影响阶段三的 scheduler 修复
- **验证**：编译通过

### 1.11 session::clear() 调用修正
- **文件**：`common/detail.hh`
- **问题**：对 `unique_ptr` 调用不存在的 `clear()` 方法，`release()` 后指针泄漏
- **修改**：改为正确的清理逻辑（如 `rdr.reset()` 或直接析构）
- **验证**：编译通过

---

## 阶段二：Senders 层修复（senders/conn.hh, recv.hh, send.hh, join.hh, stop.hh）

> 目标：修复所有 sender 的错误通知、类型不一致、内存泄漏问题。

### 2.1 定义 net 模块专用异常类型
- **文件**：新建 `common/error.hh` 或在 `common/struct.hh` 中添加
- **问题**：所有 sender 使用无意义的 `logic_error("")` 作为错误通知
- **修改**：
  - 定义异常类型层次：`net_error`（基类）、`session_closed_error`、`duplicate_recv_error`、`enqueue_failed_error` 等
  - 替换所有 sender 中的 `std::logic_error("")` 为对应的具体异常类型
- **涉及文件**：`conn.hh`、`recv.hh`、`send.hh`、`join.hh`、`stop.hh`
- **验证**：编译通过

### 2.2 send sender cnt 类型统一
- **文件**：`net.hh`、`senders/send.hh`
- **问题**：`net.hh` 中为 `size_t`，内部为 `int`
- **修改**：统一为 `size_t`（无符号，语义更正确）
- **验证**：编译通过

### 2.3 stop sender compute_sender_type 语义修正
- **文件**：`senders/stop.hh`
- **问题**：`count_value()` 返回 1 + `bool` 输出，但 stop 不产生有意义的值
- **修改**：
  - `count_value()` 改为返回 0
  - 移除 `get_value_type_identity` 中的 `bool` 返回
  - 与 `join` sender 保持一致
- **验证**：编译通过，检查上游是否依赖 stop 的输出值

### 2.4 sender op 的 request 泄漏处理
- **文件**：所有 sender 的 `op::start` 方法
- **问题**：`schedule()` 的 enqueue 失败时 request 泄漏
- **修改**：
  - 方案 A：让 `schedule()` 返回成功/失败状态，失败时 `op::start` 负责 `delete req` 并回调错误
  - 方案 B：让 `schedule()` 在 enqueue 失败时自行 `delete req` 并回调错误
  - 选择方案后统一修改所有 sender
- **验证**：编译通过

---

## 阶段三：epoll/scheduler.hh 修复

> 目标：修复 epoll scheduler 的编译错误、逻辑错误、资源管理问题。

### 3.1 移除错误的 liburing.h 包含
- **文件**：`epoll/scheduler.hh`
- **问题**：epoll scheduler 不应依赖 `<liburing.h>`
- **修改**：移除 `#include <liburing.h>`，替换为所需的正确头文件
- **验证**：编译通过

### 3.2 handle_join_req / handle_stop_req session 类型修正
- **文件**：`epoll/scheduler.hh`
- **问题**：错误转换为 `session<reader>*`
- **修改**：使用 `session_t`（即 `internal_session<recv_cache, epoll_send_cache>`）
- **依赖**：阶段一 1.10 session 统一
- **验证**：编译通过

### 3.3 handle_read_event readv 调用修正
- **文件**：`epoll/scheduler.hh`
- **问题**：`readv` 传了4个参数，多了一个 `0`
- **修改**：移除第4个参数，改为 `readv(s->fd, iov, iovcnt)`
- **验证**：编译通过

### 3.4 handle_read_event 读取后更新 tail
- **文件**：`epoll/scheduler.hh`
- **问题**：读取数据后缺少 `forward_tail(res)` 调用
- **修改**：在 `readv` 成功返回后添加 `buf.forward_tail(res)`
- **验证**：编译通过

### 3.5 handle_write_event 批量处理 + 返回值检查
- **文件**：`epoll/scheduler.hh`
- **问题**：每次写事件只处理一个请求，且未检查 writev 返回值
- **修改**：
  - 循环处理队列中的所有待发送请求（或直到 EAGAIN）
  - 检查 `writev` 返回值：处理部分写入（更新 iov 偏移）和错误情况
- **验证**：编译通过

### 3.6 accept_scheduler 服务端 socket 初始化
- **文件**：`epoll/scheduler.hh`
- **问题**：缺少 socket 创建、bind、listen 逻辑
- **修改**：
  - 添加初始化接口（或构造参数）：`init(address, port)` 或类似
  - 实现 socket → bind → listen → epoll_ctl 注册
- **验证**：编译通过

### 3.7 handle_stop_req close_session 调用修正
- **文件**：`epoll/scheduler.hh`
- **问题**：类型错误 + 重复 del_event
- **修改**：
  - 传入正确的参数类型
  - 整理 close_session 流程，避免重复操作
- **验证**：编译通过

### 3.8 schedule(recv_req) 异常路径添加 return
- **文件**：`epoll/scheduler.hh`
- **问题**：异常回调后未 return
- **修改**：在 `req->cb(exception)` 后添加 `delete req; return;`
- **验证**：编译通过

### 3.9 epoll events 数组可配置化 + 析构释放
- **文件**：`epoll/scheduler.hh`
- **问题**：硬编码 16，无析构释放
- **修改**：
  - 定义常量或模板参数 `MAX_EVENTS`
  - 在析构函数中 `delete[] events`
  - 或改用 `std::array<epoll_event, MAX_EVENTS>` 避免堆分配
- **验证**：编译通过

### 3.10 close_session 逻辑梳理
- **文件**：`epoll/scheduler.hh`
- **问题**：events 数组元素设置 ptr = nullptr 后 delete，逻辑混乱
- **修改**：理清 epoll_event 生命周期管理，确保 close_session 正确清理
- **验证**：编译通过

---

## 阶段四：io_uring/scheduler.hh 修复

> 目标：修复 io_uring scheduler 的初始化缺失、内存错误、逻辑遗漏等问题。

### 4.1 accept_scheduler 服务端 socket + ring 初始化
- **文件**：`io_uring/scheduler.hh`
- **问题**：缺少 socket 创建/bind/listen 和 `io_uring_queue_init`
- **修改**：
  - 添加 socket 初始化流程（与 epoll 类似）
  - 在构造函数或 `init()` 中调用 `io_uring_queue_init(queue_depth, &ring, 0)`
  - 析构中调用 `io_uring_queue_exit(&ring)`
- **验证**：编译通过

### 4.2 accept_scheduler::schedule 中 req 泄漏
- **文件**：`io_uring/scheduler.hh`
- **问题**：有现成连接时回调后未 `delete req`
- **修改**：在 `req->cb(opt.value())` 后添加 `delete req`
- **验证**：编译通过

### 4.3 accept_scheduler::handle_io_uring switch case 添加 break
- **文件**：`io_uring/scheduler.hh`
- **问题**：`case accept` 缺少 break，导致 fallthrough + double-free
- **修改**：添加 `break` 语句
- **验证**：编译通过

### 4.4 new/free 混用修正
- **文件**：`io_uring/scheduler.hh`
- **问题**：`new` 分配但 `free()` 释放
- **修改**：统一为 `new/delete`
- **验证**：编译通过

### 4.5 accept_scheduler::advance 接口统一
- **文件**：`io_uring/scheduler.hh`
- **问题**：与 epoll 版本签名不一致
- **修改**：统一 `advance` 接口签名（需确认哪个是正确的）
- **依赖**：可能需要同步修改 epoll 版本
- **验证**：编译通过

### 4.6 session_scheduler ring 初始化
- **文件**：`io_uring/scheduler.hh`
- **问题**：缺少 `io_uring_queue_init`
- **修改**：在构造函数或 `init()` 中初始化 ring，析构中退出 ring
- **验证**：编译通过

### 4.7 session_scheduler::schedule(recv_req) 异常路径添加 return
- **文件**：`io_uring/scheduler.hh`
- **问题**：与 epoll 同样的缺少 return 问题
- **修改**：异常回调后 `delete req; return;`
- **验证**：编译通过

### 4.8 session_scheduler::schedule(stop_req) 改为异步处理
- **文件**：`io_uring/scheduler.hh`
- **问题**：同步执行违反 schedule/advance 模式
- **修改**：
  - `schedule` 只将 stop_req 入队
  - 在 `advance` 中处理 stop 请求（关闭 fd、回调）
- **验证**：编译通过

### 4.9 handle_join_request 调用 join 回调
- **文件**：`io_uring/scheduler.hh`
- **问题**：直接 delete req 未调用 `req->cb(true)`
- **修改**：在 delete 前调用 `req->cb(true)`（或适当的完成值）
- **验证**：编译通过

### 4.10 process_err write case 添加 break + delete
- **文件**：`io_uring/scheduler.hh`
- **问题**：缺少 break，导致 fallthrough；缺少 delete uring_req
- **修改**：添加 `delete uring_req; break;`
- **验证**：编译通过

### 4.11 submit_read 中 iovec 泄漏修复
- **文件**：`io_uring/scheduler.hh`
- **问题**：`make_iovec()` 分配的内存未释放
- **修改**：配合阶段一 1.4 的修改（iovec 改为成员变量后自然解决）
- **依赖**：阶段一 1.4
- **验证**：编译通过

### 4.12 handle_send_request 添加 io_uring_submit
- **文件**：`io_uring/scheduler.hh`
- **问题**：准备了 SQE 但未提交
- **修改**：在处理完所有 send 请求后调用 `io_uring_submit(&ring)`
- **验证**：编译通过

### 4.13 send 请求队列改为环形缓冲区
- **文件**：`io_uring/scheduler.hh`
- **问题**：`std::list` 频繁堆分配
- **修改**：替换为固定容量的环形缓冲区（可复用 `packet_buffer` 的思路或使用 `moodycamel` 队列）
- **验证**：编译通过，性能无回归

---

## 阶段五：API 层修复（net.hh）

> 目标：补充 API 一致性，修复接口安全性问题。

### 5.1 补充 flat_map 风格重载
- **文件**：`net.hh`
- **问题**：`recv`、`send`、`join`、`stop` 缺少无参的 flat_map 版本
- **修改**：
  - 为 `recv`、`send`、`join`、`stop` 各添加无参重载，从 context 获取 scheduler
  - 参考 `wait_connection()` 的实现模式
- **验证**：编译通过

### 5.2 send 接口裸指针改安全封装
- **文件**：`net.hh`
- **问题**：`iovec*` 裸指针，生命周期管理不安全
- **修改**：
  - 设计安全的数据缓冲区抽象（如 `send_buffer`、`buffer_view`）
  - 或在 send sender 内部拷贝数据到内部缓冲区
  - 确保异步操作期间数据有效
- **验证**：编译通过

---

## 阶段六：整体架构优化

> 目标：统一两套 scheduler 的抽象，完善生命周期管理。

### 6.1 统一 epoll/io_uring session 模型
- **文件**：`common/detail.hh`、`epoll/scheduler.hh`、`io_uring/scheduler.hh`
- **问题**：两套 session 模板参数不同
- **修改**：
  - 设计统一的 session 模板（包含 recv 和 send 缓存）
  - epoll 和 io_uring 各自特化所需的 send_cache 类型
- **依赖**：阶段一 1.10
- **验证**：编译通过

### 6.2 scheduler 初始化/销毁接口
- **文件**：`epoll/scheduler.hh`、`io_uring/scheduler.hh`
- **问题**：缺少完整的构造/初始化/析构
- **修改**：
  - 定义统一的初始化配置结构体（bind address、port、ring size、buffer size 等）
  - 实现 RAII 风格的构造/析构
  - accept_scheduler 和 session_scheduler 都需要完整的初始化流程
- **验证**：编译通过

### 6.3 session_id 改为 tagged handle
- **文件**：`common/struct.hh`、`common/detail.hh` 及所有引用处
- **问题**：裸 `uint64_t` session_id 缺乏类型安全，存在 use-after-free 风险
- **修改**：
  - 新建 `session_id_t` 类型，封装编码/解码逻辑
  - 利用 x86-64 低 48 位存指针，高 16 位存 generation
  - session 对象添加 `uint16_t generation` 字段
  - scheduler 使用 session_id 时验证 generation
  - 替换所有 `uint64_t` session_id 为 `session_id_t`
- **验证**：编译通过，确认类型安全

### 6.4 优雅关闭机制
- **文件**：所有 scheduler 文件
- **问题**：无 shutdown 流程，session 关闭不通知 pending 请求
- **修改**：
  - scheduler 添加 `shutdown()` 方法
  - shutdown 时：停止接受新连接、关闭所有 session、通知所有 pending 的 recv/send 请求（回调错误）
  - session 关闭时遍历 pending 请求列表，逐一回调错误
- **验证**：编译通过

---

## 执行顺序总结

```
阶段一（common 层） → 阶段二（senders 层） → 阶段三（epoll） → 阶段四（io_uring） → 阶段五（API 层） → 阶段六（架构优化）
```

### 关键依赖链
- 1.4（iovec 成员化）→ 4.11（iovec 泄漏自动解决）
- 1.10（session 统一）→ 3.2（epoll session 类型）→ 6.1（统一 session 模型）
- 2.1（异常类型）→ 所有 sender/scheduler 中的错误通知
- 阶段三、四的 socket 初始化（3.6, 4.1）→ 阶段六的初始化接口统一（6.2）

### 估计工作量
| 阶段 | 预估复杂度 | 说明 |
|------|-----------|------|
| 阶段一 | ⭐⭐⭐ | 修改量大，session 统一是关键路径 |
| 阶段二 | ⭐⭐ | 定义异常类型 + 统一替换 |
| 阶段三 | ⭐⭐⭐ | 逻辑修复较多，需要理解 epoll 事件循环 |
| 阶段四 | ⭐⭐⭐⭐ | 问题最多，io_uring 接口复杂 |
| 阶段五 | ⭐⭐ | API 设计 + 实现 |
| 阶段六 | ⭐⭐⭐⭐ | 架构级重构，影响面广 |
