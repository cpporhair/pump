# MPSC → Per-Core SPSC 队列迁移设计

## 1. 背景与动机

当前所有 scheduler 使用 MPSC 队列（`core::mpsc::queue` 或 `spdk_ring SPDK_RING_TYPE_MP_SC`）接收跨域请求。当多个核心同时向同一 scheduler 投递时，MPSC 的 `tail_` CAS 竞争成为瓶颈：
- CAS 失败时重试循环，尾延迟不可预测
- `tail_` cacheline 在多核间反复 bounce

PUMP 的 Single-Thread Scheduler 不变量保证：**同一核心上的所有 scheduler 共享一个线程**。因此以核心为粒度建立 SPSC 队列是天然可行的——同核心上的多个 scheduler 共享同一个 producer 身份。

### 1.1 目标

- 消除 CAS 竞争，将跨域入队操作降为单次 `relaxed load` + `release store`
- 保持已有的 scheduler API 不变（`schedule(req*)` / `advance()`）
- 不影响同核心内部的 local queue（已经无原子）

### 1.2 非目标

- 不完全消除原子操作（SPSC 跨线程仍需 acquire/release 语义）
- 不改变 scheduler 的业务逻辑
- 不改变 preemptive scheduler 的语义（它需要 MPMC）

---

## 2. 现状盘点

### 2.1 队列使用全景

| 位置 | 队列类型 | 变量 | 用途 | 迁移目标 |
|------|---------|------|------|---------|
| **task::scheduler** | `mpsc::queue` | `tasks_request_q` | 跨核投递任务 | per-core SPSC |
| **task::scheduler** | `mpsc::queue` | `timer_request_q` | 跨核投递定时器 | per-core SPSC |
| **task::preemptive_scheduler** | `mpmc::queue` | `tasks_request_q` | 抢占式任务（任意核可消费） | **不迁移** |
| **task::preemptive_scheduler** | `mpmc::queue` | `timer_request_q` | 抢占式定时器 | **不迁移** |
| **net::session_scheduler** (io_uring) | `mpsc::queue` | `join_q` | 跨核 join session | per-core SPSC |
| **net::session_scheduler** (io_uring) | `mpsc::queue` | `send_q` | 跨核 send 数据 | per-core SPSC |
| **net::session_scheduler** (io_uring) | `mpsc::queue` | `stop_q` | 跨核 stop session | per-core SPSC |
| **net::conn_scheduler** (io_uring) | `mpsc::queue` | `request_q` | 跨核请求连接 | per-core SPSC |
| **net::conn_scheduler** (io_uring) | `mpmc::queue` | `conn_fd_q` | accept 结果分发 | **需分析** |
| **net::session_scheduler** (epoll) | `mpsc::queue` | `send_q`, `join_q`, `stop_q` | 同上 | per-core SPSC |
| **net::connect_scheduler** (epoll) | `mpsc::queue` | `conn_request_q` | 跨核发起连接 | per-core SPSC |
| **net::connect_scheduler** (epoll) | `mpmc::queue` | `session_q` | 连接结果分发 | **需分析** |
| **nvme::scheduler** | `spdk_ring` (MPSC) | `put_data_page_req_queue` | 跨核提交写 IO | per-core SPSC |
| **nvme::scheduler** | `spdk_ring` (MPSC) | `get_data_page_req_queue` | 跨核提交读 IO | per-core SPSC |
| **kv::batch::scheduler** | `spdk_ring` (MPSC) | `publish_task_queue` 等 3 个 | 跨核批处理请求 | per-core SPSC |
| **kv::index::scheduler** | `spdk_ring` (MPSC) | `update_req_queue` 等 5 个 | 跨核索引操作 | per-core SPSC |
| **kv::fs::scheduler** | `spdk_ring` (MPSC) | `allocate_req_queue` 等 3 个 | 跨核文件系统操作 | per-core SPSC |
| **recv_cache** | `local::queue` | `recv_q`, `ready_q` | 同线程内部 | **不迁移**（已无原子） |

### 2.2 特殊情况分析

#### conn_fd_q / session_q（MPMC）

这些队列用于 accept/connect 结果分发：accept 线程（或 io_uring CQE 回调）作为 producer，任意请求连接的核心作为 consumer。

- `conn_fd_q`：`schedule(conn_req*)` 中先 `try_dequeue` conn_fd_q 看有没有现成连接，有则直接交付
- `session_q`：类似机制

