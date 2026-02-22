# scheduler/net 代码审核：问题清单与修改计划

## 一、common/struct.hh

### 1.1 packet::clear() 内存释放错误
- `delete data` 应为 `delete[] data`，`data` 是 `char*` 数组

### 1.2 packet_buffer 使用 volatile 而非 atomic
- `_head` 和 `_tail` 声明为 `volatile size_t`，在多线程场景下不提供正确的内存序保证
- 需要改为 `std::atomic<size_t>` 或在单线程场景下去掉 `volatile`

### 1.3 packet_buffer::make_iovec() 只填充了 vec[0]
- 分配了 `iovec[2]`，但只设置了 `vec[0]`（tail到末尾的部分），未处理环形缓冲区回绕到开头的 `vec[1]`
- 且 `vec[0].iov_len` 设置为 `available()`（剩余可写空间），未处理回绕时需要拆分为两段的情况

### 1.4 packet_buffer::make_iovec() 每次调用都 new iovec[2]
- 每次读事件都会分配堆内存，对低延迟系统不可接受
- 需要将 iovec 作为成员变量预分配，或使用栈上分配

### 1.5 packet_buffer 缺少析构函数
- `_data` 在构造函数中 `new char[_size]`，但没有析构函数释放
- 需要添加析构函数 `delete[] _data`

### 1.6 handle_data 回绕分支参数错误
- `handle_data(start, len, f)` 第三个分支调用 `f(data(), size() - head(), _data, (start + len))`，最后一个参数应该是回绕部分的长度，而非 `start + len`
- 正确应为 `len - (size() - head())` 或类似计算

## 二、common/detail.hh

### 2.1 has_full_pkt() 逻辑反转
- `if (len != 0xffff) return false`：当 `_read_pkt_len` 返回 `0xffff` 表示数据不足（无参重载），此处条件判断反了
- 应为 `if (len == 0xffff) return false`

### 2.2 get_recv_pkt() 同样逻辑反转
- 与 has_full_pkt 同样的问题，`if (len != 0xffff)` 应为 `if (len == 0xffff)`

### 2.3 recv_cache::release() 空实现
- 函数体为空，需要实现资源释放逻辑（清理 buf、pending 的 req 等）

### 2.4 两套 session 抽象共存
- `internal_session<impl_t...>` 和 `session<reader_t>` 两种设计并存，职责重叠
- epoll scheduler 的 handle_join_req / handle_stop_req 错误地将 session_id 转换为 `session<reader>`，但实际创建的是 `internal_session`
- 需要统一为一套 session 抽象

### 2.5 session::clear() 调用错误
- `rdr.clear()` 后又 `rdr.release()`——对 `unique_ptr` 而言，没有 `clear()` 方法（这是 reader 的方法），且 release 后指针泄漏
- 需要修正清理逻辑

### 2.6 _read_pkt_len 跨缓冲区读取实现错误
- 三参数重载中 `uint08_t need[2]` 只有2字节，但 `memcpy(&need[0], d1, l1)` 和 `memcpy(&need[1], d2, l2)` 中 `&need[1]` 的偏移只有1字节，当 l1 > 1 时会溢出

## 三、senders（conn / recv / send / join / stop）

### 3.1 所有 sender 的错误通知使用占位 logic_error
- `std::make_exception_ptr(std::logic_error(""))` 作为错误信息无诊断价值
- 需要定义 net 模块专用的异常类型，携带有意义的错误信息（如 session_closed、duplicate_recv 等）

### 3.2 send sender 的 cnt 参数类型不一致
- net.hh 中 `send()` 接口参数为 `size_t cnt`，但 send_req / send op 内部使用 `int cnt`
- 需要统一类型

### 3.3 stop sender 的 compute_sender_type 语义不一致
- stop 操作成功不产生有意义的值，但 `count_value()` 返回 1，`get_value_type_identity` 返回 `bool`
- join sender 的 `count_value()` 返回 0（无输出值），两者语义应一致——都是"加入/离开会话"，不产生数据

### 3.4 所有 sender op 的 new request 可能泄漏
- `op::start` 中 `new common::xxx_req{...}` 后调用 `scheduler->schedule()`，如果 schedule 的 enqueue 失败（队列满），request 会泄漏且回调永远不被调用
- 需要处理 enqueue 失败的情况

## 四、epoll/scheduler.hh

### 4.1 错误引入 liburing.h
- epoll scheduler 包含了 `<liburing.h>`，这是 io_uring 的头文件，epoll 实现不应依赖它

### 4.2 handle_join_req / handle_stop_req 使用错误的 session 类型
- 将 `session_id` 转换为 `session<reader>*`，但 accept_scheduler 创建的是 `internal_session<recv_cache, epoll_send_cache>`（即 `session_t`）
- 需要统一使用 `session_t`

### 4.3 handle_read_event 中 readv 调用签名错误
- 调用 `readv(s->fd, ..., 2, 0)` 传了4个参数，但 POSIX `readv` 只接受3个参数 `(fd, iov, iovcnt)`
- 第4个参数 `0` 是多余的（可能混淆了 preadv）

### 4.4 handle_read_event 读取后未更新 packet_buffer 的 tail
- 读取数据后需要调用 `forward_tail(res)` 更新写入位置，当前缺失

### 4.5 handle_write_event 只处理一个请求
- 每次写事件只从队列取一个 send_req，如果有多个待发送请求会积压
- 且未检查 writev 返回值（部分写入、错误等）

### 4.6 accept_scheduler 缺少服务端 socket 初始化
- 没有创建监听 socket、bind、listen 的逻辑
- 没有将监听 socket 注册到 epoll
- 需要提供初始化接口或构造参数

