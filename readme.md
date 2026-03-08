# Pump

Pump 是一个 C++26 异步流水线框架。用 `>>` 运算符把异步操作串成线性管道，编译期确定内存布局，运行期零分配。

```cpp
just(10)
    >> then([](int x) { return x * 2; })
    >> then([](int x) { printf("%d\n", x); })  // 输出 20
    >> submit(make_root_context());
```

## 为什么需要 Pump

传统异步编程的核心困境：**逻辑是线性的，但代码是碎片化的。**

回调把连续流程撕裂到多个函数里。Future 在高频场景下有堆分配和原子计数开销。裸协程需要手工封装调度和并发控制。

Pump 的解法很直接：**让异步代码的拓扑结构和业务逻辑的拓扑结构一致。** 你想的是"先做 A，再做 B，并发做 C"，写出来的代码就是 `A >> B >> concurrent >> C`。

## 核心机制

### Sender 与 Pipeline

Sender 是一个延迟操作的描述。它不执行任何动作，直到被 `submit` 启动。

`>>` 把多个 Sender 串联成 Pipeline。Pipeline 被连接（connect）时，所有 Sender 的 Op 被展平到一个 `std::tuple` 中——不是递归嵌套，是扁平数组。这个 tuple 的大小在编译期就确定了。

### OpPusher：位置驱动的执行引擎

这是 Pump 和 stdexec 最根本的区别。

stdexec 用递归 Receiver 模型：每个操作完成后调用下一个 Receiver 的 `set_value`，形成深层嵌套的调用栈。Pump 的 OpPusher 用编译期位置索引：`push_value<pos>` 直接跳到 `pos+1` 的操作，整个执行过程在一个扁平 tuple 上线性推进。

这带来三个好处：
- 没有递归深度问题，长流水线不会栈溢出
- 缓存局部性好，所有 Op 在连续内存中
- 支持跳转和回滚——`repeat`、异常恢复等控制流只需调整位置索引

OpPusher 传播四种信号：
- `push_value` — 正常值向下传递
- `push_exception` — 异常向下传播，直到被 `any_exception` 捕获
- `push_skip` — 跳过当前元素（用于 `maybe` 等条件算子）
- `push_done` — 流结束

### Context：栈式上下文

在复杂流水线中，多个步骤需要访问同一份状态。传统做法是在每个 lambda 的参数列表中传递，Pump 用类型化的上下文栈解决这个问题：

```cpp
just()
    >> push_context(std::make_shared<MyState>())
    >> get_context<std::shared_ptr<MyState>>()
    >> then([](std::shared_ptr<MyState>& state) {
        state->update();
    })
    >> pop_context()
    >> submit(context);
```

`push_context` 压栈，`get_context<T>` 按类型查找并追加到当前值列表，`pop_context` 弹栈。对称使用，生命周期由栈管理。

## 算子速查

### 起点与终点
| 算子 | 作用 |
|------|------|
| `just(v...)` | 创建携带初始值的 Sender |
| `submit(context)` | 启动 Pipeline 执行 |

### 变换
| 算子 | 作用 |
|------|------|
| `then(f)` | 同步变换：`f(上游值) -> 下游值` |
| `transform(f)` | 同 `then` |
| `flat_map(f)` | `f` 返回一个新 Sender，展平后继续执行 |
| `flat()` | 上游值本身是 Sender 时，展平执行 |

### 流式处理
| 算子 | 作用 |
|------|------|
| `for_each(range)` | 将 range 的每个元素作为独立的流元素发射 |
| `for_each_by_args()` | 将上游容器拆成流 |
| `as_stream()` | `for_each` 的别名（`generate.hh`） |
| `repeat(n)` | 产生 n 个流元素（值为 0..n-1） |
| `forever()` | 无限流 |
| `reduce()` | 收集流的所有元素到容器 |
| `count()` | 统计流元素个数 |
| `all()` | 等待所有流元素完成，返回 bool（全部成功则 true） |

### 并发控制
| 算子 | 作用 |
|------|------|
| `concurrent(max)` | 声明最大并发数。**必须配合调度器使用才有真正并发。** |
| `sequential()` | 将并发流收束为串行流 |
| `on(sender)` | 将当前值转移到另一个 Sender 上执行（通常是调度器 Sender） |

### 条件与分支
| 算子 | 作用 |
|------|------|
| `maybe()` | `optional<T>` 有值时解包继续，无值时跳过 |
| `maybe_not()` | 与 `maybe` 相反 |
| `when_skipped(f)` | 处理跳过信号，`f` 返回新 Sender 作为替代路径 |
| `visit(...)` | 将运行时类型（`bool`/`variant`/`T*`）提升为编译期分支，配合 `if constexpr` 使用 |

### 聚合
| 算子 | 作用 |
|------|------|
| `when_all(s1, s2, ...)` | 并行执行多个 Sender，收集所有结果 |
| `when_any(s1, s2, ...)` | 并行执行，第一个完成的胜出 |

### 异常处理
| 算子 | 作用 |
|------|------|
| `any_exception(f)` | 捕获所有异常，`f(exception_ptr)` 返回恢复 Sender |
| `catch_exception<T>(f)` | 按类型捕获 |
| `ignore_all_exception()` | 吞掉所有异常，继续执行 |

### 协程桥接
| 算子 | 作用 |
|------|------|
| `await_able(context)` | 将 Sender 转为 `co_await` 对象 |

## 调度层

Pump 采用 Share-Nothing 架构：每个调度器实例只在一个线程上运行，不需要任何内部同步。多核并行通过每个核心持有独立的调度器实例实现，线程内循环调用 `advance()` 驱动所有调度器。

| 调度器 | 用途 |
|--------|------|
| **Task Scheduler** | 通用任务调度与定时器，`on(sched->as_task())` 切换执行域 |
| **Net Scheduler** | 网络 IO，支持 io_uring（推荐）、epoll 和 DPDK 三个后端 |
| **NVMe Scheduler** | 集成 SPDK 的零拷贝磁盘 IO，以页为单位操作 |
| **RPC** | 基于 Net Scheduler 的轻量 RPC 框架，支持 pipelining 和并发服务端 |

完整示例见 `apps/example/`（echo server、rpc、并发控制等）。

## 项目结构

```
src/pump/
  core/           # 核心抽象：op_pusher, context, scope, op_tuple_builder
  sender/         # 算子实现：just, then, for_each, concurrent, visit 等
  coro/           # 协程支持：return_yields, await_able

src/env/
  scheduler/
    task/         # 任务调度器 + 定时器
    dgram/        # 数据报传输层（io_uring / epoll / DPDK）
    udp/          # UDP 调度器（io_uring / epoll / DPDK 三后端）
    kcp/          # KCP 调度器（io_uring / DPDK 两后端）
    tcp/          # TCP 调度器（io_uring / epoll）
    nvme/         # NVMe 调度器（SPDK）
    rpc/          # RPC 框架（client/server，transport trait 支持 TCP/KCP）
  runtime/        # Share-Nothing 多核运行时

apps/
  example/
    hello_world/  # 基础用法：just, then, for_each, context, exception, coroutine
    echo/         # 网络 Echo（UDP/KCP/TCP × io_uring/epoll，CLI 选择）
    dpdk/         # DPDK Echo（UDP/KCP over DPDK TAP，含 Python 测试脚本）
    concurrent/   # 并发控制测试
    share_nothing/# 多核并发测试
    coro/         # 协程桥接
    nvme/         # NVMe 调度器示例
    rpc/          # RPC 示例
  test/           # 测试（sequential, when_any）
  kv/             # 基于 Pump 的高性能 KV 存储
```