这属于"多消费者抢连接"场景，不适合 SPSC。**保持 MPMC 不变。**

#### preemptive_scheduler（MPMC）

抢占队列的设计语义是"任意核可投递 + 任意核可消费"，是真正的 MPMC 场景。**保持不变。**

---

## 3. 核心设计

### 3.1 per_core_queue 包装

```cpp
namespace pump::core {

    // 编译期最大核心数，可通过 CMake 配置
    inline constexpr uint32_t MAX_CORES = 128;
    inline constexpr uint32_t BITMAP_WORDS = (MAX_CORES + 63) / 64;  // 向上取整

    // 运行时核心 ID，在 runtime 启动时初始化
    inline thread_local uint32_t this_core_id = 0;

    // Per-core SPSC 队列组 + active bitmap
    //
    // 生产者（任意核心）：enqueue 到自己核心对应的 SPSC 队列，然后 fetch_or 设置 bitmap
    // 消费者（拥有者核心）：exchange 清零 bitmap，只遍历有 bit 的队列
    //
    // 内存序保证：
    //   生产者: SPSC tail_.store(release) → bitmap fetch_or(release)
    //   消费者: bitmap exchange(acquire) → SPSC tail_.load(acquire)
    //   acquire/release 配对确保消费者看到 bit 时，对应数据已可见
    //
    // 延迟特性：
    //   enqueue 后 bitmap 位可能在下一次 advance() 才被消费者看到（最多一个轮询周期）
    //   正确性不受影响——数据已在 SPSC 中，只是消费者何时轮询到的问题
    //
    template<typename T, size_t CAPACITY = 1024>
    struct per_core_queue {
    private:
        std::array<spsc::queue<T, CAPACITY>, MAX_CORES> queues_;
        // 每个 word 覆盖 64 个核心，独占 cacheline 避免跨组 bounce
        // 128 核 = 2 个 word = 2 条 cacheline，producer bounce 范围限制在 64 核内
        struct alignas(64) bitmap_word { std::atomic<uint64_t> mask{0}; };
        std::array<bitmap_word, BITMAP_WORDS> active_mask_{};

    public:
        // 生产者调用（从任意核心）
        bool try_enqueue(T&& value) noexcept {
            if (queues_[this_core_id].try_enqueue(std::move(value))) {
                // 设置对应 bit，通知消费者
                // fetch_or 在 x86 上是 lock or，单条指令无重试
                uint32_t word = this_core_id / 64;
                uint64_t bit  = 1ULL << (this_core_id % 64);
                active_mask_[word].mask.fetch_or(bit, std::memory_order_release);
                return true;
            }
            return false;
        }

        bool try_enqueue(const T& value) noexcept {
            return try_enqueue(T(value));
        }

        // 消费者调用（从拥有此队列的核心）
        // 回调式批量消费，只遍历 bitmap 标记的活跃核心
        template<typename F>
        bool drain(F&& handler) {
            bool worked = false;
            for (uint32_t w = 0; w < BITMAP_WORDS; ++w) {
                uint64_t mask = active_mask_[w].mask.exchange(0, std::memory_order_acquire);
                while (mask) {
                    uint32_t bit = __builtin_ctzll(mask);
                    mask &= mask - 1;  // 清除最低 set bit
                    uint32_t core = w * 64 + bit;
                    T item;
                    while (queues_[core].try_dequeue(item)) {
                        handler(std::move(item));
                        worked = true;
                    }
                }
            }
            return worked;
        }

        // 单次 dequeue（兼容现有代码风格）
        // 注意：不清零 bitmap，允许后续 drain 重复检查（幂等）
        std::optional<T> try_dequeue() noexcept {
            for (uint32_t w = 0; w < BITMAP_WORDS; ++w) {
                uint64_t mask = active_mask_[w].mask.load(std::memory_order_acquire);
                while (mask) {
                    uint32_t bit = __builtin_ctzll(mask);
                    mask &= mask - 1;
                    uint32_t core = w * 64 + bit;
                    T item;
                    if (queues_[core].try_dequeue(item))
                        return std::move(item);
                }
            }
            return std::nullopt;
        }

        bool empty() const noexcept {
            for (uint32_t w = 0; w < BITMAP_WORDS; ++w) {
                if (active_mask_[w].mask.load(std::memory_order_relaxed) != 0)
                    return false;
            }
            return true;
        }
    };
}
```