### 4.7 handle_stop_req 中 close_session 调用错误
- `close_session` 方法接受 `epoll_event*` 参数，但传入的是 `session<reader>*`（类型错误）
- 并且在 close_session 之前已经 del_event + delete event，close_session 内部又会 del_event，重复操作

### 4.8 schedule(recv_req) 异常路径缺少 return
- 当 session 状态非 normal 时，调用了 `req->cb(exception)` 但没有 return，继续执行后面的 has_full_pkt 检查
- 需要在异常回调后 `delete req; return;`

### 4.9 epoll events 数组大小硬编码为 16
- `new epoll_event[16]` 硬编码且无析构释放
- 需要可配置化或至少定义为常量，并在析构中释放

### 4.10 close_session 中对 events 数组元素设置 ptr = nullptr
- `close_session` 接收的是 events 数组中的指针，设置 `e->data.ptr = nullptr` 后又 `delete e`，但 e 是 new 出来的 epoll_event 而非数组元素，逻辑混乱

## 五、io_uring/scheduler.hh

### 5.1 accept_scheduler 缺少服务端 socket 初始化
- 与 epoll 同样问题：没有 socket 创建、bind、listen 逻辑
- io_uring ring 没有初始化（缺少 `io_uring_queue_init`）

### 5.2 accept_scheduler::schedule 中未 delete req
- 当有现成连接时调用 `req->cb(opt.value())` 后没有 `delete req`，内存泄漏

### 5.3 accept_scheduler::handle_io_uring 中 switch case 缺少 break
- `case uring_event_type::accept` 没有 break，会 fallthrough 到 default
- 且 accept 分支有条件 delete uring_req，fallthrough 后又无条件 `free(uring_req)`，导致 double-free 或 delete+free 混用

### 5.4 accept_scheduler 中 new/free 混用
- `io_uring_request` 用 `new` 分配，但用 `free()` 释放，属于未定义行为
- 需要统一为 `new/delete`

### 5.5 accept_scheduler::advance 签名与 epoll 不一致
- io_uring 版本为 `advance(const runtime_t& rt)`，epoll 版本为 `advance()` 无参
- 需要统一 scheduler 的 advance 接口

### 5.6 session_scheduler 缺少 io_uring ring 初始化
- 没有 `io_uring_queue_init` 调用，ring 成员只是零初始化

### 5.7 session_scheduler::schedule(recv_req) 异常路径缺少 return
- 与 epoll 同样问题：session 状态非 normal 时回调后未 return，继续执行后续逻辑

### 5.8 session_scheduler::schedule(stop_req) 同步执行
- 直接在 schedule 调用中关闭 fd 和回调，没有通过队列异步处理
- 这违反了"schedule 只入队，advance 处理"的模式，且可能在非 scheduler 线程上执行

### 5.9 handle_join_request 未调用 join 回调
- 直接 delete req 而未调用 `req->cb(true)`，调用方永远收不到 join 完成的通知

### 5.10 process_err 中 write case 缺少 break
- `case uring_event_type::write` 没有 break，fallthrough 到 default
- 且 write case 中 delete uring_req 缺失

### 5.11 submit_read 中 make_iovec 内存泄漏
- 每次 submit_read 都调用 `recv_cache(s)->buf.make_iovec()` 分配新的 iovec 数组
- io_uring 异步操作完成后没有释放这些 iovec

### 5.12 handle_send_request 未调用 io_uring_submit
- 准备了 SQE 但没有提交，写请求不会被内核处理
- 需要在循环结束后调用 `io_uring_submit(&ring)`

### 5.13 session_scheduler 使用 std::list 管理溢出的 send 请求
- `std::list<common::send_req*>` 会频繁堆分配，不符合低延迟设计
- 考虑使用固定容量的环形缓冲区

## 六、net.hh（API 层）

### 6.1 缺少 flat_map 风格的重载
- `wait_connection()` 有无参的 flat_map 版本，但 `recv`、`send`、`join`、`stop` 没有
- 需要补充一致的 API 风格，支持从 context 或上游值中获取 scheduler

### 6.2 send 接口直接传递裸指针
- `send(scheduler, sid, iovec*, size_t)` 传递裸指针，调用方需要管理 iovec 生命周期
- 容易导致悬挂指针（异步操作期间 iovec 可能被释放）

## 七、整体架构问题

### 7.1 epoll 和 io_uring 两套实现的 session 模型不统一
- epoll 使用 `internal_session<recv_cache, epoll_send_cache>`
- io_uring 使用 `internal_session<recv_cache>`（send 通过独立队列管理）
- session 生命周期管理方式不同，需要统一抽象

### 7.2 缺少 scheduler 的初始化/销毁接口
- 两套 scheduler 都没有完整的构造/初始化/析构流程
- 缺少 bind/listen 配置、ring 大小配置、buffer 大小配置等

### 7.3 session_id 改为 tagged handle
- 当前 `session_id` 是 `uint64_t`，直接存储 session 对象指针的 `reinterpret_cast`，缺乏类型安全，且 session 关闭后旧 session_id 会导致 use-after-free
- 改为 tagged handle 方案：利用 x86-64 用户态地址只用低 48 位的特性，高 16 位编码 generation 计数器
- 新增 `session_id_t` 类型，封装编码/解码逻辑（指针还原 + generation 提取），替代裸 `uint64_t`
- session 对象内新增 `uint16_t generation` 字段，每次 close 时递增，使旧 handle 自动失效
- scheduler 内部使用 session_id 时，还原指针后比较 generation，不匹配则回调异常并丢弃请求
- 零查找开销不变（仍是指针还原），运行时仅多一次 `uint16_t` 比较

### 7.4 缺少优雅关闭机制
- 没有 scheduler 级别的 shutdown 流程
- session 关闭时没有通知 pending 的 recv/send 请求
