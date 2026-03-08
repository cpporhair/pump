# PUMP 运行时模型与核心类型

> 本文档补充 CLAUDE.md 未覆盖的运行时内部机制。设计原则、sender 表格、scheduler 模板见 CLAUDE.md。

## 1. 执行流程

```
submit(ctx) → connect<ctx_t>() → op_tuple(std::tuple<Op...>)
           → make_root_scope(op_tuple)
           → op_pusher<0, scope_t>::push_value(ctx, scope)
           → 逐 pos 推进 → push_done 结束
```

关键路径：
- `connect<ctx_t>()` 将 sender 链编译为平铺 `std::tuple<Op0, Op1, ...>`
- `op_pusher<pos>` 取出 `std::get<pos>(scope->get_op_tuple())` 执行
- 每个 op 通过 `op_pusher<pos+1>::push_value/push_exception/push_skip` 推进到下一步
- `push_done` 在流末尾触发，通知 scope 链完成

## 2. 核心类型

### 2.1 Context（运行期数据栈）

```cpp
// 根 context — shared_ptr 管理
template <typename ...content_t>
struct root_context {
    std::tuple<content_t...> datas;
};
// 创建: auto ctx = make_root_context(data1, data2);

// 压栈 context — 链接到 base
template <uint64_t id, typename base_t, typename ...content_t>
struct pushed_context {
    base_t base_context;
    std::tuple<content_t...> datas;
};

// 读取: auto& val = _get<NeedType, 0, context_t>(context);
```

`push_context` / `pop_context` 形成栈式结构，`get_context<T>()` 按类型在栈中查找。

### 2.2 Scope（op 列表容器）

```cpp
enum struct runtime_scope_type { root, stream_starter, other };

// 根 scope
template <typename op_tuple_t>
struct root_scope { op_tuple_t op_tuple; };

// 运行时 scope — 链接到 base
template <runtime_scope_type type, typename op_tuple_t, typename base_t>
struct runtime_scope {
    op_tuple_t op_tuple;
    base_t base_scope;
};
```

Scope 层级：
| 创建者 | scope_type | 说明 |
|--------|-----------|------|
| `submit` | `root` | 顶层 |
| `flat` / `flat_map` | `other` | 子 sender 展开后回接父 scope |
| `for_each` / `generate` | `stream_starter` | 流式处理，每元素创建子 scope |

### 2.3 Op 与 OpPusher

```cpp
// Op 示例（then 算子）
template <typename func_t>
struct op {
    func_t func;
    constexpr static bool then_op = true;  // 类型标记
};

// OpPusher — 四个推进入口
template<uint32_t pos, typename scope_t>
struct op_pusher {
    template<typename ctx_t, typename ...V>
    static void push_value(ctx_t& ctx, scope_t& scope, V&&... v);
    template<typename ctx_t>
    static void push_exception(ctx_t& ctx, scope_t& scope, std::exception_ptr e);
    template<typename ctx_t>
    static void push_skip(ctx_t& ctx, scope_t& scope);
    template<typename ctx_t>
    static void push_done(ctx_t& ctx, scope_t& scope);
};
```

每种 op 通过 `requires` 约束的模板特化被 `op_pusher` 匹配执行。

### 2.4 Sender

```cpp
template <typename prev_t, typename func_t>
struct sender {
    prev_t prev;
    func_t func;
    auto make_op() { return op<func_t>(std::move(func)); }
    template<typename ctx_t>
    auto connect() {
        return prev.template connect<ctx_t>().push_back(make_op());
    }
};
```

`connect()` 递归展开整条链，最终生成扁平 `op_tuple`。Sender 本身在 connect 后不再使用。

## 3. 生命周期

| 类型 | 管理方式 | 销毁时机 |
|------|---------|---------|
| Context | `shared_ptr` | 引用计数归零 |
| Scope | `shared_ptr` | 引用计数归零 |
| Op | 值语义，存于 tuple | 随 Scope 销毁 |
| Sender | 值语义 | connect 后即废弃 |

## 4. compute_sender_type

编译期推导 sender 的输出类型：

```cpp
template <typename ctx_t, typename sender_t>
struct compute_sender_type {
    consteval static uint32_t count_value();           // 输出值个数
    consteval static auto _impl_get_value_type();      // 值的 type_identity
    using value_type = ...;
};
```

自建 sender 必须特化此结构，否则编译报错。

## 5. Scheduler 运行方式

每核心运行主循环，轮询各 scheduler 的 `advance()`：

```cpp
while (running) {
    task_scheduler->advance();
    batch_scheduler->advance();
    nvme_scheduler->advance();
}
```

`advance()` 从请求队列 dequeue，执行业务逻辑，调用 `req->cb()` 触发 `op_pusher` 继续推进 pipeline。

运行环境：`src/env/runtime/share_nothing.hh` 提供多核 run/start 模式。

## 6. 模块目录

| 模块 | 路径 | 说明 |
|------|------|------|
| 核心 | `src/pump/core/*` | context, scope, op_pusher, compute_sender_type |
| Sender | `src/pump/sender/*` | 所有算子实现 |
| 协程 | `src/pump/coro/*` | task\<T\>, generator\<T\> |
| 非网络 Scheduler | `src/env/scheduler/task/`, `src/env/scheduler/nvme/` | task / nvme |
| 网络 Scheduler | `src/env/scheduler/net/*` | tcp / udp / kcp / dgram / rpc |
| 网络公共 | `src/env/scheduler/net/common/*` | session, frame, send/recv sender, frame_receiver |
| 运行时 | `src/env/runtime/*` | share_nothing 多核运行 |
| KV 应用 | `apps/kv/*` | 完整应用示例 |
| 示例 | `apps/example/*` | hello / echo / concurrent / rpc 等 |

推荐阅读顺序：CLAUDE.md → 本文档 → SENDERS_DETAIL.md → CODING_GUIDE.md → RPC_DETAIL.md