### 3.2 this_core_id 初始化

在 `share_nothing.hh` 的 `run()` 中，线程启动时设置：

```cpp
template <typename ...scheduler_t>
auto run(global_runtime_t<scheduler_t...> *runtime, uint32_t core) {
    pump::core::this_core_id = core;  // 新增
    // ... 原有逻辑
}
```

### 3.3 scheduler 改动模式

以 task::scheduler 为例：

```cpp
// Before
core::mpsc::queue<_tasks::req*, 2048> tasks_request_q;

// After
core::per_core_queue<_tasks::req*, 2048> tasks_request_q;
```

`schedule()` 无需改动（接口一致）。`handle_tasks()` 适配：

```cpp
// Before
bool handle_tasks() {
    bool worked = false;
    while (const auto req = tasks_request_q.try_dequeue()) {
        req.value()->cb();
        delete req.value();
        worked = true;
    }
    return worked;
}

// After（两种风格任选）

// 风格 A：用 drain（推荐，一次遍历所有核心）
bool handle_tasks() {
    return tasks_request_q.drain([](auto* req) {
        req->cb();
        delete req;
    });
}

// 风格 B：保持 try_dequeue 循环（兼容性好）
bool handle_tasks() {
    bool worked = false;
    while (const auto req = tasks_request_q.try_dequeue()) {
        req.value()->cb();
        delete req.value();
        worked = true;
    }
    return worked;
}
```

### 3.4 spdk_ring 替换策略

KV 应用中的 `spdk_ring` 用于纯软件队列（不涉及 DMA），可以直接替换为 `per_core_queue`：

```cpp
// Before (kv::index::scheduler)
spdk_ring* update_req_queue;
void schedule(_update::req* req) {
    assert(0 < spdk_ring_enqueue(update_req_queue, (void**)&req, 1, nullptr));
}

// After
core::per_core_queue<_update::req*, 4096> update_req_queue;
void schedule(_update::req* req) {
    assert(update_req_queue.try_enqueue(req));
}
```

`advance()` 中的 `spdk_ring_dequeue` 批量出队改为 `drain()` 或循环 `try_dequeue()`。

**NVMe scheduler 的 spdk_ring**：同样是纯软件队列（先 dequeue 到 local_put_q/local_get_q 再提交 SPDK），可以替换。但需要适配 `spdk_ring_dequeue` 的批量语义（当前用 `tail_buffer()` 直接写入 ring_queue）。

---

## 4. 内存开销分析

### 4.1 per_core_queue 内存模型

每个 SPSC queue 占用：
- `CAPACITY * sizeof(T)` + 4 个 cache line（head, tail, head_cache, tail_cache, buffer 对齐）
- 对于 `T = req*`（8 bytes），CAPACITY = 2048：`2048 * 8 + 256 = ~16.5 KB`

per_core_queue 占用：`MAX_CORES * 单个 SPSC + bitmap` = `128 * 16.5 KB + 64B ≈ 2 MB`

### 4.2 实际开销估算

以 32 核服务器为例（典型服务端配置）：

| 组件 | per_core_queue 数 | 每个 per_core_queue | 总计 |
|------|-------------------|--------------------|----- |
| task scheduler (×32) | 2 × 32 = 64 | 128 × 16KB = 2MB | 128MB |
| net session_scheduler (×32) | 3 × 32 = 96 | 2MB | 192MB |
| net conn_scheduler (×1) | 1 | 2MB | 2MB |
| nvme scheduler (×32) | 2 × 32 = 64 | 2MB | 128MB |
| kv index (×32) | 5 × 32 = 160 | 2MB | 320MB |
| kv batch (×1) | 3 | 2MB | 6MB |
| kv fs (×1) | 3 | 2MB | 6MB |
| **总计** | **391** | | **~782MB** |

MAX_CORES=128、CAPACITY=2048 时内存偏大。需要优化。

### 4.3 内存优化策略

**方案 1：减小 CAPACITY（推荐）**

大部分 scheduler 的请求突发深度远小于 2048。降到 256 或 512：
- CAPACITY=256：每个 SPSC = `256 × 8B + 256B = ~2.3KB`，per_core_queue = `128 × 2.3KB ≈ 300KB`
- 32 核 391 个队列 × 300KB ≈ **114MB**，可接受

