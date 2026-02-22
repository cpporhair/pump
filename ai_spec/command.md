# echo 示例代码审核：问题清单

## 一、echo.cc — 运行时初始化缺失（致命崩溃）

### 1.1 `create_runtime_schedulers()` 未创建和初始化任何 scheduler 实例
- `create_runtime_schedulers()` 只做了 `new runtime_schedulers()`，返回一个空的运行时对象
- 没有创建 `accept_scheduler_t` 实例，没有调用其 `init(address, port, queue_depth)` 进行 socket bind/listen 和 io_uring ring 初始化
- 没有创建 `session_scheduler_t` 实例，没有调用其 `init(queue_depth)` 进行 io_uring ring 初始化
- 没有创建 `task_scheduler_t` 实例（如果需要的话）
- 没有调用 `add_core_schedulers(...)` 将 scheduler 实例注册到 `schedulers_by_core` 和 `schedulers` 容器中

### 1.2 `schedulers_by_core` 为空导致 vector 越界崩溃
- 由于 1.1 的问题，`schedulers_by_core` 向量为空
- `env::runtime::start(rs->schedulers_by_core)` 内部直接用 `sched_getcpu()` 的返回值作为下标访问空向量，触发 `__n < this->size()` 断言失败并 core dump
- 需要在 `create_runtime_schedulers()` 中按 CPU 核心数正确填充 `schedulers_by_core`

### 1.3 `get_schedulers<accept_scheduler_t>()[0]` 也会越界
- `wait_connection(rs->get_schedulers<accept_scheduler_t>()[0])` 访问 `schedulers` 中的 `vector<accept_scheduler_t*>`，同样为空
- 即使修复了 1.2 的崩溃，这里也会因为空向量访问而崩溃

## 二、echo.cc — 管线逻辑问题

### 2.1 `pkt_iovec.vec` 内存泄漏
- `_recv_pkt_getter` 中通过 `new iovec[...]` 分配了 iovec 数组（1个或2个元素）
- `read_packet_coro` 通过 `co_yield` 将 `pkt_iovec` 传递给下游
- 下游 `flat_map` 中将 `pkt.vec` 和 `pkt.cnt` 传给 `scheduler::net::send()`，send 完成后无人负责 `delete[] pkt.vec`
- 每次 echo 回写都会泄漏一次 iovec 数组

### 2.2 `read_packet_coro` 中 `forward_head` 与异步 send 的数据竞争
- `read_packet_coro` 在 `co_yield` 之后立即调用 `buf->forward_head(pio.cnt)` 推进环形缓冲区的读指针
- 但此时 `pkt_iovec.vec` 中的 iovec 仍然指向环形缓冲区 `_data` 中的原始数据区域
- `forward_head` 推进后，该区域被标记为可写空间，后续的 `io_uring readv` 可能覆盖这块内存
- 而异步 send（`io_uring writev`）可能尚未完成，导致发送的数据被污染
- 需要在 send 完成之后再推进 head，或者在 send 之前将数据拷贝出来

### 2.3 `session_proc` 中 join 的 session 分配逻辑
- `sid.raw() % 2` 硬编码假设有 2 个核心的 session_scheduler
- 如果实际核心数不是 2，或者 `schedulers_by_core` 中 session_scheduler 的分布不同，会导致越界或负载不均
- 需要根据实际可用的 session_scheduler 数量做取模

### 2.4 `check_session` 协程的轮询方式
- `check_session` 通过 `while (!sd.closed.load()) co_yield true` 实现会话存活检查
- 这意味着每次 recv/send 循环都会检查一次 `closed` 标志
- 但由于整个管线是同步推进的（recv → parse → send），`closed` 标志只在 `any_exception` 的异常处理中被设置
- 实际上 `check_session` 永远不会在 recv 之前看到 `closed=true`（因为异常会先被 `any_exception` 捕获），这个检查是冗余的

## 三、echo.cc — main 函数结构问题

### 3.1 两层 `submit` 的执行模型不清晰
- 外层 `submit(core::make_root_context(create_runtime_schedulers()))` 同步执行，创建运行时并启动内层管线
- 内层 `submit(core::make_root_context(rs))` 启动 accept → echo 循环
- 但 `env::runtime::start(rs->schedulers_by_core)` 会阻塞当前线程运行事件循环
- 需要确保内层 submit 的 accept 管线在 `start()` 之前已经设置好（将 conn_req 注册到 accept_scheduler 的队列中），否则 `start()` 开始事件循环时没有待处理的请求

### 3.2 `start()` 中线程未 detach 或 join
- `share_nothing.hh` 中的 `start(const std::vector<scheduler_runner*>&)` 重载（第60行）直接构造 `std::thread` 但既没有 `detach()` 也没有 `join()`
- `std::thread` 析构时如果 joinable 会调用 `std::terminate()`
- 需要对非当前核心的线程调用 `.detach()` 或保存线程句柄后续 join
- 注：使用 `start(vector<tuple<...>>&)` 重载（第108行）的版本已经有 `.detach()`，需要确认 echo 实际走的是哪个重载

## 四、io_uring/scheduler.hh — 影响 echo 正确性的问题

### 4.1 `submit_write` 中 session 解码失败时未处理 sqe
- 当 `req->session_id.decode<session_t>()` 返回 nullptr 时，`sqe` 已经通过 `io_uring_get_sqe` 获取但未被准备（没有调用 `io_uring_prep_writev`）
- 未准备的 sqe 被提交到 io_uring 会导致未定义行为
- 需要在 decode 失败时回调错误并跳过该 sqe，或者在获取 sqe 之前先验证 session

### 4.2 `handle_send_request` 中 send_q 未完全排空
- 当 `io_uring_get_sqe` 返回 nullptr（SQ 满）时，只将当前请求放入 `send_list` 然后 break
- 但 `send_q` 中可能还有更多请求未被取出，这些请求会留在 queue 中等到下一次 advance
- 如果 advance 频率不够高，可能导致 send 延迟增大

### 4.3 `on_read_event` 中 recv callback 与 submit_read 的顺序
- `process_cqe` 的 read 分支先调用 `on_read_event`（回调 recv 请求），然后调用 `submit_read`（提交下一次读取）
- `on_read_event` 中的回调可能会同步触发新的 `schedule(recv_req)`，而此时 submit_read 尚未执行
- 如果新的 recv_req 发现 buffer 中有完整包（上次读取带来的多个包），会直接同步回调，不会有问题
- 但如果回调链比较长导致 recv_cache 的状态在 submit_read 之前被修改，可能存在微妙的时序问题
