### `when_any` Sender 算子实现计划（专家级审校版）

#### 1. 实现目标
- **语义**：任意一个分支完成（set_value/set_error/set_done）后，立即将该分支的结果推给下游，忽略后续到达的其他分支结果。
- **输出**：下游收到 `(uint32_t winner_index, variant_result_t winner_value)`，所有信号均包装为 variant 通过 `push_value` 传递（与 when_all 一致）。
- **无锁设计**：竞争逻辑仅使用原子操作（`std::atomic_flag` + `std::atomic<int>`），禁止互斥锁。
- **内存安全**：通过引用计数管理 `race_wrapper` 生命周期，最后递减者执行 `delete this`。

#### 2. 文件结构
- **实现文件**：`src/pump/sender/when_any.hh`
- **测试文件**：`apps/test/when_any_test.cc`
- **CMake**：在 `apps/CMakeLists.txt` 中添加 `test.when_any` 可执行目标

#### 3. 实施步骤

##### 3.1 创建 `src/pump/sender/when_any.hh`
- 从 `when_all.hh` 复制基本框架，修改 include guard 为 `PUMP_SENDER_WHEN_ANY_HH`。
- 所有组件置于 `namespace pump::sender::_when_any` 中。

##### 3.2 实现类型定义
- **`when_any_res`**：与 `when_all_res` 同构但独立定义。
  - 单值分支：`variant<monostate, exception_ptr, value_t>`
  - void 分支：`variant<monostate, exception_ptr, nullptr_t>`
- **`parent_pusher_status`**：在 `_when_any` 命名空间中独立定义（不复用 `_when_all` 版本），结构与 when_all 的 `parent_pusher_status` 一致。

##### 3.3 实现 `race_wrapper`（替代 when_all 的 `collector_wrapper` + `value_collector`）

- **数据布局**（cache-line 对齐 + false sharing 防护）：
  ```cpp
  template <uint32_t max, typename result_variant_t, typename parent_pusher_status_t>
  struct
  alignas(64)  // std::hardware_destructive_interference_size 的保守值
  __ncp__(race_wrapper) {
      // === 热路径原子变量（同一 cache line，所有分支都要触碰） ===
      std::atomic_flag finished {};       // 竞争标志（C++ 标准唯一保证 lock-free 的原子类型）
      std::atomic<int> ref_count;         // 引用计数，初始值 = 分支数

      // === 冷数据（仅 winner 写入一次，之后只读） ===
      uint32_t winner_index;              // 胜出分支索引
      result_variant_t result;            // 胜出分支结果

      // === 构造时设置，之后只读 ===
      parent_pusher_status_t parent_pusher_status;
  };
  ```

  **布局原理**：
  - `atomic_flag`（1 byte）+ `atomic<int>`（4 bytes）紧邻排列。每个分支完成时都要访问这两个字段（test_and_set + fetch_sub），将它们放在同一 cache line 的起始位置避免额外的 cache line fetch。
  - `alignas(64)` 确保整个 `race_wrapper` 起始于 cache line 边界，防止与堆上相邻对象产生 false sharing。x86 和 ARM 主流处理器 cache line 均为 64 bytes，使用硬编码值而非 `std::hardware_destructive_interference_size`（后者在 GCC/Clang 中长期未定义正确值）。
  - `winner_index` 和 `result` 只有 winner 写入一次，之后仅被同线程的 `push_value` 读取，无竞争。
  - `parent_pusher_status` 在构造时设置，后续只读。

- **竞争方法**（`set_value<index>` / `set_error<index>` / `set_done<index>` / `set_skip<index>`）：
  ```
  set_value<index>(value):
      1. finished.test_and_set(memory_order_acq_rel) 竞争
      2. 若胜出（返回 false = 之前未被设置）：
         - 写入 winner_index = index
         - result.template emplace<2>(__fwd__(value))  // move 语义，零拷贝
         - push_value 给 parent（传递 winner_index 和 __mov__(result)）
      3. 无论胜败，最后调用 release_ref()
  ```

  **关键约束**：`push_value` 必须在 `release_ref()` 之前完成——这是 `delete this` 安全性的核心不变量。PUMP 框架中 `push_value` 是同步调用链（通过 `op_pusher` 静态分派），不会异步延迟，因此该约束天然满足。

