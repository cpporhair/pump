# echo 示例代码修复计划

## 一、echo.cc — 运行时初始化缺失（致命崩溃）

### 1.1 修复 `create_runtime_schedulers()` — 创建并初始化所有 scheduler 实例

**当前问题**：`create_runtime_schedulers()` 只执行了 `new runtime_schedulers()`，未创建任何 scheduler。

**修复步骤**：

1. 确定运行时参数：
   - 监听地址（address）和端口（port），可硬编码或从 `argv` 读取
   - io_uring 队列深度（queue_depth），参考其他示例（如 `apps/kv` 或 `apps/example/share_nothing`）确定合理值
   - CPU 核心数（num_cores），通过 `std::thread::hardware_concurrency()` 或 `sysconf(_SC_NPROCESSORS_ONLN)` 获取，或暂时硬编码为可配置值

2. 创建 `accept_scheduler_t` 实例：
   - `auto* accept_sched = new accept_scheduler_t();`
   - 调用 `accept_sched->init(address, port, queue_depth)` 完成 socket bind/listen 和 io_uring ring 初始化
   - 检查 `init` 返回值，失败时打印错误并退出

3. 为每个核心创建 `session_scheduler_t` 实例：
   - 循环 `num_cores` 次，每次 `new session_scheduler_t()` 并调用 `init(queue_depth)`
   - 检查 `init` 返回值

4. 创建 `task_scheduler_t` 实例（如需要）：
   - 检查 `task_scheduler_t` 是否有 `init` 方法，如果有则调用

5. 通过 `add_core_schedulers(...)` 注册到 runtime：
   - 对每个核心调用 `rs->add_core_schedulers(task_sched, accept_sched_or_nullptr, session_sched)`
   - 注意：`accept_scheduler` 通常只绑定到一个核心（核心 0），其他核心传 `nullptr`
   - `add_core_schedulers` 的参数顺序需与 `runtime_schedulers` 模板参数顺序一致：`task_scheduler_t, accept_scheduler_t, session_scheduler_t`

**参考文件**：
- `runner.hh:93` — `add_core_schedulers` 签名和行为
- `io_uring/scheduler.hh:147-187` — `accept_scheduler::init` 的两个重载
- `io_uring/scheduler.hh:514-523` — `session_scheduler::init` 的两个重载
- 参考 `apps/example/share_nothing/main.cc` 或 `apps/kv/runtime/` 中的初始化模式

### 1.2 修复 `schedulers_by_core` 为空导致的越界崩溃

**当前问题**：`env::runtime::start(rs->schedulers_by_core)` 内部使用 `sched_getcpu()` 索引空向量。

**修复方式**：1.1 完成后此问题自动解决。`add_core_schedulers` 会逐核心填充 `schedulers_by_core`。

**验证**：确保 `schedulers_by_core.size() >= sched_getcpu() + 1`，可添加断言：
```cpp
assert(!rs->schedulers_by_core.empty() && "schedulers_by_core must not be empty");
```

### 1.3 修复 `get_schedulers<accept_scheduler_t>()[0]` 越界

**当前问题**：`schedulers` 中的 `vector<accept_scheduler_t*>` 为空。

**修复方式**：1.1 完成后此问题自动解决。`add_core_schedulers` 会同时填充 `schedulers` tuple 中对应类型的 vector。

**验证**：确保调用 `wait_connection` 前，`rs->get_schedulers<accept_scheduler_t>()` 非空。

---

## 二、echo.cc — 管线逻辑问题

### 2.1 修复 `pkt_iovec.vec` 内存泄漏

**当前问题**：`_recv_pkt_getter` 中 `new iovec[...]` 分配的内存无人释放。

**修复步骤**：

1. 定位 `get_recv_pkt` 函数（在 `common/detail.hh` 中），确认 `pkt_iovec.vec` 的分配方式
2. 方案 A — 在 send 完成后释放：
   - 在 `session_proc` 的 `flat_map` 中，send 返回后添加 `then` 释放 `pkt.vec`：
     ```cpp
     >> flat_map([](const session_data &sd, scheduler::net::common::pkt_iovec &&pkt) {
         auto* vec_ptr = pkt.vec;
         return scheduler::net::send(sd.scheduler, sd.id, pkt.vec, pkt.cnt)
             >> then([vec_ptr](...) { delete[] vec_ptr; });
     })
     ```
