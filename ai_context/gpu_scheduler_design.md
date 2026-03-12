# GPU Scheduler 需求与设计文档

## 1. 目标

为 PUMP 框架新增 **GPU (CUDA) scheduler**，基于 CUDA Driver API 的纯轮询模型，实现 GPU 计算的异步调度。与 NVMe scheduler / io_uring 同构：submit + poll，零额外线程，完全融入 PUMP 的 share_nothing 架构。

### 1.1 设计约束

| 约束 | 说明 |
|------|------|
| 仅 CUDA 后端 | 不抽象多 GPU API，直接使用 CUDA Driver API（`cu*` 系列） |
| 纯轮询，无回调 | 不使用 `cudaStreamAddCallback` / `cudaLaunchHostFunc`，不引入 CUDA 内部线程 |
| 单线程模型 | 每个 scheduler 实例绑定单核运行，拥有独立的 CUDA context/stream/memory 资源 |
| 无锁 | 跨域请求通过 per_core::queue 投递，scheduler 内部单线程处理 |
| 框架层只管调度 | kernel 编写是应用层的事，框架负责 launch + poll + 资源管理 |

### 1.2 不包含的范围

- kernel 编写辅助 / DSL
- CUDA Graph 自动 capture（可作为后续优化）
- GPU Direct Storage / GPU Direct RDMA（可作为后续优化）
- 多 GPU 间通信（NVLink / P2P，后续扩展）
- Unified Memory / managed memory 自动迁移

### 1.3 CUDA Driver API vs Runtime API

| 特性 | Runtime API (`cuda*`) | Driver API (`cu*`) |
|------|----------------------|-------------------|
| 线程管理 | 内部线程池（callback 时） | 无额外线程 |
| Context 管理 | 隐式（per-thread default） | 显式（`cuCtxCreate`） |
| 模块加载 | 编译期链接 | 运行时加载 PTX/cubin（`cuModuleLoad`） |
| 初始化 | `cudaSetDevice` | `cuInit` + `cuDeviceGet` + `cuCtxCreate` |
| 控制粒度 | 高层封装 | 完全控制 |

选择 Driver API 因为：
1. 不产生额外线程 — 不抢 share_nothing 的核心
2. 显式 CUDA context 管理 — 可以绑定到特定 PUMP 核心
3. 与 PUMP 的 polling 模型完全匹配

---

## 2. 架构概览

### 2.1 在 PUMP 中的位置

```
src/env/scheduler/
├── task/           ← CPU 计算
├── nvme/           ← 存储 IO
├── net/            ← 网络 (tcp/udp/kcp/rpc)
└── cuda/           ← GPU 计算（新增）
    ├── scheduler.hh        ← gpu_scheduler 主体
    ├── sender.hh           ← launch / memcpy sender
    ├── memory_pool.hh      ← 显存池化
    └── common/
        ├── types.hh        ← req, device_ptr, 基础类型
        └── error.hh        ← CUDA 错误处理
```

### 2.2 执行模型

```
PUMP 核心线程 (advance loop)
  │
  ├─ task_scheduler->advance()
  ├─ nvme_scheduler->advance()
  ├─ tcp_scheduler->advance()
  └─ gpu_scheduler->advance()
       │
       ├─ 1. req_q.drain() — 取出新请求
       │    ├─ cuMemcpyHtoDAsync(...)    ← 提交 H2D 传输
       │    ├─ cuLaunchKernel(...)       ← 提交 kernel
       │    ├─ cuMemcpyDtoHAsync(...)    ← 提交 D2H 传输
       │    └─ cuEventRecord(event, stream)  ← 打完成标记
       │
       └─ 2. poll pending — 轮询已提交的操作
            ├─ cuEventQuery(event) == CUDA_SUCCESS
            │    → cb(result)  ← op_pusher 继续 pipeline
            └─ cuEventQuery(event) == CUDA_ERROR_NOT_READY
                 → 继续等待
```

与 NVMe 完全同构：

