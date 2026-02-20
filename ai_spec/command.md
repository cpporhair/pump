# 任务：实现 `when_any` Sender 算子

## 0. 角色

你是一个非常资深的 C++ 开发者，对代码的质量和运行效率有非常极致的要求,对并发编程有深入理解，熟悉 PUMP 框架的设计和实现。你的任务是根据 `when_all` 算子的实现，设计并实现 `when_any` 算子，以满足特定的并发需求。  

## 1. 概述

参考已有的 `when_all` 算子（`src/pump/sender/when_all.hh`），实现 `when_any` 算子。

**语义差异**：
- `when_all`：等待所有分支完成后，将全部结果聚合推给下游
- `when_any`：任意一个分支完成（set_value/set_error/set_done）后，立即将该分支的结果推给下游，忽略后续到达的其他分支结果

## 2. 文件位置

- **实现文件**：`src/pump/sender/when_any.hh`
- **测试文件**：`apps/test/when_any_test.cc`
- **CMake**：在 `apps/CMakeLists.txt`（或对应的测试 CMakeLists）中添加 `when_any_test` 可执行目标

## 3. 接口设计

### 3.1 用户层 API

```cpp
// 管道风格（与 when_all 一致）
just() >> when_any(sender1, sender2, sender3) >> then([](auto&& result) { ... })

// when_any 接受 1 个或多个 sender 参数
inline constexpr _when_any::fn1 when_any{};
```

### 3.2 输出类型

`when_any` 的输出为一个 `std::variant`，包含所有分支的可能结果类型：

```cpp
// 结果类型定义（与 when_all_res 同构，但独立定义以保持 namespace 隔离）
template <typename ...value_t>
struct when_any_res;

// 单值分支：variant<monostate, exception_ptr, value_t>
template <typename value_t>
struct when_any_res<value_t> {
    using type = std::variant<std::monostate, std::exception_ptr, value_t>;
};

// void 分支：variant<monostate, exception_ptr, nullptr_t>
template <>
struct when_any_res<void> {
    using type = std::variant<std::monostate, std::exception_ptr, nullptr_t>;
};
```

**输出值**：只输出首个完成分支对应的那一个 variant 值，同时输出一个 `uint32_t` 表示胜出分支的索引。

即下游收到两个参数：`(uint32_t winner_index, variant_result_t winner_value)`。

**设计说明**：与 when_all 保持一致，所有信号（value/error/done）均包装为 variant 并通过 `push_value` 传递给下游。这意味着下游通过检查 variant 的 index 来判断胜出分支的完成状态（0=done/skip, 1=error, 2=value），而非通过 push_exception/push_done 通道。这保证了与 when_all 的语义一致性，下游可以用统一的模式处理两种算子的结果。

## 4. 内部实现要求

### 4.1 整体结构（对照 when_all）

参照 `when_all.hh` 中的组件划分，`when_any` 需要实现以下对应组件（均在 `namespace pump::sender::_when_any` 中）：

| when_all 组件 | when_any 对应组件 | 差异说明 |
|---|---|---|
| `value_collector` | 不需要 | when_any 无需收集所有值 |
| `collector_wrapper` | `race_wrapper` | 用 `std::atomic_flag` 实现竞争；增加 `std::atomic<int>` 引用计数管理生命周期 |
| `reducer` | `reducer` | 结构类似，但调用 race_wrapper 的竞争接口 |
| `starter` | `starter` | 结构一致 |
| `sender` | `sender` | 结构一致 |
| `fn1` / `fn2` | `fn1` / `fn2` | 结构一致 |

### 4.2 核心竞争逻辑：`race_wrapper`

#### 4.2.1 数据布局（cache-friendly）

```cpp
template <uint32_t max, typename result_variant_t, typename parent_pusher_status_t>
struct
__ncp__(race_wrapper) {
    // === 热路径原子变量（同一 cache line） ===
    std::atomic_flag finished {};           // 竞争标志（guaranteed lock-free）
    std::atomic<int> ref_count;             // 引用计数，初始值 = max

    // === 冷数据（winner 写入后只读） ===
    uint32_t winner_index;                  // 胜出分支索引
    result_variant_t result;                // 胜出分支结果

    // === 构造时设置，之后只读 ===
    parent_pusher_status_t parent_pusher_status;

    // ... 构造函数、竞争方法 ...
};
```

