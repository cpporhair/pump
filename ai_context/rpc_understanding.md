# RPC 模块理解文档

## 1. 设计意图

RPC 模块是在 PUMP sender/operator 框架上构建的轻量远程过程调用层。核心设计哲学：

- **单向调用 session**：一个 TCP 连接（session）只承载一个方向的 RPC 调用（客户端 → 服务端）。如果需要服务端推送，另建 session。这极大简化了协议设计——不需要在一条连接上区分"谁发起的请求"。
- **sender 原生组合**：RPC 的 call/serv 本身就是 sender，可以直接嵌入 pipeline 中与 `then`、`flat_map`、`concurrent` 等算子组合，不引入额外的运行时抽象。
- **最小抽象**：不搞 channel/dispatch loop 等独立运行时组件。`call` 就是"发请求+等响应"，`serv` 就是"收请求+分派+发响应"，概念数量极少。

## 2. 整体架构

```
rpc.hh                          ← 公共 API 入口，暴露 rpc::call 和 rpc::serv
├── client/
│   ├── call.hh                 ← 客户端 RPC 调用实现
│   └── trigger.hh              ← 乱序响应处理（自定义 scheduler）
├── server/
│   └── serv.hh                 ← 服务端 RPC 处理循环
└── common/
    ├── struct.hh               ← 帧结构、辅助类型、运行时 context
    ├── rpc_state.hh            ← pending_requests_map（两阶段匹配状态机）
    └── service.hh              ← service trait + variant dispatch 工具
```

## 3. 协议帧格式

```
┌──────────────┬───────────────┬──────────────┬───────┬────────────────────┐
│ total_len(4) │ request_id(8) │ service_id(2)│ flags │    payload(变长)    │
│   uint32     │   uint64      │   uint16     │ uint8 │                    │
└──────────────┴───────────────┴──────────────┴───────┴────────────────────┘
                        共 15 字节 header (__attribute__((packed)))
```

- **total_len**：整个帧（header + payload）的字节数
- **request_id**：全局唯一请求 ID，用于匹配请求和响应
- **service_id**：标识调用的服务类型（enum class → uint16_t）
- **flags**：request(0x00) / response(0x01) / push(0x02)

### 帧内存管理

`rpc_frame_helper` 是帧的 RAII 包装：
- 底层是 `new char[]` 分配的裸内存，`reinterpret_cast` 为 `rpc_frame*`
- `realloc_frame(payload_size)` 负责分配/扩展帧
- 析构时 `delete[] reinterpret_cast<char*>(frame)` 释放
- Move-only，不可拷贝

## 4. request_id 生成策略

```cpp
value = (thread_index << 48) | counter++
```

- `thread_index`：全局 `atomic<uint16_t>` 递增分配，每线程一个（thread_local）
- `counter`：每线程独立递增
- 高 16 位是线程标识，低 48 位是线程内序号
- 保证跨线程全局唯一，无锁

## 5. 服务端流程 (serv.hh)

```
net::join(sche, session_id)         ← 绑定 session
>> push_context(session_state)      ← 压入 session 状态（scheduler 指针、sid、closed 标记）
>> serv_proc()                      ← 主处理逻辑
>> pop_context()                    ← 弹出 context

serv_proc 展开：
  get_context<session_state>
  >> then(生成 pipeline) >> flat()

  内部 pipeline：
    for_each(check_rpc_state 协程)   ← 协程：session 未关闭就持续 yield true
    >> with_context(serv_runtime_context)(  ← 每次迭代一个新的 req/res 帧对
        recv_req(sd)                 ← net::recv → 将收到的 net_frame 转为 rpc_frame 存入 context
        >> dispatch<service_ids...>()← 根据 service_id 分派到具体 service::handle()
        >> send_res(sd)              ← 将 res 帧通过 net::send 发回
    )
    >> handle_exception(sd)          ← 异常时标记 session closed + 传播异常
    >> reduce()                      ← 等全部流元素处理完
```

### dispatch 机制