**方案 2：运行时核心数分配**

只为实际核心数分配 SPSC 队列，不浪费未使用核心的空间：

```cpp
template<typename T, size_t CAPACITY = 1024>
struct per_core_queue {
private:
    std::unique_ptr<spsc::queue<T, CAPACITY>[]> queues_;
    uint32_t num_cores_;
    alignas(64) std::array<std::atomic<uint64_t>, BITMAP_WORDS> active_mask_{};
public:
    explicit per_core_queue(uint32_t num_cores)
        : queues_(std::make_unique<spsc::queue<T, CAPACITY>[]>(num_cores))
        , num_cores_(num_cores) {}
};
```

32 核 × 2.3KB = 74KB/队列，391 个 ≈ **28MB**。代价是多一次指针间接寻址。

**方案 3：编译期 MAX_CORES + 减小 CAPACITY（最简单）**

```cpp
inline constexpr uint32_t MAX_CORES = 128;
// 各 scheduler 按实际需要选择 CAPACITY
core::per_core_queue<_tasks::req*, 512> tasks_request_q;  // task 高频，大一些
core::per_core_queue<_timer::req*, 128> timer_request_q;  // timer 低频，小一些
```

**推荐方案 3**，理由：
1. bitmap drain() 的外层循环次数在编译期确定（BITMAP_WORDS = 2），编译器可展开
2. 无间接寻址开销
3. 各队列按实际突发深度选择 CAPACITY，避免一刀切的浪费
4. 未使用核心的 SPSC 永远为空，bitmap 不会设置对应 bit，不会被遍历

---

## 5. advance() 轮询优化：active bitmap

### 5.1 问题

原来 dequeue 1 个队列，现在遍历 N 个（N = 核心数）。服务端场景核心数 32-128，如果每个 scheduler 有多种请求队列（如 kv::index 有 5 种），一次 advance() 要检查 5 × 128 = 640 个 SPSC 队列。即使空队列的 `try_dequeue` 只需 1 次本地比较（tail_cache），640 次仍非零开销，且在主循环中每秒调用数百万次。

### 5.2 方案：per_core_queue 内建 active bitmap

bitmap 已集成到 `per_core_queue`（见 §3.1），消费者只遍历有 bit 的队列。

**消费者侧 drain() 开销分析：**

| 场景 | 操作 | 开销 |
|------|------|------|
| 无任何生产者活跃 | `exchange(0)` × BITMAP_WORDS | 2 次 atomic exchange（128 核 = 2 个 word） |
| k 个核心有数据 | `exchange(0)` + k 次 `ctzll` + k 次 SPSC drain | O(k) 精确遍历 |

对比无 bitmap 的遍历：每次 advance() 无条件检查 128 个 SPSC 队列 = 128 次 `head == tail_cache_` 比较 + 若干 cache miss。bitmap 将其压缩为 2 次 atomic load。

**生产者侧 fetch_or 开销分析：**

`fetch_or` 引入了一个 atomic RMW，与 MPSC CAS 的区别：

| 操作 | 指令 | 重试 | cacheline 竞争 |
|------|------|------|---------------|
| MPSC CAS (tail_) | `lock cmpxchg` | 失败时无限重试 | 所有生产者争同一 tail_ |
| bitmap fetch_or | `lock or` | 无重试（一次成功） | 所有生产者争同一 bitmap word |
| SPSC tail store | `mov` + fence | 无 | 每个生产者独占自己的 tail_ |

bitmap 的 `fetch_or` 确实引入了与 MPSC 类似的 cacheline bouncing，但有本质区别：
1. **无重试**：`lock or` 硬件保证一次完成，不像 CAS 可能 spin
2. **频率更低**：只在实际入队成功时设置一次，而 MPSC CAS 是入队操作本身
3. **可接受**：这是为了让消费者从 O(N) 降到 O(k) 的 trade-off

### 5.3 try_dequeue 的 bitmap 语义

`try_dequeue()` 只 load bitmap 不清零——因为它只取一个元素，队列中可能还有剩余。bitmap 的清零由 `drain()` 负责。

存在的 race：drain() `exchange(0)` 清零后，某个 SPSC 中仍有残余数据（生产者在 exchange 之后又 enqueue 并 fetch_or）。下一轮 drain() 会处理。最坏延迟 = 一个 advance() 周期（微秒级），不影响正确性。