**布局说明**：
- `atomic_flag` 和 `atomic<int>` 紧邻排列，位于同一 cache line。每个分支完成时都要访问这两个字段，将它们放在一起避免额外的 cache line fetch。
- `winner_index` 和 `result` 只有 winner 写入一次，之后只被 `push_value` 读取（同一线程），无竞争。
- `parent_pusher_status` 在构造时设置，后续只读。

#### 4.2.2 竞争协议与内存序

```
set_value<index>(value):
    // 1. 竞争：test_and_set 使用 acq_rel
    //    - acquire：确保看到对象的完整构造
    //    - release：如果胜出，后续 result 写入对其他线程可见（用于 ref_count 释放路径）
    if (!finished.test_and_set(std::memory_order_acq_rel)):
        winner_index = index
        result.template emplace<2>(fwd(value))
        push_value 给 parent（传递 winner_index 和 result）
    // 2. 无论胜败，递减引用计数
    release_ref()

set_error<index>(exception_ptr):
    if (!finished.test_and_set(std::memory_order_acq_rel)):
        winner_index = index
        result.template emplace<1>(exception_ptr)
        push_value 给 parent（传递 winner_index 和 result）
    release_ref()

set_done<index>():
    if (!finished.test_and_set(std::memory_order_acq_rel)):
        winner_index = index
        result.template emplace<0>(std::monostate{})
        push_value 给 parent（传递 winner_index 和 result）
    release_ref()

set_skip<index>():
    与 set_done 相同逻辑
    release_ref()
```

**关键点**：每个分支无论胜败，都必须调用 `release_ref()`。这保证了引用计数正确递减。

#### 4.2.3 引用计数释放协议

```cpp
void release_ref() {
    // 最后一个递减者需要 acquire，以确保看到所有分支
    // 对 race_wrapper 的写入（包括 winner 的 result 存储）
    // 非最后递减者用 release 即可
    if (ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete this;
    }
}
```

**内存序说明**：
- `fetch_sub` 使用 `memory_order_acq_rel`：
  - **release** 语义确保当前线程对 race_wrapper 的所有写入在 decrement 之前可见
  - **acquire** 语义确保最后一个递减者（执行 delete 的线程）能看到所有其他线程的写入
- 这是引用计数的标准模式（与 `shared_ptr` 的 control block 释放一致）
- 注意：`delete this` 后不得再访问任何成员变量。`release_ref()` 应该是函数的最后一条调用

**优化说明**：如果对延迟极度敏感，可以将非最后递减者的内存序放松为 `memory_order_release`，最后递减者额外加 `atomic_thread_fence(memory_order_acquire)`。但 `acq_rel` 在 x86 上几乎零额外开销（x86 的 `lock xadd` 本身就是 full barrier），仅在 ARM/RISC-V 等弱序架构上有差异。对于本框架，统一使用 `acq_rel` 是最佳性价比选择。

#### 4.2.4 `delete this` 安全性

使用 `delete this` 是 C++ 中管理引用计数对象的标准模式（COM、`shared_ptr` control block 等均使用此模式）。安全规则：

1. 对象必须通过 `new` 分配（不能是栈对象或数组元素）
2. `delete this` 之后不得访问任何成员变量
3. `release_ref()` 必须是调用链中的**最后一步**

在 `set_value` / `set_error` / `set_done` / `set_skip` 中，push_value 必须在 `release_ref()` **之前**完成。

### 4.3 内存管理

- `race_wrapper` 通过 `new` 在堆上分配（与 when_all 的 `collector_wrapper` 一致）
- **引用计数生命周期管理**：初始值为分支数量 `max`，每个分支完成（无论胜败）都 `release_ref()`，最后一个递减者执行 `delete this`

**重要修复**：注意 when_all 的 `collector_wrapper` 存在内存泄漏——通过 `new` 分配但从未 `delete`。when_any 的 `race_wrapper` 通过引用计数正确管理生命周期。建议后续也为 when_all 的 `collector_wrapper` 加入相同的引用计数释放机制。

### 4.4 op_pusher 特化

与 when_all 相同模式，在 `namespace pump::core` 中提供：

1. **reducer 的 op_pusher 特化**：识别 `when_any_reducer` 标志，将 push_value/push_exception/push_skip/push_done 路由到 reducer
2. **starter 的 op_pusher 特化**：识别 `when_any_starter` 标志，在收到 push_value 时调用 `starter::start()`