1. `get_service_class_by_id<ids...>(runtime_id)` → `variant<monostate, service<id1>, service<id2>, ...>`
2. `visit()` 将 variant 转为编译期类型
3. `if constexpr (T::is_service)` 匹配具体 service
4. 调用 `T::handle(req, res)` → 返回 sender（可以是异步的）
5. handle 完成后填充 response header（flags=response, 复制 request_id 和 service_id）

### session 生命周期控制

`check_rpc_state` 是一个协程，通过 `session_state.closed` 控制：
- 正常时持续 `co_yield true` → `for_each` 不断驱动下一次 recv
- 异常时 `handle_exception` 调用 `sd.close()` → 协程下次检查返回 `false` → 流结束

## 6. 客户端流程 (call.hh)

```
call<service_id>(sche, sid, args...)
  → with_context(call_runtime_context)(  ← 创建调用上下文（request_id, sche, sid, req/res 帧）
      send_req<service_id>()             ← 序列化参数 + 发送请求帧
      >> wait_res<service_id>()          ← 等待匹配的响应帧 + 反序列化结果
  )
```

### send_req 详细步骤

1. `get_context<call_runtime_context>` 获取调用上下文
2. `service<service_id>::req_to_pkt(ctx.req, args...)` → 将参数序列化到 req 帧的 payload
3. 填写 header：service_id、request_id、flags=request
4. **所有权转移**：`ctx.req.frame = nullptr`，将 rpc_frame* 转交给 `net::send`
   - `net::send(sche, sid, f, len)` 内部用 `net_frame(char*, len)` 接管指针

### wait_res 详细步骤（这是最复杂的部分）

1. `get_context<call_runtime_context>` 获取上下文
2. `net::recv(sche, sid)` 接收一帧
3. 检查收到帧的 `request_id` 是否匹配当前调用：
   - **匹配**：`recv_res<true>` → 直接返回帧
   - **不匹配**：`recv_res<false>` → 这是别人的响应
4. `visit()` 将 `variant<recv_res<true>, recv_res<false>>` 转为编译期分支
5. 分支处理：
   - `recv_res<true>`：`forward_value(frame)` 直接传递
   - `recv_res<false>`：调用 `trigger.on_response(wrong_rid, frame)` 存入 map，然后 `trigger.wait_response(my_rid)` 等待自己的响应
6. 最终：`ctx.res.frame = reinterpret_cast<rpc_frame*>(frame.release())` + `service::pkt_to_res(ctx.res)` 反序列化

### 乱序响应问题

场景：客户端在同一 session 上有多个 pipelined 请求（例如通过 `concurrent`），响应可能乱序到达。

解决方案：`trigger`（trigger.hh）

- **thread_local 单例**：`static thread_local detail::trigger trigger(2048)`
- 内部持有 `pending_requests_map`
- 两个入口：
  - `on_response(rid, frame)`：收到不属于当前 call 的响应 → 存入 map
  - `wait_response(rid)`：返回一个自定义 sender，等待指定 rid 的响应

## 7. trigger 自定义 scheduler (trigger.hh)

`trigger` 是一个微型 scheduler，实现了 PUMP 自定义 scheduler 的完整模式：

### 组件

| 组件 | 说明 |
|------|------|
| `op` | 类型标记 `storage_at_op = true`，`start<pos>()` 注册回调到 map |
| `storage_at_sender` | sender，`connect()` 构建 op_tuple |
| `trigger` | 拥有 `pending_requests_map`，提供 `wait_response()` 和 `on_response()` |
| `op_pusher` 特化 | `requires storage_at_op`，调用 `op.start<pos>()` |
| `compute_sender_type` 特化 | 声明输出类型为 `net_frame` |

### pending_requests_map 两阶段匹配

slot 有 4 个状态：`empty → wait_frame / wait_callback → done`

**on_callback(rid, cb)**（wait_response 路径）：
- slot 为 empty：存 cb，状态→wait_frame（等帧到来）
- slot 为 wait_callback：已有帧 → 立即执行 cb(frame)，清空 slot