| | 提交 | 完成检测 | 额外线程 |
|---|---|---|---|
| NVMe (io_uring) | `io_uring_submit` | `io_uring_peek_cqe` | 无 |
| GPU (CUDA Driver) | `cuLaunchKernel` | `cuEventQuery` | 无 |

---

## 3. Sender API 设计

### 3.1 基础 sender

```cpp
namespace pump::scheduler::cuda {

// H2D 传输：host → device
// 输入：无（host_ptr 和 size 作为参数）
// 输出：device_ptr
gpu::memcpy_h2d(sched, host_ptr, size)

// D2H 传输：device → host
// 输入：无（dev_ptr, host_dst, size 作为参数）
// 输出：void（数据写入 host_dst）
gpu::memcpy_d2h(sched, dev_ptr, host_dst, size)

// D2D 传输：device → device
// 输入：无
// 输出：device_ptr (dst)
gpu::memcpy_d2d(sched, src_ptr, dst_ptr, size)

// kernel launch
// 输入：无
// 输出：void
gpu::launch(sched, kernel_func, grid_dim, block_dim, shared_mem_bytes, args...)

// 显存分配（从 pool）
// 输入：无
// 输出：device_ptr
gpu::alloc(sched, size)

// 显存释放（归还 pool）
// 输入：无
// 输出：void
gpu::free(sched, dev_ptr, size)
}
```

### 3.2 使用示例

```cpp
// 基础用法：上传 → 计算 → 下载
just(host_data, size)
    >> gpu::memcpy_h2d(gpu_sched)
    >> gpu::launch(gpu_sched, vector_add, {N/256}, {256}, 0, /* extra args */)
    >> gpu::memcpy_d2h(gpu_sched, host_output, size)
    >> then([](){ /* GPU 完成，回到 PUMP 线程 */ })
    >> submit(ctx);

// 与其他 scheduler 交互
tcp::recv(session)
    >> then([](net_frame&& frame) { return parse(frame); })
    >> gpu::memcpy_h2d(gpu_sched)
    >> gpu::launch(gpu_sched, inference, grid, block)
    >> gpu::memcpy_d2h(gpu_sched, output_buf, output_size)
    >> on(task_sched.as_task())
    >> then([](){ return build_response(); })
    >> tcp::send(session, resp, resp_len)
    >> submit(ctx);

// 并发多 kernel
for_each(batches) >> concurrent(N)
    >> gpu::memcpy_h2d(gpu_sched)
    >> gpu::launch(gpu_sched, process, grid, block)
    >> gpu::memcpy_d2h(gpu_sched, outputs[i], size)
    >> reduce();
```

### 3.3 设计决策：细粒度 vs 粗粒度 sender

**方案 A：每个 CUDA 操作一个 sender（细粒度）**

```cpp
just()
    >> gpu::memcpy_h2d(sched, host, size)   // sender 1 → event poll
    >> gpu::launch(sched, kernel, ...)       // sender 2 → event poll
    >> gpu::memcpy_d2h(sched, host, size)   // sender 3 → event poll
```

每步一次 event poll，3 次 dequeue/enqueue 往返。延迟 = 3 × polling interval。

**方案 B：组合操作（粗粒度）**

```cpp
just()
    >> gpu::run(sched, [](cuda_stream& stream, auto& mem_pool) {
        auto* dev = mem_pool.alloc(size);
        cuMemcpyHtoDAsync(dev, host, size, stream);
        cuLaunchKernel(kernel, ..., stream);
        cuMemcpyDtoHAsync(host_out, dev, size, stream);
        mem_pool.free(dev, size);
    })
    // 一次性提交全部 CUDA 操作到同一 stream，只 poll 一次 event
```

一次提交，一次 poll。用户在 lambda 里直接操作 stream 和 memory pool。

**方案 C：两者都提供**

- `gpu::run(sched, lambda)` — 粗粒度，用户完全控制，lambda 内自由组合 CUDA 调用
- `gpu::launch` / `gpu::memcpy_*` — 细粒度，简单场景更直观

