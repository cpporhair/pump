# Pump: 高性能可组合异步流程管道库

Pump 是一个专为高性能场景设计的 C++ 异步库，基于 **Sender/Operator** 语义构建。它允许开发者通过声明式的方式组合复杂的异步逻辑，并通过 **Scheduler** 在不同的执行域（如计算线程、NVMe IO、网络事件循环）之间切换。

## 1. 技术架构概览

Pump 的架构分为四个核心层次：

### 1.1 编排层 (Sender & Pipeline)
*   **Sender**：Pump 的基本构建单元，描述一个延迟执行的操作。它是声明式的，在被连接（connect）并启动前不执行任何操作。
*   **Pipeline (`>>`)**：通过重载运算符 `>>`，将多个 Sender 串联成线性流水线，避免了嵌套的回调结构，使业务逻辑呈现线性的拓扑结构。

### 1.2 执行引擎 (Op & OpPusher)
这是 Pump 核心实现特征：
*   **Op (Operator)**：当 Sender 被连接时，会根据上下文生成一组 Op。Pump 将整个流水线的 Op 映射到一个平铺的 `std::tuple` 中。
*   **OpPusher**：轻量级执行推进器，通过编译期的位置索引（Position）管理执行流。
    *   **位置推进**：`push_value(pos)` 触发 `pos + 1` 的操作，减少了运行时的虚函数寻址和递归深度。
    *   **控制流**：支持跳转与回滚，用于高效实现 `repeat`、`retry` 或异常跳转。

### 1.3 资源与上下文 (Scope & Context)
*   **Scope**：定义异步操作的生命周期范围，负责管理 Op 列表的内存和层级关系。
*   **Context**：采用栈式管理机制。支持通过 `push_context<T>` 和 `get_context<T>` 在流水线的深层直接访问高层定义的对象，而不需要在每个函数签名中显式传递。

### 1.4 调度层 (Scheduler)
Pump 强调执行域的隔离与切换：
*   **Task Scheduler**：通用任务调度与定时器。
*   **NVMe Scheduler**：集成 SPDK，提供零拷贝的磁盘 IO 调度。
*   **Net Scheduler**：支持 `io_uring` 和 `epoll` 的网络事件循环。
*   **Runtime**：`env/runtime/share_nothing.hh` 提供了多核运行模型（按核启动 scheduler 组合）。

---

## 2. 核心语义层

项目提供了丰富的 Sender 算子，涵盖了流程控制的各个方面：

*   **起点**：`just(...)`、`submit(context)`
*   **变换/展开**：`then`、`transform`、`flat/flat_map`
*   **并发/顺序**：`concurrent(max)`、`sequential()`、`with_concurrent(...)`
*   **流式**：`for_each`、`generate`、`repeat`
*   **聚合**：`when_all`、`reduce`、`to_container`
*   **异常与跳过**：`catch_exception`、`any_exception`、`when_skipped`、`visit`
*   **上下文操作**：`push_context`、`pop_context`、`get_context`
*   **协程桥接**：`await_able()` 或 `await_able(context)` 将 sender 变为可 `co_await`

---

## 3. 与传统异步方式的对比

### 3.1 传统方式的局限性
1.  **Callback (回调)**：导致“回调地狱”，逻辑被撕裂在多个函数中。错误处理复杂，且难以管理跨步骤的并发计数。
2.  **Future/Promise**：通常以单值为中心，组合能力依赖 `then/when_all` 等有限操作。在高频场景下，`std::future` 可能涉及堆分配和原子引用计数的额外开销。
3.  **裸协程**：虽然具有较好的可读性，但执行域切换与并发控制仍需开发者手工封装调度逻辑。

### 3.2 Pump 的改进
1.  **线性声明式流程**：通过 Pipeline 保持逻辑连续性，便于阅读和维护。
2.  **显式调度切换**：使用 `on(...)` 明确执行上下文，减少隐式跳转。
3.  **资源开销优化**：Op 链在连接时静态确定大小，运行期几乎无额外内存分配。
4.  **内建并发控制**：原生支持 `for_each + concurrent + reduce` 模式表达“扇出/收敛”。

### 3.3 代码对比示例：KV 存储写入流程
在高性能 KV 存储中，一个写入请求涉及：分配 ID -> 更新索引 -> 分配磁盘空间 -> 并发写 NVMe -> 失败回滚。

**Pump 方式（逻辑扁平，失败路径可组合）：**
```cpp
auto apply_request = 
    get_context<batch*>()
    >> then(allocate_id)
    >> then(update_index_concurrently) // 内部使用 for_each + concurrent
    >> then(allocate_disk_pages)
    >> then([](auto res) {
        return just()
            >> write_nvme_data(res->spans)
            >> write_metadata(res)
            // 失败时自动执行并发回滚
            >> catch_exception<write_failed>([res](...) {
                return free_disk_spans(res->spans); 
            });
    })
    >> flat();
```

**传统回调方式：**
需要维护多个计数器、处理嵌套的错误分支，状态传递极其分散。

---

## 4. 与 stdexec (P2300) 的对比

`stdexec` 是 C++26 异步标准的基石。Pump 在设计理念上与其契合，但在实现策略 and 应用场景上有所侧重。

| 特性 | stdexec (P2300) | Pump |
| :--- | :--- | :--- |
| **执行模型** | 递归 Receiver 模型 | 平铺 OpTuple + Position 推进 |
| **内存布局** | 嵌套的状态机结构 | 扁平化的元组结构 |
| **上下文传递** | 较弱，通常需通过参数显式传递 | 强支持，支持类型化的 Context 栈 |
| **编译性能** | 极其沉重的模板递归 | 优化的模板展开，编译压力相对较小 |
| **硬件集成** | 通用抽象 | 深度集成 NVMe/SPDK, io_uring 等高性能组件 |
| **类型分发** | 传统的静态匹配 (通常需要 `variant` 或 `any_sender`) | **visit** 算子 (将运行时值提升为编译期类型，支持 `if constexpr` 分支展开) |
| **信号机制** | `set_value/error/stopped` | `push_value/exception/skip/done` |

**差异总结：**
*   `stdexec` 追求**通用性最大化**，旨在成为 C++ 标准的通用异步底座。
*   `Pump` 侧重于**工程化性能与特定场景（存储/网络）的开发效率**。其 `OpPusher` 机制特别适合处理 long 流水线，`Context` 机制则大幅简化了复杂业务中的参数传递难题。

---

## 5. 项目结构

```
src/pump/              # 编排层核心：sender, op_pusher, context, coro
src/env/               # 调度层与运行时：task/nvme/net scheduler, runtime
apps/example/          # 示例：hello_world, echo, nvme, share_nothing
apps/kv/               # 应用示例：基于 Pump 构建的高性能 KV 存储
3rd/                   # 第三方依赖
```

## 6. 构建与依赖

### 关键依赖
*   **C++26 编译器** (GCC 13+ 或 Clang 17+)
*   **liburing** (用于网络 io_uring 调度)
*   **SPDK + DPDK** (用于 NVMe 调度)
*   **numa, pthread, ssl, crypto** (用于相关应用)

### 构建步骤
```bash
cmake -S . -B build
cmake --build build
```

---
> **注意**：本项目目前包含部分正在开发或需谨慎使用的算子（见 `src/pump/sender`），建议参考 `apps/example` 了解标准用法。