3. 方案 B — 修改 `pkt_iovec` 结构，使用 RAII（如 `std::unique_ptr<iovec[]>`），在析构时自动释放
   - 这需要修改 `common/struct.hh` 中的 `pkt_iovec` 定义
   - 影响面较大，需要评估所有使用方
4. 方案 C — 完全避免动态分配，使用栈上的固定大小数组（如 `iovec vec[2]`），修改 `pkt_iovec` 为值类型
   - 在 `common/struct.hh` 中将 `pkt_iovec` 改为包含固定大小 `iovec` 数组的结构体

**推荐方案**：方案 A 最小侵入，先实现 A 确保不泄漏；后续可考虑 C 做更彻底的优化。

**参考文件**：
- `common/detail.hh` — `get_recv_pkt` 实现
- `common/struct.hh` — `pkt_iovec` 定义

### 2.2 修复 `forward_head` 与异步 send 的数据竞争

**当前问题**：`read_packet_coro` 中 `co_yield` 后立即 `forward_head`，但 send 可能还未完成，导致环形缓冲区数据被覆盖。

**修复步骤**：

1. 分析数据流：`read_packet_coro` → `co_yield pkt_iovec` → 下游 `flat_map` 调用 `send()` → send 完成
2. 方案 A — 将 `forward_head` 延迟到 send 完成后：
   - 将 `packet_buffer*` 和 `pio.cnt` 一起传递到下游
   - 在 send 完成的回调/then 中执行 `buf->forward_head(cnt)`
   - 需要修改 `pkt_iovec` 结构或额外传递 buffer 指针和 forward 计数
   - 具体做法：修改 `pkt_iovec` 增加 `packet_buffer*` 和 `forward_cnt` 字段
   - 在 `read_packet_coro` 中不再调用 `forward_head`，改为填充这两个字段
   - 在 send 完成的 `then` 中调用 `pkt.buf->forward_head(pkt.forward_cnt)`

3. 方案 B — 在 yield 之前将数据拷贝到独立缓冲区：
   - 在 `read_packet_coro` 中，yield 之前分配新 buffer 并 memcpy
   - `pkt_iovec.vec` 指向拷贝后的数据
   - 可以安全 `forward_head`
   - 缺点：额外的内存分配和拷贝开销

**推荐方案**：方案 A，零拷贝且符合异步设计模式。

**注意**：此修复与 2.1 的 iovec 释放紧密相关，两者应一起设计。

### 2.3 修复 `session_proc` 中 `sid.raw() % 2` 硬编码

**当前问题**：硬编码 `% 2` 假设有 2 个 session_scheduler。

**修复步骤**：

1. 将 `runtime_schedulers` 指针 `rs` 传递到取模处
2. 使用 `rs->get_schedulers<session_scheduler_t>().size()` 获取实际数量
3. 替换 `sid.raw() % 2` 为 `sid.raw() % rs->get_schedulers<session_scheduler_t>().size()`
4. 注意：`get_by_core` 使用的是核心编号作为索引，而取模后的值应该是核心索引
   - 需要确认 `session_scheduler` 在哪些核心上有实例
   - 如果不是所有核心都有 session_scheduler，需要建立一个有效核心索引列表
5. 更安全的做法：取模基于 `schedulers_by_core.size()` 或收集所有非 nullptr 的 session_scheduler 核心列表

**修复代码示意**：
```cpp
auto session_count = rs->get_schedulers<session_scheduler_t>().size();
auto core_idx = sid.raw() % session_count;
// 使用 get_schedulers<session_scheduler_t>()[core_idx] 代替 get_by_core
```

**注意**：`session_proc` 中有两处 `get_by_core<session_scheduler_t>(sid.raw() % 2)`，都需要修改。

### 2.4 `check_session` 协程的轮询方式 — 评估是否移除

**当前问题**：`check_session` 的 `closed` 检查是冗余的，因为异常会先被 `any_exception` 捕获。