**建议采用方案 C**，以 `gpu::run` 为主力（大多数场景），细粒度 sender 为语法糖。

---

## 4. Scheduler 内部设计

### 4.1 六组件

#### 4.1.1 req

```cpp
struct req {
    // 用户提交的 CUDA 操作（lambda 闭包）
    std::move_only_function<void(gpu_env&)> work;
    // 完成回调 → op_pusher 推进 pipeline
    std::move_only_function<void()> cb;
};
```

#### 4.1.2 核心资源

```cpp
struct gpu_scheduler {
    // CUDA 资源（每个 scheduler 实例独占）
    CUcontext   cu_ctx;
    CUstream    stream;         // 主 stream（简单场景）
    CUstream    streams[N];     // stream pool（并发场景）
    memory_pool mem_pool;       // 显存池

    // 请求队列
    core::per_core::queue<req*> req_q;

    // 飞行中的操作
    struct pending_op {
        CUevent event;
        std::move_only_function<void()> cb;
    };
    std::vector<pending_op> pending;

    // event 池（避免反复创建/销毁）
    std::vector<CUevent> event_pool;
};
```

#### 4.1.3 advance()

```cpp
bool advance() {
    bool did_work = false;

    // 1. 提交新请求
    did_work |= req_q.drain([&](req* r) {
        CUstream s = acquire_stream();
        CUevent  e = acquire_event();

        gpu_env env{s, mem_pool, device, device_id};
        r->work(env);               // 用户 lambda：通过 gpu_env 提交 CUDA 操作
        cuEventRecord(e, s);        // 打完成标记

        pending.push_back({e, std::move(r->cb)});
        delete r;
    });

    // 2. 轮询完成
    for (auto it = pending.begin(); it != pending.end(); ) {
        if (cuEventQuery(it->event) == CUDA_SUCCESS) {
            it->cb();               // → op_pusher<pos+1>::push_value
            release_event(it->event);
            it = pending.erase(it);
            did_work = true;
        } else {
            ++it;
        }
    }

    return did_work;
}
```

### 4.2 CUDA Context 绑定

CUDA Driver API 要求每个线程绑定一个 `CUcontext`。

```
初始化时：
  cuInit(0)
  cuDeviceGet(&device, gpu_id)
  cuCtxCreate(&cu_ctx, 0, device)

advance() 首次调用时（在 PUMP 核心线程上）：
  cuCtxSetCurrent(cu_ctx)    // 绑定到当前线程
```

**单 GPU 多 scheduler 实例**：共享同一个 `CUcontext`（CUDA context 支持多线程共享，只要每个线程 `cuCtxSetCurrent`）。

**多 GPU**：每个 GPU 一个 `CUcontext`，每个 gpu_scheduler 实例绑定到特定 GPU。

### 4.3 Stream 管理策略

| 策略 | 适用场景 |
|------|---------|
| 单 stream | 简单顺序执行，一个 scheduler 实例只处理串行请求 |
| stream pool（固定 N 条） | 支持 `concurrent(N)` — 每条并发 pipeline 分配一条 stream |
| 动态分配 | 按需创建，用完归还 pool |

**建议初期用单 stream**，后续按需扩展到 stream pool。

### 4.4 Event 池化

`cuEventCreate` / `cuEventDestroy` 有开销，需要池化复用：

```cpp
CUevent acquire_event() {
    if (!event_pool.empty()) {
        auto e = event_pool.back();
        event_pool.pop_back();
        return e;
    }
    CUevent e;
    cuEventCreate(&e, CU_EVENT_DISABLE_TIMING);  // 不需要计时，更快
    return e;
}

void release_event(CUevent e) {
    event_pool.push_back(e);
}
```

---

## 5. 显存池化

### 5.1 设计

与 scope slab 类似的分级池化，但管理的是 GPU 显存：