**on_frame(rid, frame)**（on_response 路径）：
- slot 为 empty：存 frame，状态→wait_callback（等 callback 到来）
- slot 为 wait_frame：已有 cb → 立即执行 cb(frame)，清空 slot

这是一个简洁的两阶段握手：无论帧和 callback 谁先到，都能正确配对。

### slot 容量与 hash

- 固定大小 vector（构造时指定 capacity）
- 用 `rid % slots.size()` 做 hash
- **隐含假设**：同时在飞的请求数不超过 capacity，否则 hash 冲突会导致状态错误

## 8. Service 定义模式

应用层通过特化 `service<service_id>` 来定义服务：

```cpp
template <>
struct service<my_enum::add> {
    constexpr static bool is_service = true;
    struct __attribute__((packed)) req_struct { int a, b; };
    struct __attribute__((packed)) res_struct { int v; };

    // 服务端：处理请求，填充响应，返回 sender
    static auto handle(rpc_frame_helper& req, rpc_frame_helper& res);

    // 客户端序列化：将参数写入请求帧
    static auto req_to_pkt(rpc_frame_helper& req, int a, int b);

    // 客户端反序列化：从响应帧解析结果
    static auto pkt_to_res(rpc_frame_helper& res) → res_struct;
};
```

特征：
- handle 返回 sender（可以是 `just()` 表示同步完成，也可以是异步 pipeline）
- 序列化直接 `reinterpret_cast`，zero-copy，依赖 `__attribute__((packed))` 保证布局
- service_id 必须是底层类型为 `uint16_t` 的 enum class（`uint16_enum_concept` 约束）

## 9. 示例程序 (apps/example/rpc/)

### 架构

- server 线程和 client 线程分别启动
- 共用同一个端口 8080
- server 接受连接后为每个 session 启动 `rpc::serv<sub, add>`
- client 建立 5 条连接组成连接池，round-robin 发送 15 次 `rpc::call<add>`

### 启动模式（两阶段 submit 模式）

server 和 client 都用相同的模式：
```
just()
>> get_context<rs_t*>()
>> then(构建业务 pipeline + submit 到独立 context)  ← 第一个 submit：启动业务逻辑
>> get_context<rs_t*>()
>> then(runtime::start)                              ← 启动 scheduler 主循环
>> submit(root_context(create_schedulers()))          ← 第二个 submit：启动整个流程
```

关键理解：第一个 `then` 里的 `submit` 只是"投递"业务 pipeline 到 scheduler 队列，然后立即返回。接下来的 `runtime::start` 才真正开始轮询 `advance()`，此时队列里已经有了业务 pipeline 的初始 op。

### 连接池

```
repeat(5) >> concurrent() >> connect >> join >> 存入 pool >> count()
```
- `concurrent()` 无参数 = 无限并发
- 但这里没有 `on(scheduler)`，所以实际上 connect 本身就是异步 sender，concurrent 只是允许同时在飞

## 10. 所有权与生命周期总结

| 对象 | 所有权 | 生命周期 |
|------|--------|---------|
| `rpc_frame`（通过 `rpc_frame_helper`） | RAII，`delete[] char*` | 帧处理期间。send 前通过 `frame = nullptr` 转移给 net 层 |
| `net_frame` | Move-only RAII，`delete[] _data` | 从 net::recv 产出到被消费（release 或析构） |
| `call_runtime_context` | 值语义，在 `with_context` 作用域内 | 单次 RPC 调用的完整生命周期 |
| `serv_runtime_context` | 值语义，在 `with_context` 作用域内 | 单次请求处理（recv→dispatch→send）|
| `session_state` | 值语义，在 `push_context` 作用域内 | session 存续期间 |
| `trigger` | thread_local 静态变量 | 线程生命周期 |
| `pending_requests_map` | trigger 成员 | 跟随 trigger |

### 帧所有权转移链

**客户端发送**：
```
rpc_frame_helper.frame (req)
  → ctx.req.frame = nullptr（手动释放所有权）
  → net::send(sche, sid, f, len)
  → net_frame(char*, len) 接管
  → send_req.frame
  → io_uring 写完后 net_frame 析构释放
```