- **`release_ref()`**：
  ```cpp
  void release_ref() {
      if (ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) [[unlikely]] {
          delete this;
      }
  }
  ```

  **内存序精确分析**：
  - `fetch_sub` 使用 `memory_order_acq_rel`：
    - **release 语义**：当前线程对 race_wrapper 的所有写入（包括 winner 的 result 存储）在 decrement 之前对其他线程可见。
    - **acquire 语义**：最后一个递减者（执行 delete 的线程）能看到所有其他线程之前的写入。
  - 这是引用计数释放的标准模式（与 `shared_ptr` control block 的 `_M_release()` 一致，参见 libstdc++ `shared_ptr_base.h`）。
  - **`[[unlikely]]`**：对于 N 个分支，只有 1/N 的概率走 delete 路径。分支预测提示让处理器在热路径（非最后递减者）上无分支预测惩罚。

  **ARM/RISC-V 进阶优化（可选，注释说明即可）**：
  ```cpp
  // 对延迟极度敏感的弱序架构，可将非最后递减者放松为 release：
  // if (ref_count.fetch_sub(1, memory_order_release) == 1) {
  //     atomic_thread_fence(memory_order_acquire);
  //     delete this;
  // }
  // 节省：ARM 上每次非最后 fetch_sub 省一条 dmb ish 指令（~40 cycles on Cortex-A72）
  // 但 x86 上 acq_rel 和 release 对 RMW 操作完全相同（lock xadd 本身是 full barrier），
  // 因此本框架统一使用 acq_rel 是最佳性价比选择。
  ```

- **`delete this` 安全规则**：
  1. 对象必须通过 `new` 分配（不能是栈对象或数组元素）——在 `starter::start()` 中保证。
  2. `delete this` 之后不得访问任何成员变量——`release_ref()` 是函数体最后一行。
  3. `push_value` 必须在 `release_ref()` 之前完成——PUMP 的同步 push 模型保证。

##### 3.4 异常安全的启动协议（**关键改进**）

**问题**：`starter::start()` 中通过 `new` 创建 `race_wrapper`（ref_count = N），然后依次调用 `submit_senders` 启动 N 个分支。若第 K 个分支启动时抛异常（K < N），已启动的 K 个分支最终会调用 release_ref() K 次，但 ref_count 初始值为 N，导致 ref_count 永远降不到 0 → **内存泄漏**。

**注意**：when_all 的 `collector_wrapper` 存在同样的问题（甚至更严重——它根本没有 delete 机制）。when_any 必须正确处理。

**方案**：在 `submit_senders` 外层捕获异常，补偿未启动分支的 ref_count：
```cpp
template<typename context_t, typename scope_t, typename parent_pusher_status_t>
auto
start(context_t& context, scope_t& scope, parent_pusher_status_t&& s) {
    constexpr uint32_t branch_count = sizeof...(op_list_t);
    auto* wrapper = new race_wrapper_t(branch_count, __fwd__(s));
    submit_all_senders<0>(context, scope, wrapper, wrapper);
    // submit_all_senders 内部逐个启动，若某分支抛异常，
    // 在 catch 中为所有未启动分支调用 release_ref()，保证计数归零。
}
```

具体实现：`submit_senders` 改为递归模板，每次启动一个分支时用 try-catch 包裹。若分支 K 失败，为分支 K 到 N-1 各调用一次 `wrapper->release_ref()`（共 N-K 次），然后重新抛出异常。这保证了无论启动过程中哪里失败，ref_count 最终都能归零。