```cpp
struct memory_pool {
    // 按大小分级：256B, 1K, 4K, 16K, 64K, 256K, 1M, 4M, 16M, 64M
    static constexpr size_t NUM_CLASSES = 10;
    std::vector<CUdeviceptr> free_lists[NUM_CLASSES];

    CUdeviceptr alloc(size_t bytes);   // 从对应级别的 free_list 取，没有则 cuMemAlloc
    void free(CUdeviceptr ptr, size_t bytes);  // 归还到 free_list
    void trim();  // 释放多余的缓存显存（显存压力大时）
};
```

### 5.2 要点

- `cuMemAlloc` 开销 ~100μs，必须池化
- 每个 scheduler 实例独占一个 pool（单线程，无锁）
- 可提供 RAII wrapper：`gpu::scoped_mem` — 自动 alloc/free
- 大块显存（>64M）不池化，直接 alloc/free

---

## 6. Kernel 加载

CUDA Driver API 需要手动加载 kernel（不像 Runtime API 自动链接）：

```cpp
// 编译：nvcc --ptx my_kernel.cu -o my_kernel.ptx
// 或：nvcc --cubin my_kernel.cu -o my_kernel.cubin

// 加载
CUmodule module;
cuModuleLoad(&module, "my_kernel.ptx");    // 或 cuModuleLoadData(embedded)

// 获取 kernel 函数
CUfunction kernel;
cuModuleGetFunction(&kernel, module, "vector_add");

// launch 时传 CUfunction
gpu::launch(sched, kernel, grid, block, shared_mem, arg1, arg2, ...);
```

**kernel 加载是应用层职责**，scheduler 只接收 `CUfunction` 指针。框架可提供辅助工具但不强制。

---

## 7. 错误处理

### 7.1 CUDA 错误分类

| 错误类型 | 处理方式 |
|---------|---------|
| launch 失败（参数错误、资源不足） | `cuLaunchKernel` 返回错误码 → 立即抛异常 → `any_exception` 捕获 |
| kernel 运行时错误（越界、assert） | `cuEventQuery` / 下次 CUDA 调用时报告 → sticky error |
| 显存不足 | `cuMemAlloc` 返回 `CUDA_ERROR_OUT_OF_MEMORY` → 可 trim pool 后重试或抛异常 |
| GPU 挂死 | `cuEventQuery` 永远返回 NOT_READY → 需要超时机制 |

### 7.2 异常类型

```cpp
struct cuda_error : std::runtime_error {
    CUresult code;
    cuda_error(CUresult code, const char* msg);
};
```

可用 `catch_exception<cuda_error>` 精确捕获。

---

## 8. 多 GPU 支持

```cpp
// 每个 GPU 一个 scheduler 实例
// 初始化时：
for (int i = 0; i < num_gpus; i++) {
    cuDeviceGet(&devices[i], i);
    cuCtxCreate(&contexts[i], 0, devices[i]);
    gpu_schedulers[i] = new gpu_scheduler(contexts[i]);
}

// 路由：
auto gpu_id = data.shard_id % num_gpus;
just(data) >> gpu::run(gpu_scheds[gpu_id], work) >> ...;
```

与 index scheduler 的 hash 分片路由完全一致。

---

## 9. Kernel Batching ✅ 已决定：应用层实现

GPU 最怕小 kernel 频繁 launch（launch overhead ~5-10μs）。

### 9.1 为什么不在框架层实现

`gpu::run` 的 req 使用类型擦除的 `std::move_only_function<void(gpu_env&)>`，scheduler 的 `advance()` 看到的是不透明 lambda，无法判断：
- 是否是同一个 kernel
- 参数能否拼接
- 输出怎么拆分

与 KV I/O batching 的本质区别：

| | KV I/O Batching (fs scheduler) | GPU Kernel Batching |
|---|---|---|
| 合并逻辑 | 通用的：append pages to span_list | **应用特定的**：取决于 kernel 输入布局 |
| Leader 工作 | write_data(merged_spans) | 需要重组数据、launch 更大的 kernel |
| Follower 结果 | 空标记 | 需要从 batched output 中**提取各自的子结果** |
| 框架可通用抽象 | 可以 | **不可以** |