### 5.4 >64 核支持

使用 `std::array<std::atomic<uint64_t>, BITMAP_WORDS>` 数组，每个 word 覆盖 64 核。`BITMAP_WORDS = (MAX_CORES + 63) / 64`，128 核 = 2 个 word。drain() 的外层循环固定 2 次迭代，编译器可完全展开。

---

## 6. 实施计划

分为两大阶段：先完成框架层（src/pump + src/env），验证通过后再迁移应用层（apps/kv）。

### Phase 0：基础设施

| 步骤 | 内容 | 文件 |
|------|------|------|
| 0.1 | 实现 `per_core_queue<T, CAPACITY>`（含 bitmap） | `src/pump/core/lock_free_queue.hh` |
| 0.2 | 添加 `thread_local this_core_id` | `src/pump/core/lock_free_queue.hh` |
| 0.3 | 在 `share_nothing.hh` 的 `run()` 中初始化 `this_core_id` | `src/env/runtime/share_nothing.hh` |

### Phase 1：框架 scheduler 迁移

| 步骤 | 内容 | 文件 | 验证 |
|------|------|------|------|
| 1.1 | task::scheduler 迁移 | `src/env/scheduler/task/tasks_scheduler.hh` | 现有 task 测试 |
| 1.2 | net::session_scheduler 迁移（io_uring） | `src/env/scheduler/net/io_uring/scheduler.hh` | echo/rpc 测试 |
| 1.3 | net::session_scheduler 迁移（epoll） | `src/env/scheduler/net/epoll/scheduler.hh` | 同上 |
| 1.4 | net::conn_scheduler / connect_scheduler 迁移 | `io_uring/connect_scheduler.hh`, `epoll/connect_scheduler.hh` | 连接测试 |

### Phase 2：框架验证（已完成）

| 步骤 | 内容 | 状态 |
|------|------|------|
| 2.1 | 编译验证，无新增编译错误 | DONE |
| 2.2 | test.sequential — 全部 PASS | DONE |
| 2.3 | test.when_any — 13/13 PASS | DONE |
| 2.4 | test.rpc — 54/54 PASS | DONE |
| 2.5 | test.rpc_concurrent — PASS | DONE |
| 2.6 | test.rpc_timeout — PASS | DONE |
| 2.7 | test.rpc_disconnect — PASS | DONE |

**Phase 0-2 完成，框架层迁移结束。apps/kv 仍使用 spdk_ring，可正常编译运行。**

---

### Phase 3：NVMe + KV 应用 scheduler 迁移（独立阶段）

NVMe scheduler 的构造函数接收外部传入的 `spdk_ring*`（由 `apps/kv/runtime/init.hh` 创建），
改动构造函数会影响 KV init 代码，因此 NVMe 与 KV 放在同一阶段一起改。

| 步骤 | 内容 | 文件 |
|------|------|------|
| 3.1 | NVMe scheduler：替换 spdk_ring 为 per_core_queue | `src/env/scheduler/nvme/scheduler.hh` |
| 3.2 | KV init：删除 spdk_ring_create，适配新构造函数 | `apps/kv/runtime/init.hh` |
| 3.3 | batch scheduler | `apps/kv/batch/scheduler.hh` |
| 3.4 | index scheduler | `apps/kv/index/scheduler.hh` |
| 3.5 | fs scheduler | `apps/kv/fs/scheduler.hh` |

这些 scheduler 都使用 `spdk_ring`，替换模式相同：
- `spdk_ring_enqueue` → `per_core_queue::try_enqueue`
- `spdk_ring_dequeue` + 循环处理 → `per_core_queue::drain(handler)`
- 删除 `spdk_ring_create` / `spdk_ring_free`

**NVMe scheduler 注意**：当前用 `spdk_ring_dequeue` 批量出队到 `ring_queue` 的 `tail_buffer()`，
是 zero-copy 批量操作。改为 per_core_queue 后需要逐个 dequeue 到 local queue。

### Phase 4：清理与文档

| 步骤 | 内容 |
|------|------|
| 4.1 | 更新 CLAUDE.md 自建 Scheduler 模板中的队列说明 |
| 4.2 | KV benchmark 端到端回归测试 |
| 4.3 | 如果 `mpsc::queue` 不再被使用，标记为 deprecated 或删除 |