**处理方式**：

1. 分析 `check_session` 在管线中的角色：它作为 `as_stream()` 的源，控制循环是否继续
2. 当前管线结构：`check_session → as_stream → recv → parse → send → count → any_exception → count`
3. 当异常发生时（如连接断开），`any_exception` 捕获异常并设置 `closed=true`
4. 但此时管线已经因异常终止，`check_session` 的循环不会再被驱动
5. 两种处理方式：
   - **保留但简化**：如果 `check_session` 的存在是为了未来扩展（如外部关闭信号），可保留但添加注释说明当前是冗余的
   - **移除**：用 `just() >> forever()` 替代 `check_session`，依赖异常机制终止循环
6. **推荐**：暂时保留，添加 TODO 注释说明冗余性，优先修复致命 bug

---

## 三、echo.cc — main 函数结构问题

### 3.1 修复两层 `submit` 的执行模型

**当前问题**：内层管线需要在 `start()` 之前将 accept 请求注册到队列中。

**分析**：

1. 当前执行流程：
   ```
   外层 submit → create_runtime_schedulers() → then(setup内层管线) → then(start())
   ```
2. `then` 是顺序执行的，所以内层 `submit` 会先执行（设置管线并提交 accept 请求），然后才执行 `start()`
3. 需要验证：内层 `submit` 是否是同步完成的？即它是否在返回前就已经将 `wait_connection` 对应的 accept 请求提交到了 accept_scheduler 的队列中
4. 检查 `submit` 的实现：如果 `submit` 是同步启动管线的第一步（提交初始请求），那么 `start()` 之前请求已在队列中，是正确的
5. 如果 `submit` 只是注册而不执行，则需要确保 `start()` 后事件循环会驱动管线

**修复步骤**：

1. 阅读 `submit` 和 `make_root_context` 的实现，确认管线启动的同步/异步语义
2. 如果内层 `submit` 是同步触发首个 `schedule(conn_req)` 的，则当前顺序正确，无需修改
3. 如果不是，需要重构 main：
   - 方案：将管线设置和 start 的顺序调整为：先 setup 管线（注册 accept 请求到队列），再 start 事件循环
   - 或者在 `start()` 内部添加初始化钩子

### 3.2 修复 `start()` 中线程未 detach 或 join

**当前问题**：`share_nothing.hh:60` 的 `start(const std::vector<scheduler_runner*>&)` 重载中，`std::thread` 构造后既未 detach 也未 join。

**修复步骤**：

1. 确认 echo 实际使用的是哪个 `start` 重载：
   - `echo.cc:145` 调用 `env::runtime::start(rs->schedulers_by_core)`
   - `rs->schedulers_by_core` 类型为 `std::vector<std::tuple<...>*>`...不对
   - 看 `runner.hh:55`：`schedulers_by_core` 类型为 `std::vector<std::tuple<Schedulers*...>>`
   - 对应 `share_nothing.hh:108` 的 `start(std::vector<std::tuple<scheduler_t*...>>&)` 重载
   - 该重载（第118-121行）已有 `.detach()`，所以 **echo 不受此 bug 影响**

2. 但 `share_nothing.hh:60` 的另一个重载仍有 bug，需要修复：
   - 在第65行，将 `std::thread([runner](){runner->operator()();});` 改为：
     ```cpp
     std::thread([runner](){(*runner)();}).detach();
     ```

**参考文件**：
- `share_nothing.hh:60-72` — 有 bug 的 `start` 重载
- `share_nothing.hh:108-136` — echo 实际使用的 `start` 重载（已有 detach）

---

## 四、io_uring/scheduler.hh — 影响 echo 正确性的问题

### 4.1 修复 `submit_write` 中 session 解码失败时未处理 sqe

**当前问题**：`io_uring_get_sqe` 获取 sqe 后，若 `decode` 返回 nullptr，sqe 未被准备就会被提交。

**修复步骤**：