### 9.2 应用层实现模式

按 KV fs scheduler 的 Leader/Follower 模式，用户写自己的 batch scheduler：

```
inference_batch_scheduler
  handle_batch():
    1. drain 多个请求（per_core::queue）
    2. 第一个 → leader
    3. 后续 → follower (存入 leader_res.followers)
    4. 合并输入数据（应用特定逻辑）
    5. leader cb(variant<leader_res*, follower_res>)

leader 的 pipeline:
  >> gpu::run(gpu_sched, [res](gpu_env& env) {
      // 上传 batched_input → launch batched_kernel → 下载 batched_output
  })
  >> then([](leader_res* res) {
      // 拆分结果到各 follower 的 output 缓冲区（应用特定逻辑）
  })
  >> notify_followers(res)
```

框架已提供所有构建 batch scheduler 的工具：
- 六组件 scheduler 模式 + per_core::queue（跨域投递）
- variant 多路返回（leader/follower/failed 分支）
- `gpu::run` 作为执行原语
- `if constexpr` + `flat()` 做 pipeline 分支处理

---

## 10. 实现路线

### Phase 1：最小可用（MVP）

- [ ] gpu_scheduler 骨架（req_q, advance, pending）
- [ ] CUDA context / stream / event 管理
- [ ] `gpu::run(sched, lambda)` sender — 粗粒度，用户 lambda 内自由操作
- [ ] op_pusher / compute_sender_type 特化
- [ ] 基本错误处理（launch 失败 → 异常）
- [ ] 简单示例：vector_add（H2D → kernel → D2H → 验证结果）

### Phase 2：资源管理

- [ ] 显存 memory_pool（分级池化）
- [ ] event 池化
- [ ] `gpu::alloc` / `gpu::free` sender
- [ ] RAII scoped_mem

### Phase 3：细粒度 sender

- [ ] `gpu::launch` sender
- [ ] `gpu::memcpy_h2d` / `gpu::memcpy_d2h` sender
- [ ] stream pool（支持 concurrent）

### Phase 4：高级特性

- [x] 多 GPU 路由 — 已有 runtime 模块支持，无需新代码
- [x] kernel batching — 已决定：应用层实现（见 §9）
- [ ] CUDA Graph capture/replay（见 §13）
- ~~GPU Direct 集成（GDS / RDMA）~~ — 暂不实现

---

## 11. 与现有 Scheduler 的对比

| 特性 | task_scheduler | nvme_scheduler | gpu_scheduler |
|------|---------------|----------------|---------------|
| 计算位置 | CPU | NVMe 控制器 | GPU |
| 提交方式 | 函数调用 | io_uring submit | cuLaunchKernel |
| 完成检测 | 立即完成 | io_uring peek | cuEventQuery |
| 资源管理 | 无 | fd / io_uring | stream / 显存 / event |
| 典型延迟 | ns~μs | 10~100μs | 10μs~ms |
| Batching | 无需 | 无需 | 应用层实现（§9） |
| Graph 优化 | N/A | N/A | 框架层支持（§13） |

---

## 12. 设计决策记录

### 12.1 gpu::run 的 lambda 签名 ✅ 已决定

**结论：采用聚合对象 `gpu_env&`。**

```cpp
gpu::run(sched, [](gpu_env& env) {
    auto* dev = env.alloc(size);
    env.memcpy_h2d(dev, host, size);
    env.launch(kernel, {gx,gy,gz}, {bx,by,bz}, shared, arg1, arg2);
    env.memcpy_d2h(host_out, dev, size);
    env.free(dev, size);
})
```

`gpu_env` 结构：