**客户端接收**：
```
net::recv → net_frame
  → frame.release() 拿出 char*
  → reinterpret_cast<rpc_frame*>
  → ctx.res.frame 接管
  → rpc_frame_helper 析构释放
```

## 11. 未理解的问题

### Q1: wait_res 只 recv 一次？ — 已理解，不存在死锁

`wait_res` 内部只调用了一次 `net::recv`。设计逻辑：

- **单个 call**：session 上只有一个请求在飞，下一帧必然是自己的响应 → 走 `recv_res<true>` 直接返回，不进入 trigger 路径。
- **N 个并发 call**：N 个 recv 竞争收帧。收到非自己的帧 → 存入 trigger + 挂起；剩余 N-1 个继续 recv。最后一个 recv 的人一定收到自己的帧（前面的人已经收走了所有别人的帧并通过 trigger 分发）。

关键洞察：trigger 不需要自己 recv，它只是一个"暂存+匹配"机制。真正驱动收包的始终是各个并发 call 的 `net::recv`，它们互相为对方接力。

### Q2: trigger 是 thread_local 静态变量 — 已理解，极致效率设计

```cpp
static thread_local detail::trigger trigger(2048);
```

有意为之。利用 PUMP single-thread scheduler 的不变量：
- 同一线程上的所有 call 在同一个 advance() 循环中顺序推进，无并发竞争
- 因此 trigger 和 pending_requests_map 完全不需要原子操作或任何同步原语
- thread_local 保证每线程一个实例，零共享

当前是骨架代码，后续需要完善的点：
- session 关闭时清理对应 slot（fail_all 或按 session 清理），在单线程模型下实现简单且无需同步

### Q3: pending_requests_map 的 hash 冲突 — 已理解，容量可配置

`rid % slots.size()` 是刻意选择：定长数组 + 取模，零分配，cache 友好。rid 线程内递增，只要同时在飞的请求数 < capacity 就不会冲突。默认 2048 足够，应用层可根据并发量配置更大。

### Q4: send_req 中帧所有权转移的安全性 — 可接受，无需处理

当前手动 `frame = nullptr` + `net::send` 接管的模式足够，构造失败的场景实际不会发生。

### Q5: rpc_frame_helper.realloc_frame 只增不减 — 已理解，正确设计

"只增长的 buffer"策略，减少 heap allocation 次数。小帧复用大帧内存不会有问题：`total_len` 标记实际使用大小，读取不会越界；析构释放最后一次分配的完整内存，不会泄漏。

### Q6: 服务端 dispatch 对 monostate 的处理 — 已理解，正确设计

dispatch 是编译期静态分发，monostate 意味着收到了未注册的 service_id，属于协议层严重错误。抛异常由上层应用决定如何处理（关闭 session 或其他策略）。

### Q7: push 语义 — 废代码，需要删除

`rpc_flags::push = 0x02` 是遗留废代码，应在完善时删除。

### Q8: check_rpc_state 协程的引用生命周期 — 已理解，安全

context 对象在 `push_context` 和 `pop_context` 之间始终存活，无论中间 pipeline 同步还是异步。所以 `check_rpc_state(sd)` 和 `handle_exception(sd)` 持有的 `session_state` 引用在整个 session 生命周期内有效。

### Q9: client 中 wait_res 引用捕获 ctx 的安全性 — 已理解，同 Q8

`call_runtime_context` 通过 `with_context` 压入，生命周期由 push/pop 边界保证。内部 lambda 的 `[&ctx]` 引用在异步 recv 和 scope 切换过程中始终有效。

### Q10: fail_all 未实现 — 待完善

session 断开时，pending_requests_map 中可能有 wait_frame 或 wait_callback 状态的 slot，其 callback 永远不会被调用，导致 pipeline 挂起。需要实现 `fail_all`：遍历所有非 empty 的 slot，对有 callback 的 slot 传播异常。