标志命名：
```cpp
constexpr static bool when_any_reducer = true;  // 在 reducer 上
constexpr static bool when_any_starter = true;  // 在 starter 上
```

### 4.5 compute_sender_type 特化

```cpp
template<typename context_t, typename prev_t, typename ...sender_t>
struct compute_sender_type<context_t, sender::_when_any::sender<prev_t, sender_t...>> {
    // count_value() 返回 2（winner_index + result）
    // value_type 为 std::tuple<uint32_t, common_result_variant_t>
};
```

其中 `common_result_variant_t` 需要将所有分支的 result variant 合并为一个统一的 variant 类型。

**简化方案**：如果所有分支的 value_type 相同，输出 `std::tuple<uint32_t, when_any_res<T>::type>`；如果不同，输出所有可能类型的 variant 的 variant。

推荐先实现**所有分支 value_type 相同**的情况，后续再扩展异构分支。

### 4.6 单分支编译期优化

当 `when_any` 只有一个分支时（`sizeof...(sender_t) == 1`），整个竞争机制可以退化：

```cpp
// 在 starter::start() 中：
if constexpr (sizeof...(op_list_t) == 1) {
    // 无需 new race_wrapper，无需原子操作
    // 直接将唯一分支的结果作为 winner_index=0 推给下游
    // 可通过一个轻量级的 single_reducer 实现
}
```

这对于在循环或高频调用中使用 `when_any(single_sender)` 的场景可以完全消除原子操作和堆分配的开销。

## 5. 关于取消（Cancellation）的说明

当前设计中，winner 完成后其余分支仍会继续执行直到自然完成。这意味着：
- 败者分支的计算资源被浪费
- race_wrapper 的生命周期被延长到最慢分支完成

**当前版本不实现取消机制**，原因：
1. PUMP 框架当前没有通用的 cancellation token / stop_token 机制
2. when_all 同样没有取消机制
3. 引入取消需要在 sender/receiver 协议中增加 stop channel，属于框架级改动

**未来优化方向**：
- 引入 `stop_token` 传递给各分支 sender，winner 完成时触发 cancellation
- 可参考 P2300（std::execution）的 `stop_token` 设计
- 对于高频交易场景，这是重要的优化——避免 CPU 在注定被丢弃的分支上浪费周期

## 6. 代码风格要求

必须严格遵循 PUMP 项目现有代码风格（参考 `when_all.hh`）：

- 使用 `__ncp__()` 宏标记 noncopyable 类
- 使用 `__fwd__()` / `__mov__()` 进行完美转发和移动
- 使用 `__all_must_rval__()` 断言右值
- 使用 `__typ__()` 获取类型
- include guard 格式：`PUMP_SENDER_WHEN_ANY_HH`
- 命名空间：`pump::sender` 和内部 `pump::sender::_when_any`
- 缩进：4 空格
- 结构体声明风格与 when_all.hh 保持一致（`struct __ncp__(name) {` 换行风格）

## 7. 测试要求

测试文件 `apps/test/when_any_test.cc`，参考 `apps/test/sequential_test.cc` 的测试框架风格（TEST/PASS/FAIL 宏、test_count/pass_count/fail_count 计数）。

### 7.1 单线程测试

| 测试用例 | 说明 |
|---|---|
| `test_when_any_single_sender` | 只有一个分支，验证该分支结果正确传递，winner_index == 0 |
| `test_when_any_basic` | 多个 just() 分支，验证某个分支胜出并正确传递值 |
| `test_when_any_with_values` | 不同值的分支（如 `just(10)`, `just(20)`, `just(30)`），验证结果是其中之一 |
| `test_when_any_in_pipeline` | `just() >> when_any(...) >> then(...) >> submit(context)` 完整管道 |
| `test_when_any_void_senders` | 所有分支返回 void 的情况 |

### 7.2 异常路径测试

| 测试用例 | 说明 |
|---|---|
| `test_when_any_exception` | 某个分支抛异常，验证异常结果能正确作为 variant 中的 exception_ptr 传递 |
| `test_when_any_all_exception` | 所有分支都抛异常，验证首个异常被传递 |

### 7.3 多核测试（参考 sequential_test.cc 的 start_workers/stop_workers 模式）