```cpp
struct gpu_env {
    CUstream     stream;      // scheduler 为本次请求分配的 stream
    memory_pool& mem_pool;    // 显存池
    CUdevice     device;      // GPU 设备
    int          device_id;   // GPU 编号

    // 便捷方法（包装底层 cu* API）
    CUdeviceptr alloc(size_t bytes);
    void free(CUdeviceptr, size_t bytes);
    void memcpy_h2d(CUdeviceptr dst, const void* src, size_t bytes);
    void memcpy_d2h(void* dst, CUdeviceptr src, size_t bytes);
    void launch(CUfunction f, dim3 grid, dim3 block, unsigned shared, auto&&... args);

    // 将来可扩展：pinned_memory_pool, device properties, etc.
};
```

**决策理由：**
- 可扩展：将来加 pinned memory、stream priority 等不改 lambda 签名
- `env.launch()` 封装 `cuLaunchKernel` 的 `void** args`，提供类型安全
- 用户需要底层 API 时，`env.stream` 随时可用
- 不叫 `cuda_context` 是因为 PUMP 的 "context" 已有特定含义（数据栈），避免歧义

---

### 12.2 单 GPU 多 scheduler 实例 vs 单实例 ✅ 已决定

**结论：单实例。每个 GPU 绑定一个 gpu_scheduler 实例，其他核心通过 per_core::queue 跨域投递。**

**决策理由：**
- GPU 是共享资源，多实例不会带来真正的并行（GPU 内部自己调度）
- 显存统一管理，避免碎片（N 个 pool = N 倍碎片）
- 跨域投递开销（per_core::queue, ns 级）相比 GPU kernel 延迟（μs~ms）可忽略
- 简单：1 个 CUDA context、1 个显存 pool、1 组 stream
- 多 GPU 场景：每个 GPU 一个 scheduler 实例，按 GPU ID 路由

### 12.3 kernel 参数传递方式 ✅ 已决定

**结论：variadic template 自动构建 `void*[]` 数组。**

```cpp
// gpu_env::launch 实现
template<typename... Args>
void launch(CUfunction f, dim3 grid, dim3 block, unsigned shared, Args&&... args) {
    void* params[] = { const_cast<void*>(static_cast<const void*>(&args))... };
    cuLaunchKernel(f, grid.x, grid.y, grid.z, block.x, block.y, block.z,
                   shared, stream, params, nullptr);
}

// 用户侧
env.launch(kernel, {N/256}, {256}, 0, dev_a, dev_b, dev_out, n);
```

**注意：** 参数类型必须和 kernel 声明一致（size 匹配），否则运行时 UB。这与 CUDA Runtime API 的 `<<<>>>` 本质相同 — kernel 是运行时加载的 `CUfunction`，无法编译期检查参数类型。

### 12.4 pinned memory ✅ 已决定

**结论：不默认使用。gpu_env 提供 pinned memory 的 alloc/free 方法，在示例中主动使用以教育用户。**

```cpp
// gpu_env 提供的 pinned memory API
void* alloc_pinned(size_t bytes);       // cuMemAllocHost，从 pinned pool 分配
void free_pinned(void* ptr, size_t bytes);  // 归还 pinned pool
```

```cpp
// 示例中展示用法
gpu::run(sched, [](gpu_env& env) {
    // pinned host memory — DMA 带宽翻倍，保证真正异步传输
    auto* host_buf = env.alloc_pinned(size);
    memcpy(host_buf, src_data, size);

    auto* dev = env.alloc(size);
    env.memcpy_h2d(dev, host_buf, size);
    env.launch(kernel, grid, block, 0, dev, n);
    env.memcpy_d2h(host_buf, dev, size);
    env.free(dev, size);
    // host_buf 在 callback 后由用户 free_pinned
})
```

**决策理由：**
- pinned memory 锁住物理页，过度使用影响 OS 和其他核心
- 不默认用 → 用户显式选择，理解代价
- 示例中主动使用 → 用户知道这个能力存在以及怎么用
- pinned pool 池化复用，避免反复 cuMemAllocHost（~100μs）

### 12.5 Stream 优先级 ✅ 已决定