---

## 7. 不迁移的队列

| 队列 | 原因 |
|------|------|
| `preemptive_scheduler` 的 MPMC 队列 | 语义需要多消费者 |
| `conn_fd_q` / `session_q`（MPMC） | accept 线程 produce，多核 consume |
| `recv_cache` 的 `recv_q` / `ready_q`（local） | 已经无原子，同线程操作 |

---

## 8. 风险与缓解

### 8.1 this_core_id 未初始化

**风险**：如果 schedule() 在 runtime run() 之前被调用（如初始化阶段），this_core_id 为默认值 0，所有请求挤到 queue[0]。

**缓解**：
- 默认值 0 不会导致正确性问题，只是退化为"所有初始化阶段请求走同一个 SPSC"
- 初始化阶段通常是单线程，无并发问题
- 可添加 `assert(this_core_id_initialized)` 在 debug 模式下检测

### 8.2 bitmap fetch_or 的 cacheline bouncing

**风险**：多核同时入队到同一 scheduler 时，`active_mask_` 的 `fetch_or` 引起 cacheline bouncing，部分抵消 SPSC 消除 CAS 的收益。

**缓解**：
- `fetch_or`（`lock or`）无重试，最坏开销是一次 cache miss（~200 cycles），远低于 MPSC CAS 争用时的 spin 延迟（可达数千 cycles）
- 同一核心连续多次 enqueue 到同一 scheduler 时，`fetch_or` 命中本地缓存（bit 已设置，无 invalidation）
- 128 核时 bitmap 分为 2 个 word（每个覆盖 64 核），高低 64 核不互相 bounce
- 如果 profiling 证明 bitmap bouncing 是瓶颈，可改为 per-core flag（`alignas(64) atomic<bool> active_flags_[MAX_CORES]`），以空间换消除 RMW，但当前方案已足够

### 8.3 NVMe scheduler 批量出队语义变化

**风险**：当前 `spdk_ring_dequeue` 一次取多个到 `ring_queue` 的连续内存，per_core_queue 不支持这种 zero-copy 批量操作。

**缓解**：
- 为 per_core_queue 添加 `drain_to(ring_queue&, max_count)` 方法
- 或在 NVMe scheduler 中改为逐个 dequeue（simple first，benchmark 后再优化）

### 8.4 自建 Scheduler 模板更新

**风险**：用户按旧模板自建 scheduler 仍用 MPSC。

**缓解**：Phase 4 更新文档。旧代码仍能编译运行（MPSC 不删除），只是性能不最优。

---

## 9. 性能预期

| 指标 | MPSC (当前) | Per-Core SPSC + bitmap (迁移后) |
|------|------------|-------------------------------|
| 入队延迟 | CAS 循环，高争用时 ~100-500ns | SPSC store ~10ns + fetch_or ~20-40ns ≈ 30-50ns |
| 入队吞吐（32 核同时投递） | CAS 串行化，争用严重 | SPSC 无争用；bitmap 有 cacheline bounce 但无重试 |
| 出队延迟 (drain) | 1 个队列，~20ns | exchange × 2 + ctzll × k 次 SPSC drain，O(k) |
| 出队空转 (无数据) | dequeue 1 次 ~10ns | exchange × 2 = ~10ns（比遍历 128 个 SPSC 快得多）|
| 缓存行 bounce | tail_ 被多核争抢 | SPSC tail_ 无争用；bitmap word 有 bounce 但影响小 |
| 内存（32 核，方案 3） | ~1MB | ~30-120MB（取决于 CAPACITY 选择） |

**核心收益**：
1. **入队端**消除 CAS 重试，P99 延迟从不可预测变为确定性（~50ns 上界）
2. **出队端**通过 bitmap 跳过非活跃核心，32-128 核场景下 drain() 只触碰有数据的队列
3. 入队多付出的 fetch_or 开销（~20ns）换来出队端从 O(N) 降到 O(k) + O(1) bitmap check

---

## 10. 验证策略

1. **单元测试**：per_core_queue 的多线程正确性测试（N producer + 1 consumer）
2. **集成测试**：现有 echo/rpc/concurrent 测试全部通过
3. **压力测试**：多核高并发 schedule 场景，对比迁移前后延迟分布
4. **回归测试**：KV benchmark 端到端性能