1. 定位代码：`scheduler.hh:446-456` — `submit_write` 函数
2. 方案 A — 先验证 session 再获取 sqe（推荐）：
   ```cpp
   auto submit_write(io_uring_sqe *sqe, common::send_req *req) {
       auto* s = req->session_id.decode<session_t>();
       if (!s) {
           // 回调错误，不使用 sqe
           // 需要将 sqe 标记为 NOP 或者重构调用方不提前获取 sqe
           io_uring_prep_nop(sqe);  // 提交一个空操作避免未定义行为
           req->on_err(/* error code */);
           return;
       }
       io_uring_prep_writev(sqe, s->fd, req->iov, req->iov_cnt, 0);
       // ... 设置 user_data 等
   }
   ```
3. 方案 B — 重构 `handle_send_request`，在获取 sqe 之前先验证 session：
   - 在 `handle_send_request` 中，先 decode session
   - 如果 decode 失败，直接回调错误，不获取 sqe
   - 如果成功，再 `io_uring_get_sqe` 并调用 `submit_write`

**推荐方案**：方案 B 更干净，避免浪费 sqe 资源。但如果 `submit_write` 的调用方已经获取了 sqe，则用方案 A 中的 `io_uring_prep_nop` 作为安全降级。

**参考文件**：
- `scheduler.hh:446-456` — `submit_write`
- `scheduler.hh:459-486` — `handle_send_request`

### 4.3 ~~评估 `on_read_event` 中 recv callback 与 submit_read 的顺序~~ — 无需修改

**原始问题**：`process_cqe` 中先回调 `on_read_event`，再 `submit_read`，是否存在时序问题。

**分析结论：当前代码正确，无需修改。**

1. **同步回调场景**：所有操作在 session_scheduler 线程内顺序执行，无并发问题。
2. **异步回调场景**（回调链跳转到其他 scheduler）：`cb()` 立即返回（仅 enqueue），`on_read_event` 和 `submit_read` 之间无外部干扰。
3. **`schedule(recv_req*)` 不走队列是有意设计**：同一时刻只有一个线程调用它，构成合法的 SPSC 模式（producer = session_scheduler 线程做 `forward_tail`，consumer = 调用方线程做 `has_full_pkt`/`forward_head`）。
4. **`packet_buffer` 的 `used()` 中 `_tail.load(relaxed)`**：consumer 端读 producer 的变量理论上应用 `acquire`，但在 x86 TSO 下所有 load 隐含 acquire 语义，因此当前实现正确。如未来需支持 ARM 等弱内存序架构，需将 consumer 端的 `_tail.load` 改为 `acquire`。

---

## 五、修复优先级和执行顺序

### P0 — 致命崩溃（必须先修）
1. **1.1** `create_runtime_schedulers()` 创建并初始化 scheduler 实例
2. **1.2** 自动解决（依赖 1.1）
3. **1.3** 自动解决（依赖 1.1）

### P1 — 数据正确性
4. **2.2** `forward_head` 与异步 send 的数据竞争（需先于 2.1 设计，因为两者的修复方案相互关联）
5. **2.1** `pkt_iovec.vec` 内存泄漏（与 2.2 一起修复）
6. **4.1** `submit_write` 中 session 解码失败的未定义行为

### P2 — 健壮性和正确性
7. **2.3** `sid.raw() % 2` 硬编码修复
8. **3.2** `start()` 中线程未 detach（非 echo 直接路径，但影响其他使用者）

### P3 — 优化和清理
9. **3.1** 验证两层 submit 的执行模型（可能无需修改，需读代码确认）
10. **2.4** `check_session` 冗余检查（添加注释或移除）
11. ~~**4.3** `on_read_event` 回调顺序~~ — 已确认安全，无需修改（x86 下正确）

---

## 六、验证计划

1. **编译验证**：修复 1.1 后，确保 `example.echo` 编译通过
2. **启动验证**：运行 echo 服务，确认不再崩溃（验证 1.1/1.2/1.3）
3. **功能验证**：使用 telnet/nc 连接，发送数据验证 echo 回显正确
4. **内存验证**：使用 valgrind 或 AddressSanitizer 检查内存泄漏（验证 2.1）
5. **并发验证**：多客户端同时连接，验证 session 分配和数据不错乱（验证 2.2/2.3）
6. **异常验证**：客户端异常断开，验证 session 正确清理