**结论：推迟到 Phase 3（stream pool）。** 初期单 stream，优先级无意义。引入 stream pool 后再考虑是否暴露 `cuStreamCreateWithPriority`。

---

## 13. CUDA Graph — 框架层优化

### 13.1 原理

CUDA Graph 把一组 CUDA 操作录制成一个图，然后作为整体重放。减少每次请求的 CPU 端开销。

```
常规 gpu::run（每次请求）：
  cuMemcpyHtoDAsync()   ~2μs CPU overhead
  cuLaunchKernel()      ~5μs CPU overhead
  cuMemcpyDtoHAsync()   ~2μs CPU overhead
  cuEventRecord()       ~1μs
  ─────────────────
  总计 ~10μs CPU 开销

CUDA Graph（每次请求）：
  cuGraphLaunch()       ~1-2μs CPU overhead（替代上面全部）
  cuEventRecord()       ~1μs
  ─────────────────
  总计 ~2-3μs（减少 70-80%）
  首次额外开销：capture + instantiate ~100μs（一次性）
```

### 13.2 三阶段

```cpp
// 1. Capture — 录制操作（不执行）
cuStreamBeginCapture(stream, CU_STREAM_CAPTURE_MODE_GLOBAL);
cuMemcpyHtoDAsync(dev_a, host_a, BYTES, stream);   // 录制，不执行
cuLaunchKernel(kernel, ..., stream, ...);            // 录制，不执行
cuMemcpyDtoHAsync(host_out, dev_out, BYTES, stream); // 录制，不执行
cuStreamEndCapture(stream, &graph);

// 2. Instantiate — 编译为可执行对象（一次性）
cuGraphInstantiate(&graph_exec, graph, ...);

// 3. Launch — 重放整个图（可重复，异步）
cuGraphLaunch(graph_exec, stream);
cuEventRecord(event, stream);   // 打标记
cuEventQuery(event);            // 轮询完成 — 完全兼容现有 polling 模型
```

### 13.3 约束

| 约束 | 说明 |
|------|------|
| **结构固定** | 每次 replay 执行相同操作序列（同 kernel、同 grid/block dim、同 memcpy 大小） |
| **指针固定** | capture 时的 device/host 指针被烤进图中 |
| **数据内容可变** | replay 前写入新数据到**同一 host 地址**即可 |
| **不能 pool alloc** | `cuMemAlloc` 非 stream-ordered，不被录制 — 必须预分配固定缓冲区 |

适用场景：固定结构的重复计算（推理、信号处理、仿真时间步）
不适用：可变 shape、动态控制流、一次性计算

### 13.4 API 设计

核心洞察：**`gpu::replay` 完全复用 `gpu::run` 的机制，零新增模板特化。**

replay 本质上是把 `cuGraphLaunch` 塞到 `gpu::run` 的 lambda 里：

```cpp
// RAII graph handle
struct graph_handle {
    CUgraphExec exec = nullptr;
    CUgraph     graph = nullptr;

    ~graph_handle() {
        if (exec)  cuGraphExecDestroy(exec);
        if (graph) cuGraphDestroy(graph);
    }
    graph_handle(graph_handle&& o) noexcept
        : exec(o.exec), graph(o.graph) { o.exec = nullptr; o.graph = nullptr; }
    graph_handle(const graph_handle&) = delete;
};

// scheduler 新增方法
struct scheduler {
    // Capture — 在 scheduler 线程上同步执行
    // 通常在初始化阶段（advance loop 之前）调用
    graph_handle capture(auto&& func) {
        ensure_ctx_bound();
        check_cu(cuStreamBeginCapture(stream_, CU_STREAM_CAPTURE_MODE_GLOBAL));
        gpu_env env{stream_, device_, device_id_, &dev_pool_, &pin_pool_};
        func(env);
        CUgraph graph;
        check_cu(cuStreamEndCapture(stream_, &graph));
        CUgraphExec exec;
        check_cu(cuGraphInstantiate(&exec, graph, nullptr, nullptr, 0));
        return graph_handle{exec, graph};
    }
};

// Replay sender — 零新增模板特化
namespace gpu {
    template<typename scheduler_t>
    auto replay(scheduler_t* sched, graph_handle* handle) {
        return run(sched, [handle](gpu_env& env) {
            check_cu(cuGraphLaunch(handle->exec, env.stream));
        });
    }
}
```