##### 3.5 实现 `reducer`
- 结构与 when_all 的 `reducer` 类似，持有 `race_wrapper*` 指针。
- 标志：`constexpr static bool when_any_reducer = true;`
- `set_value` / `set_error` / `set_done` / `set_skip` 路由到 `race_wrapper` 对应方法。

##### 3.6 实现 `starter`
- 结构与 when_all 的 `starter` 一致。
- 标志：`constexpr static bool when_any_starter = true;`
- `start()` 方法中通过 `new` 创建 `race_wrapper`（引用计数初始值为分支数），然后调用 `submit_senders` 启动所有分支（含 3.4 的异常安全逻辑）。

##### 3.7 实现 `sender` / `fn1` / `fn2`
- 与 when_all 结构一致，适配 `_when_any` 命名空间。
- `sender::connect()` 中 result 类型为单个 `result_variant_t`（所有分支共用），而非 when_all 的 `value_collector` tuple。
- 定义 `inline constexpr _when_any::fn1 when_any{};`

##### 3.8 单分支零开销优化（**必须实现**）

当 `sizeof...(sender_t) == 1` 时，`when_any` 退化为直通管道，此时：
- **零堆分配**：不 `new race_wrapper`，消除 malloc/free 开销（典型 ~50-100ns）。
- **零原子操作**：不需要 `atomic_flag` 和 `atomic<int>`，消除 cache line bounce。
- **直接转发**：唯一分支的结果直接包装为 `(0, variant_result)` 推给下游。

实现方式：在 `starter::start()` 中 `if constexpr (sizeof...(op_list_t) == 1)` 分支，使用轻量级的 `single_race_wrapper`（纯栈对象，无原子操作），或直接在 reducer 中内联转发。

**理由**：这不是可选优化。在高频交易场景中，`when_any(single_sender)` 可能出现在条件分支的退化路径上，每次多余的 heap allocation + atomic RMW 是不可接受的性能回归。编译期消除是零运行时成本的。

##### 3.9 添加 `op_pusher` 特化（`namespace pump::core`）
- **reducer 特化**：识别 `when_any_reducer` 标志，将 push_value/push_exception/push_skip/push_done 路由到 reducer 的 set_value/set_error/set_skip/set_done。
- **starter 特化**：识别 `when_any_starter` 标志，收到 push_value 时调用 `starter::start()`，传入 `_when_any::parent_pusher_status`。

##### 3.10 添加 `compute_sender_type` 特化（`namespace pump::core`）
- `count_value()` 返回 2（winner_index + result）。
- `value_type` 为 `std::tuple<uint32_t, common_result_variant_t>`。
- 先实现所有分支 value_type 相同的情况。

##### 3.11 创建测试文件 `apps/test/when_any_test.cc`
- 参考 `sequential_test.cc` 的测试框架风格（TEST/PASS/FAIL 宏、计数器、start_workers/stop_workers 模式）。
- **单线程测试**：
  - `test_when_any_single_sender`：单分支，验证 winner_index == 0
  - `test_when_any_basic`：多个 just() 分支
  - `test_when_any_with_values`：不同值的分支
  - `test_when_any_in_pipeline`：完整管道
  - `test_when_any_void_senders`：所有分支返回 void
- **异常路径测试**：
  - `test_when_any_exception`：单个分支抛异常
  - `test_when_any_all_exception`：所有分支抛异常
- **多核测试**：
  - `test_multicore_when_any_basic`：多 scheduler 分发
  - `test_multicore_when_any_race`：不同延时竞争
  - `test_multicore_when_any_stress`：大量重复执行压力测试
- **内存安全测试**：
  - `test_when_any_no_leak`：验证无内存泄漏

##### 3.12 更新 CMakeLists.txt
- 添加 `test.when_any` 可执行目标和 `pthread numa` 链接。

##### 3.13 编译与验证
- 编译通过，运行全部测试用例，特别关注多核压力测试下的内存安全和竞争正确性。

#### 4. 关键设计决策