| 测试用例 | 说明 |
|---|---|
| `test_multicore_when_any_basic` | 多个分支分发到不同 scheduler 执行，验证首个完成的分支正确传递 |
| `test_multicore_when_any_race` | 多个分支包含不同延时（通过计算量模拟），验证竞争正确性 |
| `test_multicore_when_any_stress` | 大量重复执行 when_any，验证无内存泄漏、无崩溃 |

### 7.4 内存安全测试

| 测试用例 | 说明 |
|---|---|
| `test_when_any_no_leak` | 重复执行大量 when_any 管道，通过进程 RSS 或自定义计数器验证无内存泄漏 |

### 7.5 测试主函数结构

```cpp
int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "when_any Module Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    // Single-thread tests
    std::cout << "\n--- Single-thread Tests ---" << std::endl;
    test_when_any_single_sender();
    test_when_any_basic();
    test_when_any_with_values();
    test_when_any_in_pipeline();
    test_when_any_void_senders();

    // Exception tests
    std::cout << "\n--- Exception Tests ---" << std::endl;
    test_when_any_exception();
    test_when_any_all_exception();

    // Multi-core tests
    uint32_t num_cores = std::min(8u, std::thread::hardware_concurrency());
    std::cout << "\n--- Multi-core Tests (using " << num_cores << " cores) ---" << std::endl;
    start_workers(num_cores);
    test_multicore_when_any_basic();
    test_multicore_when_any_race();
    test_multicore_when_any_stress();
    test_when_any_no_leak();
    stop_workers();

    // Summary
    std::cout << "\n========================================" << std::endl;
    std::cout << "Results: " << pass_count << "/" << test_count << " passed";
    if (fail_count > 0) std::cout << ", " << fail_count << " FAILED";
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;

    return fail_count > 0 ? 1 : 0;
}
```

## 8. 实现步骤建议

1. **创建 `src/pump/sender/when_any.hh`**，从 `when_all.hh` 复制并修改
2. **实现 `race_wrapper`**：使用 `std::atomic_flag` + `std::atomic<int>` ref_count，注意内存序标注
3. **实现 `release_ref()`**：`fetch_sub(1, memory_order_acq_rel)`，最后递减者 `delete this`
4. **修改 `reducer`**，使用 `when_any_reducer` 标志
5. **修改 `starter`**，使用 `when_any_starter` 标志
6. **修改 `sender`/`fn1`/`fn2`**，适配新命名空间
7. **添加 `op_pusher` 特化和 `compute_sender_type` 特化**
8. **创建测试文件**，逐步实现并通过各测试用例
9. **更新 CMakeLists.txt**，添加测试目标
10. **编译并运行测试**，确保全部通过（特别关注多核压力测试下的内存安全）

## 9. 注意事项

### 9.1 无锁设计
- PUMP 框架是无锁的，when_any 的竞争逻辑只允许使用原子操作，不得引入互斥锁
- 使用 `std::atomic_flag`（而非 `std::atomic<bool>`）作为竞争标志——它是 C++ 标准中**唯一保证 lock-free** 的原子类型

### 9.2 内存序
- 竞争标志 `finished`：使用 `memory_order_acq_rel`
- 引用计数 `ref_count`：使用 `memory_order_acq_rel`
- **禁止使用默认的 `memory_order_seq_cst`**——在 ARM/RISC-V 上 seq_cst 有额外开销（dmb ish full barrier），acq_rel 已足够保证正确性
- x86 上 acq_rel 和 seq_cst 对 RMW 操作性能相同，但显式标注体现了对内存模型的理解

### 9.3 单线程 scheduler
- 每个 scheduler 是单线程的，但 when_any 的多个分支可能被调度到不同的 scheduler
- 因此 race_wrapper 的原子操作是必要的

### 9.4 内存安全
- race_wrapper 的 `release_ref()` 必须是每个分支执行路径的**最后一步**
- winner 的 `push_value` 必须在 `release_ref()` 之前完成
- `delete this` 后不得访问任何成员——将 `release_ref()` 放在函数体最后一行

### 9.5 宏和类型工具
- 使用项目已有的宏（`__ncp__`、`__fwd__`、`__mov__`、`__typ__`），不要引入新的工具宏

### 9.6 `parent_pusher_status`
- 在 `_when_any` 中定义独立版本（保持 namespace 隔离），不复用 `_when_all::parent_pusher_status`
