# Pump

用 `>>` 把异步操作串成线性管道的 C++26 框架。编译期展平为 `std::tuple`，运行期零分配。

```cpp
just(10)
    >> then([](int x) { return x * 2; })
    >> then([](int x) { printf("%d\n", x); })  // 输出 20
    >> submit(make_root_context());
```

## 为什么

异步编程的核心困境：**逻辑是线性的，代码是碎片化的。**

回调把连续流程撕裂到多个函数。Future 有堆分配和原子计数开销。协程需要手工封装调度和并发控制。

Pump 的解法：**让代码拓扑和业务拓扑一致。** "先做 A，再做 B，并发做 C" 写出来就是 `A >> B >> concurrent >> C`。

## 实战：KV 存储写路径

一次 batch put 跨越 5 个 scheduler，涉及 Leader/Follower 合并、多级并发、variant 分支和分层异常处理——如果用回调或协程写，这是上千行散落在十几个函数里的状态机。用 Pump 写出来仍然是一条线性 pipeline：

```cpp
auto apply() {
    return get_context<batch*>()
        >> then([](batch* b) {
            return just()
                >> request_put_serial_number(b)           // → batch scheduler：分配版本号
                >> update_index(b)                        // → index scheduler × N：并发更新索引
                >> merge_batch_and_allocate_page()        // → fs scheduler：Leader/Follower 合并分配页面
                >> then([b](auto&& res) {                 //   返回 variant<leader*, follower, failed>
                    if constexpr (is_same_v<leader_res*, __typ__(res)>) {
                        return just()                     // Leader：写入 + 通知 Follower
                            >> write_data(res->span_list) //   → nvme scheduler × M：并发 DMA 写
                            >> write_meta(res)            //   → fs + nvme：元数据（嵌套 Leader/Follower）
                            >> free_page_when_error(res->span_list)
                            >> notify_follower(res);
                    }
                    else if constexpr (is_same_v<follower_res, __typ__(res)>) {
                        return just(__fwd__(res));         // Follower：等 Leader 通知后继续
                    }
                    else {
                        return just(make_exception_ptr(new allocate_page_failed()));
                    }
                })
                >> flat()
                >> ignore_all_exception()                 // 屏蔽异常，确保缓存阶段执行
                >> cache_data_if_succeed(b);              // → index scheduler × N：并发缓存
        })
        >> flat();
}

// 每个子阶段也是 pipeline
auto update_index(batch* b) {
    return for_each(b->cache)                             // 每个 KV 对
        >> concurrent()                                   // 并发路由到 index scheduler
        >> then([](key_value& kv) { return index::update(kv.file); })
        >> flat() >> reduce()
        >> then([](bool ok) { if (!ok) throw update_index_failed(); });
}

auto write_data(write_span_list& list) {
    return for_each(list.spans)                           // 每个 span
        >> concurrent()                                   // 并发提交到 nvme scheduler
        >> nvme::then_put_span()
        >> all([](write_span& s) { return s.all_wrote(); });
}
```

跨域数据流：

```
batch scheduler           分配版本号
       ↓
index scheduler × N       并发更新索引（for_each + concurrent）
       ↓
fs scheduler               Leader/Follower 合并分配页面 → variant<leader*, follower, failed>
       ↓
    ┌──┴──────────────────┐
    │ leader              │ follower
    ↓                     ↓
nvme scheduler × M       （等待 leader 通知）
    │ 并发 DMA 写数据
    ↓
fs scheduler               写元数据（嵌套 Leader/Follower）
    ↓
nvme scheduler × P         并发 DMA 写元数据
    │
    ├── notify_follower ──→ follower 继续
    ↓                     ↓
index scheduler × N        并发缓存数据
```

30 行代码，5 次跨域跳转，体现了 Pump 的核心优势：

- **声明式编排** — 读起来就是自上而下的线性流程，没有回调地狱和状态机
- **零锁并发** — 每个 scheduler 单线程运行，跨域通信走 per-core SPSC 队列，无 CAS 竞争
- **编译期类型安全** — `if constexpr` 分支在编译时展开，Op 平铺到 `std::tuple`，零虚函数开销
- **结构化异常安全** — `ignore_all_exception()` + cache 构成类 RAII 保证，异常路径编译期可见
- **Scheduler = 单线程状态容器** — Leader/Follower 合并写在 `handle()` 中，就是普通顺序代码

完整代码见 `apps/kv/`。

## 核心机制

### Sender 与 Pipeline