##### 4.1 不实现取消机制
- Winner 完成后其余分支继续执行直到自然完成。
- 原因：PUMP 框架当前无通用 cancellation token 机制，when_all 也无取消，引入需框架级改动。
- **延迟影响**：败者分支浪费 CPU 周期 + race_wrapper 生命周期延长到最慢分支完成。对 HFT 场景，未来引入 `stop_token`（参考 P2300）是重要优化方向。

##### 4.2 单分支编译期优化（必须实现）
- `sizeof...(sender_t) == 1` 时退化为无原子操作、无堆分配的轻量路径。
- 见 3.8 详细说明。

##### 4.3 代码风格
- 严格遵循 PUMP 项目现有风格：`__ncp__`、`__fwd__`、`__mov__`、`__all_must_rval__`、`__typ__` 宏。
- 4 空格缩进，结构体声明风格与 when_all.hh 保持一致。

#### 5. 专家级审校要点（设计评审检查清单）

##### 5.1 内存序正确性证明
- **竞争标志 `finished`**：`test_and_set(acq_rel)` 建立 happens-before 关系。Winner 的后续 result 写入对同线程的 `push_value` 可见（program order）。Losers 无需看到 result（它们直接走 release_ref）。
- **引用计数 `ref_count`**：`fetch_sub(acq_rel)` 确保最后递减者（执行 delete）能看到所有之前递减者的写入。这是 `shared_ptr` control block 的标准模式，已被 C++ 标准库广泛验证。
- **禁止 `seq_cst`**：显式标注 `acq_rel` 而非依赖默认 `seq_cst`，在 ARM/RISC-V 上避免不必要的 full barrier（`dmb ish`），在 x86 上零额外开销（`lock xadd` 本身是 full barrier）。

##### 5.2 数据竞争自由证明
- `winner_index` 和 `result`：仅由 winner（唯一一个 `test_and_set` 返回 false 的线程）写入，之后由同线程的 `push_value` 读取。Losers 从不访问这两个字段。无竞争。
- `parent_pusher_status`：构造时写入，之后只读。无竞争。
- `finished` 和 `ref_count`：原子变量，通过原子操作访问。无竞争。

##### 5.3 生命周期正确性
- `push_value` 在 PUMP 中是**同步调用链**（`op_pusher::push_value` 直接调用下一个 op 的方法，不经过队列或异步调度）。因此 winner 的 `push_value` 完成后再调用 `release_ref()` 时，race_wrapper 中的 result 数据已经被下游消费完毕。
- Winner 的 `release_ref()` 不一定是最后一次递减（可能还有并行的 loser 分支未完成）。这是安全的——loser 的 `release_ref()` 不访问 result 字段。
- 最后递减者执行 `delete this` 时，所有分支都已完成，无悬挂指针。

##### 5.4 异常安全保证
- **分支内异常**：分支执行中抛出的异常通过 `set_error` 传递给 race_wrapper，参与竞争。winner 的异常通过 variant 传递给下游。无资源泄漏。
- **启动阶段异常**：见 3.4，通过补偿未启动分支的 ref_count 保证 race_wrapper 最终被释放。

##### 5.5 when_all collector_wrapper 内存泄漏
- 审查确认 when_all 的 `collector_wrapper` 通过 `new` 分配但从未 `delete`。when_any 的 `race_wrapper` 通过引用计数正确管理生命周期。建议后续为 when_all 加入相同机制（此次不改动 when_all）。

##### 5.6 编译器优化友好性
- **`[[unlikely]]` on delete path**：`release_ref()` 中 `if (...) [[unlikely]] { delete this; }` 让编译器将 delete 路径放到 cold section，热路径（非最后递减者）保持 straight-line 执行。
- **`constexpr` 分支消除**：`if constexpr (sizeof...(op_list_t) == 1)` 在编译期完全消除多分支路径的代码生成，零运行时开销。
- **`inline` 标注**：竞争方法（set_value 等）和 release_ref 标注 `inline`，配合模板实例化，编译器在 `-O2` 下通常能内联整个竞争路径（test_and_set + fetch_sub），减少函数调用开销。