**为什么不需要新的 op/op_pusher/compute_sender_type：**
`gpu::replay` 返回的就是 `gpu::run` 的 sender，work lambda 里执行 `cuGraphLaunch` 而非多个独立 cu* 调用。对框架而言完全透明。

### 13.5 典型用法

```cpp
// ── 初始化（advance loop 之前） ──

auto gpu_sched = cuda::scheduler(0);
auto kernel = cuda::scheduler::get_function(module, "inference");

// 预分配固定缓冲区（指针不变，内容每次更新）
cuda::check_cu(cuCtxSetCurrent(gpu_sched.cu_context()));
CUdeviceptr dev_input  = gpu_sched.dev_pool_.alloc(INPUT_BYTES);
CUdeviceptr dev_output = gpu_sched.dev_pool_.alloc(OUTPUT_BYTES);
auto* host_input  = (float*)gpu_sched.pin_pool_.alloc(INPUT_BYTES);
auto* host_output = (float*)gpu_sched.pin_pool_.alloc(OUTPUT_BYTES);

// Capture graph（操作被录制，不执行）
auto graph = gpu_sched.capture([&](cuda::gpu_env& env) {
    env.memcpy_h2d(dev_input, host_input, INPUT_BYTES);
    env.launch(kernel, {BATCH/256}, {256}, 0,
               (CUdeviceptr)dev_input, (CUdeviceptr)dev_output, BATCH);
    env.memcpy_d2h(host_output, dev_output, OUTPUT_BYTES);
});

// ── 请求处理（advance loop 中） ──

// 1. 写新数据到 host_input（同一地址，graph 内的 H2D 会读这个地址）
memcpy(host_input, request_data, INPUT_BYTES);

// 2. replay
just()
    >> cuda::gpu::replay(&gpu_sched, &graph)
    >> then([host_output]() {
        process_result(host_output);  // host_output 已有结果
    })
    >> submit(ctx);
```

### 13.6 动态 shape 的 re-capture

shape 变化时需要重新 capture。cache 放在应用层：

```cpp
std::unordered_map<size_t, graph_handle> graph_cache;

auto& get_or_create_graph(size_t batch_size) {
    if (auto it = graph_cache.find(batch_size); it != graph_cache.end())
        return it->second;
    auto [it, _] = graph_cache.emplace(batch_size,
        gpu_sched.capture([&](gpu_env& env) { /* ... */ }));
    return it->second;
}
```

### 13.7 与 Kernel Batching 的关系

两种优化互补，可叠加使用：

| | Kernel Batching (§9) | CUDA Graph (§13) |
|---|---|---|
| 优化目标 | 减少 launch 次数（N→1） | 减少每次 launch 的 CPU 开销（~10μs→~2μs） |
| 实现层面 | 应用层 | 框架层 |
| 适用场景 | 多个独立请求可合并 | 同一结构的计算反复执行 |
| 叠加效果 | batch scheduler 合并 N 个请求 → 用 `gpu::replay` 执行 batched graph | N 个请求合并为 1 次 launch，且该 launch 只需 ~2μs CPU 开销 |

### 13.8 框架新增代码量

| 组件 | 代码量 | 说明 |
|------|--------|------|
| `graph_handle` 结构 | ~15 行 | RAII，move-only |
| `scheduler::capture()` | ~10 行 | 录制 + 实例化 |
| `gpu::replay()` | ~5 行 | 语法糖，复用 `gpu::run` |
| 新增模板特化 | 0 | 完全复用现有基础设施 |
| 总计 | ~30 行 | |