Sender 描述一个延迟操作，直到 `submit` 才执行。`>>` 串联多个 Sender 成 Pipeline，`connect()` 时所有 Op 展平到一个 `std::tuple` 中——扁平数组，不是递归嵌套。tuple 大小编译期确定。

### OpPusher：位置驱动执行

Pump 和 stdexec 的根本区别。stdexec 用递归 Receiver 模型，每层操作嵌套调用 `set_value`。Pump 用编译期位置索引：`push_value<pos>` 直接跳到 `pos+1`，在扁平 tuple 上线性推进。

- 没有递归深度问题，长流水线不会栈溢出
- 缓存局部性好，所有 Op 在连续内存中
- 支持跳转和回滚——`repeat`、异常恢复只需调整位置索引

四种信号：`push_value`（值传递）、`push_exception`（异常传播）、`push_skip`（跳过元素）、`push_done`（流结束）。

### Context：栈式上下文

跨多个步骤传递状态，不需要逐层 lambda 捕获：

```cpp
just()
    >> push_context(std::make_shared<MyState>())
    >> get_context<std::shared_ptr<MyState>>()
    >> then([](std::shared_ptr<MyState>& state) { state->update(); })
    >> pop_context()
    >> submit(context);
```

## 调度层

Share-Nothing 架构：每个调度器实例单线程运行，无内部同步。多核通过每核独立实例实现，线程内循环 `advance()` 驱动。

| 调度器 | 用途 |
|--------|------|
| **Task** | 通用任务调度与定时器，`on(sched->as_task())` 切换执行域 |
| **TCP** | TCP 网络 IO（io_uring / epoll） |
| **UDP** | UDP 数据报（io_uring / epoll / DPDK） |
| **KCP** | 可靠 UDP（io_uring / epoll / DPDK） |
| **NVMe** | SPDK 零拷贝磁盘 IO |
| **RPC** | 轻量 RPC，session-only API，支持 TCP/KCP 等可靠协议 |

## 算子速查

### 变换与流控
| 算子 | 作用 |
|------|------|
| `just(v...)` | 起始值 |
| `submit(ctx)` | 启动执行 |
| `then(f)` / `transform(f)` | 同步变换 |
| `flat_map(f)` | f 返回 Sender，展平执行 |
| `flat()` | 展平上游 Sender |
| `on(sender)` | 切换执行域 |

### 流式处理
| 算子 | 作用 |
|------|------|
| `for_each(range)` / `as_stream()` | 将 range 流式化 |
| `repeat(n)` / `forever()` | 重复 n 次 / 无限流 |
| `concurrent(max)` | 声明并发（需配合调度器） |
| `sequential()` | 收束为串行 |
| `reduce()` / `all()` / `count()` | 聚合流元素 |

### 分支与条件
| 算子 | 作用 |
|------|------|
| `visit()` | 运行时类型 → 编译期分支（bool/variant/T*） |
| `maybe()` / `maybe_not()` | optional 解包 / 反向 |
| `when_skipped(f)` | 处理跳过信号 |
| `when_all(...)` / `when_any(...)` | 并行等待 / 竞争 |

### 异常与协程
| 算子 | 作用 |
|------|------|
| `any_exception(f)` | 捕获所有异常，f 返回恢复 Sender |
| `catch_exception<T>(f)` | 按类型捕获 |
| `ignore_all_exception()` | 吞异常继续执行 |
| `await_able(ctx)` | Sender → `co_await` |

## 项目结构

```
src/pump/
  core/           # op_pusher, context, scope, op_tuple_builder
  sender/         # just, then, for_each, concurrent, visit ...
  coro/           # return_yields, await_able

src/env/
  scheduler/
    task/         # 任务调度 + 定时器
    nvme/         # NVMe（SPDK）
    net/          # 网络相关
      common/     # session 组合、net_frame、通用 send/recv sender
      tcp/        # TCP（io_uring / epoll）
      udp/        # UDP（io_uring / epoll / DPDK）
      kcp/        # KCP（io_uring / epoll / DPDK）
      dgram/      # 数据报传输层（io_uring / epoll / DPDK）
      rpc/        # RPC（session-only，支持 TCP / KCP）
  runtime/        # Share-Nothing 多核运行时

apps/
  example/        # hello_world, echo, rpc, concurrent, coro, nvme, dpdk
  test/           # sequential, when_any
  kv/             # 高性能 KV 存储（展示完整 scheduler 协作）
```
