# RPC 层详细设计文档

基于 `command.md` 需求文档，结合 Net 层、Task Scheduler 及 PUMP 框架现有实现的详细设计。

---

## 一、总体架构

### 1.1 模块分层

```
src/env/scheduler/rpc/
├── rpc.hh                      # 对外统一头文件（rpc::call / rpc::notify / rpc::serve）
├── common/
│   ├── header.hh               # RPC 固定头定义与解析
│   ├── message_type.hh         # 消息类型枚举
│   ├── error.hh                # RPC 错误类型
│   ├── codec_concept.hh        # Codec concept 约束
│   └── config.hh               # Channel 配置（超时、pending 上限等）
├── channel/
│   ├── channel.hh              # RPC Channel 核心结构
│   ├── pending_map.hh          # 请求-响应关联表
│   └── dispatcher.hh           # 消息分发器
├── senders/
│   ├── call.hh                 # rpc::call sender（请求-响应）
│   ├── notify.hh               # rpc::notify sender（单向通知）
│   ├── serve.hh                # rpc::serve sender（接收循环）
│   └── reply.hh                # 内部：自动回复 Response
└── codec/
    └── raw_codec.hh            # 默认 Codec 实现（零拷贝 raw buffer）
```

### 1.2 依赖关系

```
rpc::call / rpc::notify / rpc::serve
         │
         ▼
   RPC Channel (channel.hh)
   ├── pending_map.hh     ─── 请求-响应关联
   ├── dispatcher.hh      ─── method_id → handler 分发
   ├── header.hh          ─── RPC 固定头解析/构造
   └── Codec<T>           ─── payload 编解码（模板参数）
         │
         ▼
   Net 层 (net::recv / net::send / net::stop)
   ├── session_scheduler   ─── 会话 IO
   ├── packet_buffer       ─── SPSC ring buffer
   └── session_id_t        ─── 会话标识
```

### 1.3 线程模型

```
Core 0                          Core 1
┌─────────────────────┐        ┌─────────────────────┐
│ task_scheduler       │        │ task_scheduler       │
│ session_scheduler    │        │ session_scheduler    │
│                     │        │                     │
│ Channel A (sid=1)   │        │ Channel C (sid=3)   │
│   ├── pending_map   │        │   ├── pending_map   │
│   ├── dispatcher    │        │   ├── dispatcher    │
│   └── recv_loop     │        │   └── recv_loop     │
│                     │        │                     │
│ Channel B (sid=2)   │        │                     │
│   ├── pending_map   │        │                     │
│   ├── dispatcher    │        │                     │
│   └── recv_loop     │        │                     │
└─────────────────────┘        └─────────────────────┘
```

- 每个 Channel 绑定到 session 所在 core 的 scheduler 线程
- Channel 所有状态（pending_map、dispatcher、recv_loop）在该线程单线程操作，**无锁**
- 跨核 RPC 调用通过 `session_scheduler` 的 lock-free send queue 投递

---

## 二、RPC 消息格式

### 2.1 Wire Format

```
Net 层帧:
┌──────────────────┬───────────────────────────────────────────┐
│ uint16_t length  │                payload                    │
│    (2 bytes)     │             (≤ 65535 bytes)               │
└──────────────────┴───────────────────────────────────────────┘
                   │← ─ ─ ─ ─ Net 层 payload ─ ─ ─ ─ ─ ─ ─ →│

RPC 层视角（在 Net payload 内部）:
┌─────────┬──────────────┬─────────────┬───────────────────────┐
│  type   │  request_id  │  method_id  │    rpc payload        │
│ (1 byte)│  (4 bytes)   │  (2 bytes)  │  (≤ 65528 bytes)     │
└─────────┴──────────────┴─────────────┴───────────────────────┘
│← ─ ─ ─ ─ ─ ─ RPC header (7 bytes) ─ ─ ─ ─ →│
```

### 2.2 消息类型枚举

```cpp
// common/message_type.hh
namespace pump::rpc {
    enum class message_type : uint8_t {
        request      = 0x01,
        response     = 0x02,
        notification = 0x03,
        error        = 0x04,
    };
}
```

### 2.3 RPC Header 结构

```cpp
// common/header.hh
namespace pump::rpc {

    struct rpc_header {
        message_type type;        // 1 byte
        uint32_t     request_id;  // 4 bytes, 网络字节序（小端，内部通信无需转换）
        uint16_t     method_id;   // 2 bytes

        static constexpr size_t size = 7;
    };

    // 从 buffer 解析 header（零拷贝，直接读取指针位置）
    // data 指向 Net payload 起始位置
    inline rpc_header parse_header(const uint8_t* data) {
        rpc_header h;
        h.type       = static_cast<message_type>(data[0]);
        std::memcpy(&h.request_id, data + 1, 4);
        std::memcpy(&h.method_id,  data + 5, 2);
        return h;
    }

    // 将 header 写入 buffer（用于 encode 时构造 iovec[0]）
    inline void write_header(uint8_t* buf, const rpc_header& h) {
        buf[0] = static_cast<uint8_t>(h.type);
        std::memcpy(buf + 1, &h.request_id, 4);
        std::memcpy(buf + 5, &h.method_id,  2);
    }
}
```

**设计决策**：
- 内部 RPC 通信，字节序使用本机序（小端），避免额外转换开销
- Header 共 7 字节，足够小，可内联到 iovec[0] 避免额外堆分配
- `parse_header` 使用 `memcpy` 而非强制转换，避免对齐问题

### 2.4 Error Response 扩展字段

Error 类型消息的 payload 部分携带错误信息：

```
Error payload:
┌──────────────┬──────────────────────┐
│  error_code  │  error_message       │
│  (2 bytes)   │  (剩余字节, 可选)     │
└──────────────┴──────────────────────┘
```

```cpp
// common/error.hh
namespace pump::rpc {

    enum class error_code : uint16_t {
        success          = 0,
        method_not_found = 1,
        remote_error     = 2,
        codec_error      = 3,
        timeout          = 4,
        internal_error   = 5,
    };

    // RPC 层异常类型
    struct rpc_timeout_error : std::runtime_error {
        uint32_t request_id;
        rpc_timeout_error(uint32_t rid)
            : std::runtime_error("rpc timeout"), request_id(rid) {}
    };

    struct method_not_found_error : std::runtime_error {
        uint16_t method_id;
        method_not_found_error(uint16_t mid)
            : std::runtime_error("method not found"), method_id(mid) {}
    };

    struct remote_error : std::runtime_error {
        error_code code;
        remote_error(error_code c, const char* msg)
            : std::runtime_error(msg), code(c) {}
    };

    struct connection_lost_error : std::runtime_error {
        connection_lost_error()
            : std::runtime_error("connection lost") {}
    };

    struct channel_closed_error : std::runtime_error {
        channel_closed_error()
            : std::runtime_error("channel closed") {}
    };

    struct pending_overflow_error : std::runtime_error {
        pending_overflow_error()
            : std::runtime_error("pending requests overflow") {}
    };
}
```

---

## 三、Codec 抽象

### 3.1 Codec Concept

```cpp
// common/codec_concept.hh
namespace pump::rpc {

    // payload 视图：指向 Net 层 ring buffer 内的数据
    // 可能跨越 ring buffer 边界，因此用 iovec 表示
    struct payload_view {
        const iovec* vec;   // 1 或 2 段
        uint8_t      cnt;   // iovec 数量（1 或 2）
        size_t       len;   // 总长度
    };

    // Codec 必须满足的 concept
    template<typename C>
    concept rpc_codec = requires(C codec) {
        // 解码：从 payload_view 反序列化为 T
        // T 由调用方指定（模板方法）
        // { codec.template decode_payload<T>(payload_view) } -> std::same_as<T>;

        // 编码：将对象序列化为 iovec 数组
        // 返回 encode_result，包含 iovec 数组和生命周期管理
        // { codec.encode_payload(obj) } -> encode_result 类型;
    };

    // 编码结果：持有序列化数据的生命周期
    struct encode_result {
        iovec  vec[4];          // 最多 4 段 iovec（header + payload 分段）
        size_t cnt;             // 实际 iovec 数量
        size_t total_len;       // 总字节数

        // payload 数据存储（栈上小缓冲 + 可选堆分配）
        uint8_t  inline_buf[64]; // 小 payload 内联存储
        uint8_t* heap_buf;       // 大 payload 堆分配（nullptr 如果不需要）

        ~encode_result() { delete[] heap_buf; }
        encode_result(encode_result&&) noexcept = default;
        encode_result& operator=(encode_result&&) noexcept = default;
    };
}
```

### 3.2 Ring Buffer 线性化辅助

Codec 解码时 payload 可能跨越 ring buffer 边界。提供辅助函数：

```cpp
namespace pump::rpc {

    // 如果 payload 连续，直接返回指针；如果跨边界，拷贝到 tmp_buf 后返回
    // tmp_buf 由调用方在栈上提供，避免堆分配
    inline const uint8_t* linearize_payload(
        const payload_view& pv,
        uint8_t* tmp_buf,       // 调用方提供的临时缓冲区
        size_t tmp_buf_size     // 临时缓冲区大小
    ) {
        if (pv.cnt == 1) {
            // 连续数据，零拷贝
            return static_cast<const uint8_t*>(pv.vec[0].iov_base);
        }
        // 跨边界，需要拷贝到线性缓冲区
        assert(pv.len <= tmp_buf_size);
        size_t offset = 0;
        for (uint8_t i = 0; i < pv.cnt; ++i) {
            std::memcpy(tmp_buf + offset, pv.vec[i].iov_base, pv.vec[i].iov_len);
            offset += pv.vec[i].iov_len;
        }
        return tmp_buf;
    }
}
```

### 3.3 默认 Raw Codec

```cpp
// codec/raw_codec.hh
namespace pump::rpc {

    // 最简 Codec：payload 就是原始字节，不做序列化
    // 适用于 trivially-copyable 类型的直接传输
    struct raw_codec {
        template<typename T>
        T decode_payload(const payload_view& pv) {
            static_assert(std::is_trivially_copyable_v<T>);
            T result;
            uint8_t tmp[sizeof(T)];
            const uint8_t* data = linearize_payload(pv, tmp, sizeof(T));
            std::memcpy(&result, data, sizeof(T));
            return result;
        }

        template<typename T>
        encode_result encode_payload(const T& obj) {
            static_assert(std::is_trivially_copyable_v<T>);
            encode_result r{};
            r.heap_buf = nullptr;
            static_assert(sizeof(T) <= sizeof(r.inline_buf));
            std::memcpy(r.inline_buf, &obj, sizeof(T));
            r.vec[0] = {r.inline_buf, sizeof(T)};
            r.cnt = 1;
            r.total_len = sizeof(T);
            return r;
        }
    };
}
```

---

## 四、Pending Request Map

### 4.1 设计目标

- O(1) 查找、插入、删除
- 预分配固定大小数组，热路径无堆分配
- 单线程操作（session 所在 core），无需锁
- 支持超时清理

### 4.2 数据结构

```cpp
// channel/pending_map.hh
namespace pump::rpc {

    struct pending_slot {
        enum class state : uint8_t {
            empty = 0,
            active,
        };

        state                                                       status;
        uint32_t                                                    request_id;
        uint64_t                                                    deadline_ms;   // 0 表示无超时
        std::move_only_function<void(std::variant<payload_view,
                                                   std::exception_ptr>)> cb;
    };

    template<uint32_t MaxPending = 1024>
    struct pending_map {
        std::array<pending_slot, MaxPending> slots;
        uint32_t active_count = 0;

        // 插入 pending 请求，返回 slot index
        // request_id % MaxPending 作为初始 slot，线性探测
        std::optional<uint32_t> insert(uint32_t request_id,
                                        uint64_t deadline_ms,
                                        auto&& cb) {
            if (active_count >= MaxPending) return std::nullopt;
            uint32_t idx = request_id % MaxPending;
            // 线性探测找到空 slot
            for (uint32_t i = 0; i < MaxPending; ++i) {
                uint32_t probe = (idx + i) % MaxPending;
                if (slots[probe].status == pending_slot::state::empty) {
                    slots[probe].status     = pending_slot::state::active;
                    slots[probe].request_id = request_id;
                    slots[probe].deadline_ms = deadline_ms;
                    slots[probe].cb         = std::forward<decltype(cb)>(cb);
                    ++active_count;
                    return probe;
                }
            }
            return std::nullopt; // 不应到达
        }

        // 根据 request_id 查找并移除，返回 callback
        auto remove(uint32_t request_id)
            -> std::optional<std::move_only_function<void(std::variant<payload_view,
                                                                       std::exception_ptr>)>> {
            uint32_t idx = request_id % MaxPending;
            for (uint32_t i = 0; i < MaxPending; ++i) {
                uint32_t probe = (idx + i) % MaxPending;
                auto& slot = slots[probe];
                if (slot.status == pending_slot::state::empty) return std::nullopt;
                if (slot.status == pending_slot::state::active &&
                    slot.request_id == request_id) {
                    auto cb = std::move(slot.cb);
                    slot.status = pending_slot::state::empty;
                    --active_count;
                    return cb;
                }
            }
            return std::nullopt;
        }

        // 超时扫描：检查所有 active slot，超时的执行 exception callback
        void check_timeouts(uint64_t now_ms) {
            for (uint32_t i = 0; i < MaxPending; ++i) {
                auto& slot = slots[i];
                if (slot.status == pending_slot::state::active &&
                    slot.deadline_ms > 0 && now_ms >= slot.deadline_ms) {
                    auto cb = std::move(slot.cb);
                    slot.status = pending_slot::state::empty;
                    --active_count;
                    cb(std::make_exception_ptr(
                        rpc_timeout_error(slot.request_id)));
                }
            }
        }

        // Channel 关闭时，所有 pending 请求收到异常
        void fail_all(std::exception_ptr e) {
            for (uint32_t i = 0; i < MaxPending; ++i) {
                auto& slot = slots[i];
                if (slot.status == pending_slot::state::active) {
                    auto cb = std::move(slot.cb);
                    slot.status = pending_slot::state::empty;
                    cb(e);
                }
            }
            active_count = 0;
        }
    };
}
```

### 4.3 设计说明

| 决策 | 理由 |
|------|------|
| 固定大小数组 | 预分配，无堆分配；cache 友好（线性存储） |
| open-addressing + 线性探测 | 简单高效，无指针追逐 |
| `request_id % MaxPending` | 由于 request_id 单调递增，分布均匀 |
| variant callback | Response 和 exception 统一通过同一 callback 传递 |
| 超时扫描全表 | MaxPending 固定且不大（通常 1024），线性扫描可接受 |

---

## 五、消息分发器（Dispatcher）

### 5.1 设计原则：Handler 统一返回 Sender

**所有 handler 统一返回 sender**，无论业务逻辑是同步还是异步：

| 场景 | handler 写法 | 说明 |
|------|-------------|------|
| 同步处理 | `return just(FooResponse{...});` | 用 `just()` 包装同步结果 |
| 异步处理 | `return just() >> on(sched) >> then(process);` | 直接返回 sender pipeline |
| 查表处理 | `return just(req) >> flat_map(lookup) >> then(build_resp);` | 任意组合 |

**优势**：
- 统一接口：`on()` 只有一个注册方法，handler 签名一致
- PUMP 原生：handler 返回值就是 sender，与框架无缝组合
- 无歧义：框架不需要判断 handler 返回值是 sender 还是普通值
- 灵活性：handler 内部可以切换 scheduler、并发、异常处理

### 5.2 Handler 接口

```cpp
// channel/dispatcher.hh
namespace pump::rpc {

    // 传递给 handler 的请求上下文
    struct handler_context {
        uint32_t     request_id;  // 原始请求 ID（Request 有效，Notification 为 0）
        uint16_t     method_id;   // 方法 ID
        payload_view payload;     // payload 视图（指向 ring buffer，零拷贝）
    };

    // handler 签名：接收 handler_context，返回 sender
    //
    // 对于 Request handler：
    //   sender 产生的值作为 Response payload 发送回调用方
    //   sender 产生的异常转为 Error Response 发送回调用方
    //
    // 对于 Notification handler：
    //   sender 产生的值被忽略（无 Response）
    //   sender 产生的异常由框架记录/忽略
    //
    // 用法示例：
    //   // 同步：直接返回结果
    //   dispatcher.on(METHOD_ADD, [](handler_context&& ctx) {
    //       auto req = codec.decode_payload<AddReq>(ctx.payload);
    //       return just(AddResp{.result = req.a + req.b});
    //   });
    //
    //   // 异步：返回 sender pipeline
    //   dispatcher.on(METHOD_GET, [sched](handler_context&& ctx) {
    //       auto key = codec.decode_payload<GetReq>(ctx.payload).key;
    //       return just(key)
    //           >> on(sched->as_task())
    //           >> flat_map([](auto&& k) { return db_lookup(k); })
    //           >> then([](auto&& val) { return GetResp{.value = val}; });
    //   });

    // 运行期分发表：method_id → handler
    template<typename channel_t>
    struct dispatcher {
        // handler_fn：类型擦除的 handler 包装
        // 接收 channel 引用 + handler_context
        // 内部执行：handler(ctx) >> encode_response >> send
        using handler_fn = std::move_only_function<void(channel_t&, handler_context&&)>;

        static constexpr uint16_t MAX_METHODS = 256;

        std::array<handler_fn, MAX_METHODS> request_handlers;
        std::array<handler_fn, MAX_METHODS> notification_handlers;
        handler_fn default_handler;

        dispatcher() {
            default_handler = [](channel_t& ch, handler_context&& ctx) {
                ch.send_error(ctx.request_id, error_code::method_not_found,
                              "method not found");
            };
        }

        // ─── 注册 Request handler ───
        // Func 签名：auto handler(handler_context&&) -> sender<ResponsePayload>
        // 框架自动：handler(ctx) >> encode >> send_response
        template<typename Func>
        void on(uint16_t method_id, Func&& handler) {
            request_handlers[method_id] = wrap_request_handler(
                std::forward<Func>(handler));
        }

        // ─── 注册 Notification handler ───
        // Func 签名：auto handler(handler_context&&) -> sender<T>（T 被忽略）
        // 框架自动：handler(ctx) >> ignore_results >> submit
        template<typename Func>
        void on_notification(uint16_t method_id, Func&& handler) {
            notification_handlers[method_id] = wrap_notification_handler(
                std::forward<Func>(handler));
        }

        // ─── 分发 Request ───
        void dispatch_request(channel_t& ch, handler_context&& ctx) {
            auto& h = request_handlers[ctx.method_id];
            if (h) {
                h(ch, std::move(ctx));
            } else {
                default_handler(ch, std::move(ctx));
            }
        }

        // ─── 分发 Notification ───
        void dispatch_notification(channel_t& ch, handler_context&& ctx) {
            auto& h = notification_handlers[ctx.method_id];
            if (h) {
                h(ch, std::move(ctx));
            }
            // 未注册的 notification 静默丢弃
        }

    private:
        // 包装 Request handler：
        // handler 返回的 sender 产生值 → encode + send_response
        // handler 返回的 sender 产生异常 → send_error
        template<typename Func>
        handler_fn wrap_request_handler(Func&& f) {
            return [f = std::forward<Func>(f)](
                channel_t& ch, handler_context&& ctx
            ) mutable {
                auto request_id = ctx.request_id;
                auto method_id  = ctx.method_id;
                auto ch_ptr     = ch.shared_from_this();

                // handler 返回 sender，进入 pipeline 执行
                f(std::move(ctx))
                    >> then([ch_ptr, request_id, method_id](auto&& response) {
                        auto encoded = ch_ptr->codec().encode_payload(response);
                        ch_ptr->send_response(request_id, method_id,
                                              std::move(encoded));
                    })
                    >> any_exception([ch_ptr, request_id](std::exception_ptr e) {
                        ch_ptr->send_error(request_id, error_code::remote_error,
                                           "handler exception");
                        return just();
                    })
                    >> submit(ch.context());
            };
        }

        // 包装 Notification handler：
        // handler 返回的 sender 执行即可，不发送 Response
        template<typename Func>
        handler_fn wrap_notification_handler(Func&& f) {
            return [f = std::forward<Func>(f)](
                channel_t& ch, handler_context&& ctx
            ) mutable {
                f(std::move(ctx))
                    >> ignore_all_exception()
                    >> submit(ch.context());
            };
        }
    };
}
```

### 5.3 使用示例

```cpp
auto ch = rpc::make_channel<MyCodec>(sid, session_sched, task_sched);

// ─── 同步 handler：用 just() 包装返回值 ───
ch->dispatcher().on(METHOD_PING, [](rpc::handler_context&& ctx) {
    return just(PongResponse{.timestamp = now()});
});

// ─── 同步 handler：需要解码请求 ───
ch->dispatcher().on(METHOD_ADD, [codec](rpc::handler_context&& ctx) {
    auto req = codec.decode_payload<AddRequest>(ctx.payload);
    return just(AddResponse{.result = req.a + req.b});
});

// ─── 异步 handler：涉及 IO 操作 ───
ch->dispatcher().on(METHOD_GET, [sched, &db](rpc::handler_context&& ctx) {
    auto key = codec.decode_payload<GetRequest>(ctx.payload).key;
    return just(std::move(key))
        >> on(sched->as_task())
        >> then([&db](auto&& k) { return db.get(k); })
        >> then([](auto&& val) { return GetResponse{.value = val}; });
});

// ─── 异步 handler：需要跨 scheduler ───
ch->dispatcher().on(METHOD_STORE, [task_sched, nvme_sched](rpc::handler_context&& ctx) {
    auto req = codec.decode_payload<StoreRequest>(ctx.payload);
    return just(std::move(req))
        >> on(task_sched->as_task())
        >> then([](auto&& r) { return prepare_write(r); })
        >> on(nvme_sched->put(page))    // 切换到 NVMe scheduler
        >> then([]() { return StoreResponse{.ok = true}; });
});

// ─── Notification handler ───
ch->dispatcher().on_notification(METHOD_HEARTBEAT, [](rpc::handler_context&& ctx) {
    // 更新心跳时间戳
    return just();  // 无返回值，sender<void>
});
```

### 5.4 编译期分发（可选优化）

对于方法数量固定且编译期已知的协议，可通过模板特化实现零开销分发：

```cpp
// 编译期分发器（协议层可选使用）
template<typename ProtocolDef>
struct static_dispatcher {
    // ProtocolDef 需提供：
    //   static constexpr auto method_table = std::tuple{
    //       method_entry<METHOD_A, HandlerA>{},
    //       method_entry<METHOD_B, HandlerB>{},
    //   };

    template<typename channel_t>
    static void dispatch(channel_t& ch, handler_req&& req) {
        dispatch_impl(ch, std::move(req),
                      std::make_index_sequence<
                          std::tuple_size_v<decltype(ProtocolDef::method_table)>>{});
    }

private:
    template<typename channel_t, size_t... Is>
    static void dispatch_impl(channel_t& ch, handler_req&& req,
                               std::index_sequence<Is...>) {
        // 展开为 if-else chain（编译器通常优化为 jump table）
        bool matched = ((std::get<Is>(ProtocolDef::method_table).method_id == req.method_id
            && (std::get<Is>(ProtocolDef::method_table).handle(ch, std::move(req)), true))
            || ...);
        if (!matched) {
            ch.send_error(req.request_id, error_code::method_not_found, "method not found");
        }
    }
};
```

---

## 六、RPC Channel

### 6.1 核心结构

```cpp
// channel/channel.hh
namespace pump::rpc {

    enum class channel_status : uint8_t {
        active,     // 正常工作
        draining,   // 停止接受新请求，等待 pending 完成
        closed,     // 已关闭
    };

    struct channel_config {
        uint32_t max_pending       = 1024;    // 最大并发请求数
        uint64_t default_timeout_ms = 5000;   // 默认请求超时（ms），0 表示无超时
        bool     enable_heartbeat  = false;   // 是否启用心跳
        uint64_t heartbeat_interval_ms = 1000; // 心跳间隔
        uint32_t heartbeat_max_miss = 3;       // 最大心跳丢失次数
    };

    template<
        typename Codec,                       // 编解码器类型
        typename SessionScheduler,            // net session_scheduler 类型
        typename TaskScheduler,               // task_scheduler 类型（用于超时）
        typename ProtocolState = void         // 上层协议 per-session 状态（可选）
    >
    struct channel {
        // ─── 身份与引用 ───
        net::session_id_t       session_id;
        SessionScheduler*       session_sched;
        TaskScheduler*          task_sched;

        // ─── RPC 状态 ───
        uint32_t                next_request_id = 1;  // 单调递增
        channel_status          status = channel_status::active;
        pending_map<>           pending;              // 请求-响应关联表
        dispatcher<channel>     dispatch;             // 消息分发器
        Codec                   _codec;               // 编解码器实例

        // ─── 协议层状态 ───
        [[no_unique_address]]
        ProtocolState           protocol_state;       // void 时零开销

        // ─── 配置 ───
        channel_config          config;

        // ─── 生成请求 ID ───
        uint32_t alloc_request_id() {
            return next_request_id++;
        }

        // ─── 访问器 ───
        Codec& codec() { return _codec; }
        auto& dispatcher() { return dispatch; }

        // ─── 发送 Response ───
        void send_response(uint32_t request_id, uint16_t method_id,
                           encode_result&& payload);

        // ─── 发送 Error ───
        void send_error(uint32_t request_id, error_code code,
                        const char* message);

        // ─── 发送 Notification ───
        void send_notification(uint16_t method_id, encode_result&& payload);
    };
}
```

### 6.2 Channel 创建

```cpp
namespace pump::rpc {

    // 工厂函数
    template<typename Codec,
             typename SessionScheduler,
             typename TaskScheduler,
             typename ProtocolState = void>
    auto make_channel(
        net::session_id_t sid,
        SessionScheduler* session_sched,
        TaskScheduler* task_sched,
        channel_config config = {}
    ) {
        using channel_t = channel<Codec, SessionScheduler, TaskScheduler, ProtocolState>;
        // Channel 使用 shared_ptr 管理生命周期
        // 因为 recv_loop、pending callbacks、用户 pipeline 都可能持有引用
        auto ch = std::make_shared<channel_t>();
        ch->session_id     = sid;
        ch->session_sched  = session_sched;
        ch->task_sched     = task_sched;
        ch->config         = config;
        return ch;
    }
}
```

### 6.3 Channel 生命周期状态机

```
             create
               │
               ▼
          ┌──────────┐
          │  active   │◄─── serve() 启动接收循环
          └────┬──────┘     call() / notify() 正常收发
               │
         close() / 连接断开
               │
               ▼
          ┌──────────┐
          │ draining  │ ── 停止接受新请求
          └────┬──────┘    等待 pending 完成或超时
               │
         pending 清空 / 超时
               │
               ▼
          ┌──────────┐
          │  closed   │ ── fail_all pending
          └──────────┘    net::stop 关闭底层 session
```

### 6.4 发送实现

Channel 的发送方法构造 RPC header + payload，通过 `net::send` 发送：

```cpp
// 内部发送辅助：构造 [rpc_header | payload] 的 iovec 并调用 net::send
template<typename Codec, typename SessionScheduler, typename TaskScheduler, typename PS>
void channel<Codec, SessionScheduler, TaskScheduler, PS>::send_response(
    uint32_t request_id, uint16_t method_id, encode_result&& payload)
{
    // 构造 header
    uint8_t header_buf[rpc_header::size];
    write_header(header_buf, {message_type::response, request_id, method_id});

    // 组装 iovec: [header] + [payload iovecs...]
    iovec send_vec[5]; // header(1) + payload(最多4)
    send_vec[0] = {header_buf, rpc_header::size};
    for (size_t i = 0; i < payload.cnt; ++i) {
        send_vec[i + 1] = payload.vec[i];
    }

    // 通过 net::send 发送（send 会自动添加 uint16_t 长度前缀）
    // 注意：payload 的生命周期必须在 send 完成前有效
    // 因此 encode_result 需要通过 move 捕获到 send 的 callback 中
    auto* req = new net::common::send_req{
        session_id,
        send_vec,
        payload.cnt + 1,
        [payload = std::move(payload)](bool ok) mutable {
            // send 完成后 payload 自动析构
            // 如果需要错误处理可在此添加
        }
    };
    net::common::prepare_send_vec(req);
    session_sched->schedule(req);
}
```

---

## 七、接收循环（Recv Loop）

### 7.1 设计概述

接收循环是一个持续运行的 pipeline，使用 `forever()` + `net::recv` 不断接收数据，对每次 recv 返回的 `packet_buffer` 解析所有完整消息并分发。

### 7.2 Pipeline 结构

```cpp
// senders/serve.hh
namespace pump::rpc {

    template<typename channel_ptr_t>
    auto serve(channel_ptr_t ch) {
        auto* sched = ch->session_sched;
        auto  sid   = ch->session_id;

        return forever()                           // 无限循环
            >> net::recv(sched, sid)               // 等待 recv 事件
            >> then([ch](net::packet_buffer* buf) {
                // 一次 recv 可能包含多条完整消息，全部处理
                process_all_messages(ch, buf);
            })
            >> any_exception([ch](std::exception_ptr e) {
                // recv 异常（连接断开等）
                ch->on_connection_lost(e);
                return just(); // 终止循环——通过设置 channel 状态
            });
    }
}
```

### 7.3 消息解析与分发

```cpp
namespace pump::rpc {

    template<typename channel_ptr_t>
    void process_all_messages(channel_ptr_t& ch, net::packet_buffer* buf) {
        // 循环处理 buffer 中所有完整消息
        while (net::detail::has_full_pkt(*buf)) {
            // 获取当前消息的 pkt_iovec（已去掉长度前缀的 payload）
            auto pkt = net::detail::get_recv_pkt(*buf);

            // pkt 指向 Net payload = [rpc_header(7B) | rpc_payload]
            // 需要从 iovec 中提取 header
            dispatch_message(ch, pkt);

            // 前进 buffer head，释放已处理的数据
            buf->forward_head(pkt_total_len);
        }
    }

    template<typename channel_ptr_t>
    void dispatch_message(channel_ptr_t& ch, const net::pkt_iovec& pkt) {
        // 1. 解析 RPC header（前 7 字节）
        // 需要处理 header 可能跨越 iovec 边界的情况
        uint8_t header_buf[rpc_header::size];
        const uint8_t* header_data = extract_header_bytes(pkt, header_buf);
        auto header = parse_header(header_data);

        // 2. 构造 payload_view（跳过 header 后的数据）
        payload_view pv = make_payload_view(pkt, rpc_header::size);

        // 3. 按消息类型分发
        switch (header.type) {
            case message_type::response:
            case message_type::error:
                handle_response(ch, header, pv);
                break;

            case message_type::request:
                handle_request(ch, header, pv);
                break;

            case message_type::notification:
                handle_notification(ch, header, pv);
                break;
        }
    }

    // 处理 Response/Error：查找 pending_map，执行 callback
    template<typename channel_ptr_t>
    void handle_response(channel_ptr_t& ch, const rpc_header& header,
                         const payload_view& pv) {
        auto cb = ch->pending.remove(header.request_id);
        if (!cb) return; // 已超时移除或重复响应，丢弃

        if (header.type == message_type::error) {
            // 解析 error payload，构造异常
            auto err = parse_error_payload(pv);
            (*cb)(std::make_exception_ptr(remote_error(err.code, err.message)));
        } else {
            (*cb)(pv);
        }
    }

    // 处理 Request：分发到 handler，handler 返回 sender → encode → send_response
    template<typename channel_ptr_t>
    void handle_request(channel_ptr_t& ch, const rpc_header& header,
                        const payload_view& pv) {
        handler_context ctx{
            .request_id = header.request_id,
            .method_id  = header.method_id,
            .payload    = pv,
        };
        ch->dispatch.dispatch_request(*ch, std::move(ctx));
    }

    // 处理 Notification：分发到 handler，handler 返回 sender → 执行即可
    template<typename channel_ptr_t>
    void handle_notification(channel_ptr_t& ch, const rpc_header& header,
                             const payload_view& pv) {
        handler_context ctx{
            .request_id = 0, // Notification 无 request_id
            .method_id  = header.method_id,
            .payload    = pv,
        };
        ch->dispatch.dispatch_notification(*ch, std::move(ctx));
    }
}
```

### 7.4 超时检测集成

超时检测有两种实现方式：

**方案 A：per-request 定时器（精确但有开销）**

每个 `rpc::call` 启动一个 `task_scheduler::delay` 定时器。超时后检查 pending_map 中是否还存在该 request_id。

**方案 B：周期性扫描（批量但有延迟）** ← **推荐**

在接收循环中定期调用 `pending.check_timeouts(now_ms)`：

```cpp
// 在 serve 的 pipeline 中集成超时检测
auto serve(channel_ptr_t ch) {
    return forever()
        >> net::recv(sched, sid)
        >> then([ch](net::packet_buffer* buf) {
            process_all_messages(ch, buf);
            // 每次 recv 返回后顺便检查超时
            ch->pending.check_timeouts(scheduler::task::scheduler::now_ms());
        })
        >> any_exception(...);
}
```

**推荐方案 B 的理由**：
- 无额外定时器开销
- recv 返回频率通常足够高，超时精度可接受
- 对于空闲连接（长时间无 recv 返回），可额外启动一个低频定时器补充扫描

---

## 八、Sender API 设计

### 8.1 rpc::call — 请求-响应 Sender

`rpc::call` 是 RPC 层的核心 sender，遵循 PUMP 的自定义 scheduler sender 模式（req/cb/op/sender/op_pusher）。

```cpp
// senders/call.hh
namespace pump::rpc::_call {

    // ─── req ───
    // 提交给 session_scheduler 的 send 请求完成后，
    // 真正的等待发生在 pending_map 中：recv_loop 收到 response 后执行 cb
    template<typename channel_ptr_t>
    struct req {
        channel_ptr_t                ch;
        uint16_t                     method_id;
        encode_result                encoded_payload;  // 已编码的请求 payload
        uint64_t                     timeout_ms;
        // cb 是收到 Response 后的回调，由 op.start() 设置
        // 存储在 pending_map 中，而非 req 本身
    };

    // ─── op ───
    template<typename channel_ptr_t>
    struct op {
        constexpr static bool rpc_call_op = true;  // 类型标记

        channel_ptr_t ch;
        uint16_t      method_id;
        encode_result encoded_payload;
        uint64_t      timeout_ms;

        template<uint32_t pos, typename ctx_t, typename scope_t>
        void start(ctx_t& ctx, scope_t& scope) {
            if (ch->status != channel_status::active) {
                pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                    ctx, scope,
                    std::make_exception_ptr(channel_closed_error()));
                return;
            }

            // 1. 分配 request_id
            uint32_t rid = ch->alloc_request_id();

            // 2. 计算 deadline
            uint64_t deadline = timeout_ms > 0
                ? scheduler::task::scheduler::now_ms() + timeout_ms
                : 0;

            // 3. 注册到 pending_map
            // callback 闭包捕获 ctx/scope，收到 response 后继续 pipeline
            auto inserted = ch->pending.insert(rid, deadline,
                [ctx = ctx, scope = scope](
                    std::variant<payload_view, std::exception_ptr> result
                ) mutable {
                    std::visit([&](auto&& r) {
                        using T = std::decay_t<decltype(r)>;
                        if constexpr (std::is_same_v<T, payload_view>) {
                            // 解码 response payload 并继续 pipeline
                            pump::core::op_pusher<pos + 1, scope_t>::push_value(
                                ctx, scope, std::move(r));
                        } else {
                            // 异常传播
                            pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                                ctx, scope, r);
                        }
                    }, std::move(result));
                });

            if (!inserted) {
                pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                    ctx, scope,
                    std::make_exception_ptr(pending_overflow_error()));
                return;
            }

            // 4. 构造 send iovec: [rpc_header | payload]
            uint8_t header_buf[rpc_header::size];
            write_header(header_buf, {message_type::request, rid, method_id});

            iovec send_vec[5];
            send_vec[0] = {header_buf, rpc_header::size};
            for (size_t i = 0; i < encoded_payload.cnt; ++i) {
                send_vec[i + 1] = encoded_payload.vec[i];
            }

            // 5. 通过 net::send 发送
            auto* send_req = new net::common::send_req{
                ch->session_id,
                send_vec,
                encoded_payload.cnt + 1,
                [payload = std::move(encoded_payload)](bool ok) mutable {
                    // send 完成，payload 生命周期结束
                    // send 失败不在此处理（由 recv_loop 的连接断开检测处理）
                }
            };
            net::common::prepare_send_vec(send_req);
            ch->session_sched->schedule(send_req);
        }
    };

    // ─── sender ───
    template<typename channel_ptr_t>
    struct sender {
        channel_ptr_t ch;
        uint16_t      method_id;
        encode_result encoded_payload;
        uint64_t      timeout_ms;

        auto make_op() {
            return op<channel_ptr_t>{
                ch, method_id, std::move(encoded_payload), timeout_ms};
        }

        template<typename ctx_t>
        auto connect() {
            return pump::core::builder::op_list_builder<0>().push_back(make_op());
        }
    };
}

// ─── 对外 API ───
namespace pump::rpc {

    // 泛型版本：接收已编码的 payload
    template<typename channel_ptr_t>
    auto call(channel_ptr_t ch, uint16_t method_id,
              encode_result&& payload, uint64_t timeout_ms = 0) {
        return _call::sender<channel_ptr_t>{
            ch, method_id, std::move(payload), timeout_ms};
    }

    // 便捷版本：自动用 Codec 编码请求对象
    template<typename Request, typename channel_ptr_t>
    auto call(channel_ptr_t ch, uint16_t method_id,
              const Request& req, uint64_t timeout_ms = 0) {
        auto encoded = ch->codec().encode_payload(req);
        return _call::sender<channel_ptr_t>{
            ch, method_id, std::move(encoded), timeout_ms};
    }
}
```

### 8.2 op_pusher 特化

```cpp
// senders/call.hh (底部)
namespace pump::core {

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::rpc_call_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename context_t>
        static inline void push_value(context_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };
}
```

### 8.3 compute_sender_type 特化

```cpp
namespace pump::core {

    template<typename context_t, typename channel_ptr_t>
    struct compute_sender_type<context_t, rpc::_call::sender<channel_ptr_t>> {
        // call 返回 payload_view（由调用方决定如何解码）
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<rpc::payload_view>{};
        }
        using value_type = rpc::payload_view;
    };
}
```

### 8.4 rpc::notify — 通知 Sender

```cpp
// senders/notify.hh
namespace pump::rpc {

    // notify 是 fire-and-forget，不进入 pending_map
    // 直接构造 [rpc_header | payload] 并 net::send
    template<typename channel_ptr_t>
    auto notify(channel_ptr_t ch, uint16_t method_id, encode_result&& payload) {
        // 构造 header
        uint8_t header_buf[rpc_header::size];
        write_header(header_buf, {message_type::notification, 0, method_id});

        // 组装 iovec 并发送
        // notify 不需要等待响应，直接用 then + net::send 实现即可
        // 返回 sender<void>
        return just()
            >> then([ch, method_id, payload = std::move(payload)]() mutable {
                // 构造并发送
                ch->send_notification(method_id, std::move(payload));
            });
    }

    // 便捷版本
    template<typename Message, typename channel_ptr_t>
    auto notify(channel_ptr_t ch, uint16_t method_id, const Message& msg) {
        auto encoded = ch->codec().encode_payload(msg);
        return notify(ch, method_id, std::move(encoded));
    }
}
```

### 8.5 rpc::serve — 接收循环 Sender

```cpp
// senders/serve.hh
namespace pump::rpc {

    template<typename channel_ptr_t>
    auto serve(channel_ptr_t ch) {
        return forever()
            >> net::recv(ch->session_sched, ch->session_id)
            >> then([ch](net::packet_buffer* buf) {
                process_all_messages(ch, buf);
                ch->pending.check_timeouts(
                    scheduler::task::scheduler::now_ms());
            })
            >> any_exception([ch](std::exception_ptr e) {
                ch->on_connection_lost(e);
                return just();
            });
    }
}
```

---

## 九、典型使用流程

### 9.1 服务端

```cpp
// 1. 已有 net 层 scheduler
auto* accept_sched  = get_accept_scheduler();
auto* session_sched = get_session_scheduler();
auto* task_sched    = get_task_scheduler();

// 2. 定义 Codec（或使用 raw_codec）
using MyCodec = pump::rpc::raw_codec;

// 3. 等待连接并创建 Channel
net::wait_connection(accept_sched)
    >> then([session_sched, task_sched](net::session_id_t sid) {
        // 创建 RPC Channel
        auto ch = rpc::make_channel<MyCodec>(sid, session_sched, task_sched);

        // 注册 handler —— 统一返回 sender
        ch->dispatcher().on(METHOD_HELLO, [](rpc::handler_context&& ctx) {
            auto req = MyCodec{}.decode_payload<HelloRequest>(ctx.payload);
            return just(HelloResponse{.message = "world"});
        });

        ch->dispatcher().on(METHOD_ADD, [](rpc::handler_context&& ctx) {
            auto req = MyCodec{}.decode_payload<AddRequest>(ctx.payload);
            return just(AddResponse{.result = req.a + req.b});
        });

        // 异步 handler（也返回 sender，完全一致的注册接口）
        ch->dispatcher().on(METHOD_GET, [task_sched](rpc::handler_context&& ctx) {
            auto key = MyCodec{}.decode_payload<GetRequest>(ctx.payload).key;
            return just(std::move(key))
                >> on(task_sched->as_task())
                >> then([](auto&& k) { return db_lookup(k); })
                >> then([](auto&& val) { return GetResponse{.value = val}; });
        });

        // Notification handler（也返回 sender）
        ch->dispatcher().on_notification(METHOD_HEARTBEAT,
            [](rpc::handler_context&& ctx) {
                return just(); // fire-and-forget
            });

        // 启动接收循环
        return net::join(session_sched, sid)
            >> then([ch]() {
                return rpc::serve(ch);
            }) >> flat();
    }) >> flat()
    >> submit(core::make_root_context(runtime_scope));
```

### 9.2 客户端

```cpp
// 1. 连接到服务端
net::connect(connect_sched, "127.0.0.1", 8080)
    >> then([session_sched, task_sched](net::session_id_t sid) {
        auto ch = rpc::make_channel<MyCodec>(sid, session_sched, task_sched);

        return net::join(session_sched, sid)
            >> then([ch]() {
                // 启动接收循环（后台，处理 Response）
                rpc::serve(ch) >> submit(/* ctx */);

                // 发起 RPC 调用
                return rpc::call(ch, METHOD_HELLO, HelloRequest{.name = "pump"}, 5000);
            }) >> flat();
    }) >> flat()
    >> then([](rpc::payload_view pv) {
        auto resp = MyCodec{}.decode_payload<HelloResponse>(pv);
        // 处理响应...
    })
    >> any_exception([](std::exception_ptr e) {
        // 超时或连接断开
        return just();
    })
    >> submit(ctx);
```

### 9.3 双向 RPC

```cpp
// 两端都注册 handler + 都可以发起 call
auto ch = rpc::make_channel<RaftCodec, RaftState>(sid, session_sched, task_sched);

// 作为服务端：处理对方的请求——统一返回 sender
ch->dispatcher().on(APPEND_ENTRIES, [](rpc::handler_context&& ctx) {
    auto req = RaftCodec{}.decode_payload<AppendEntriesReq>(ctx.payload);
    return just(handle_append_entries(std::move(req)));
});
ch->dispatcher().on(REQUEST_VOTE, [](rpc::handler_context&& ctx) {
    auto req = RaftCodec{}.decode_payload<RequestVoteReq>(ctx.payload);
    return just(handle_request_vote(std::move(req)));
});

// 启动接收循环
rpc::serve(ch) >> submit(ctx);

// 作为客户端：向对方发起请求
rpc::call(ch, APPEND_ENTRIES, AppendEntriesReq{...})
    >> then([](rpc::payload_view pv) { ... })
    >> submit(ctx);
```

### 9.4 并发 RPC 调用

```cpp
// when_all 并发
when_all(
    rpc::call(ch, METHOD_A, req_a),
    rpc::call(ch, METHOD_B, req_b),
    rpc::call(ch, METHOD_C, req_c)
) >> then([](auto&& results) {
    // 三个响应都到达后执行
});

// 扇出到多个 peer
for_each(peer_channels)
    >> concurrent()
    >> then([req](auto& peer_ch) {
        return rpc::call(peer_ch, METHOD_VOTE, req);
    }) >> flat()
    >> reduce(0, [](int votes, auto&& resp) {
        return votes + (resp.granted ? 1 : 0);
    });
```

---

## 十、连接管理

### 10.1 连接断开处理

```cpp
template<typename ...Ts>
void channel<Ts...>::on_connection_lost(std::exception_ptr e) {
    if (status == channel_status::closed) return;
    status = channel_status::closed;

    // 所有 pending 请求收到异常
    pending.fail_all(std::make_exception_ptr(connection_lost_error()));

    // 关闭底层 session
    // net::stop 通过 session_scheduler 执行
    auto* stop_req = new net::common::stop_req{
        session_id,
        [](bool) {}
    };
    session_sched->schedule(stop_req);
}
```

### 10.2 优雅关闭

```cpp
template<typename ...Ts>
auto channel<Ts...>::close() {
    if (status != channel_status::active) return;
    status = channel_status::draining;

    // 如果无 pending 请求，直接关闭
    if (pending.active_count == 0) {
        status = channel_status::closed;
        // net::stop ...
        return;
    }

    // 否则等待 pending 清空或超时
    // 通过 task_scheduler 设置一个 drain 超时
    // 超时后 fail_all 并关闭
}
```

### 10.3 心跳（可选）

```cpp
// 心跳作为独立的 pipeline 运行
auto start_heartbeat(channel_ptr_t ch) {
    return forever()
        >> on(ch->task_sched->delay(ch->config.heartbeat_interval_ms))
        >> then([ch]() {
            if (ch->status != channel_status::active) {
                // 停止心跳
                throw channel_closed_error();
            }
            // 发送心跳 Notification
            rpc::notify(ch, METHOD_HEARTBEAT, HeartbeatMsg{});
        })
        >> any_exception([](std::exception_ptr) {
            return just(); // 停止心跳循环
        });
}
```

---

## 十一、错误处理流程

### 11.1 完整错误传播路径

```
                       调用方                           服务端
                         │                               │
    rpc::call ──────────►│   [Request]                   │
         │               │ ─────────────────────────────►│
         │               │                               │── handler 执行
         │               │                               │
         │               │   [Response] 或 [Error]       │
         │               │◄───────────────────────────── │
         │               │                               │
    pending_map ─────────┤
         │               │
    ┌────┴─────┐         │
    │ Response │         │
    │ payload  │───► op_pusher<pos+1>::push_value ──► then([](payload_view) {...})
    └──────────┘
    ┌────┴─────┐
    │  Error   │
    │ exception│───► op_pusher<pos+1>::push_exception ──► any_exception(...)
    └──────────┘
    ┌────┴─────┐
    │ Timeout  │
    │ (扫描)   │───► pending_map::check_timeouts ──► cb(exception_ptr) ──► push_exception
    └──────────┘
    ┌────┴─────┐
    │ 连接断开  │
    │          │───► pending_map::fail_all ──► 所有 cb(exception_ptr)
    └──────────┘
```

### 11.2 服务端 Handler 异常处理

```
Handler 执行
    │
    ├── 正常返回 Response ──► codec.encode_payload ──► send_response
    │
    ├── 抛出异常 ──► catch ──► send_error(remote_error)
    │
    └── 返回 sender ──► sender 执行
            │
            ├── sender 产生值 ──► encode ──► send_response
            │
            └── sender 产生异常 ──► catch ──► send_error(remote_error)
```

---

## 十二、性能设计要点

### 12.1 零拷贝读路径

```
              Net recv (readv 到 ring buffer)
                        │
                        ▼
              packet_buffer (SPSC ring buffer)
                        │
              ┌─────────┴──────────┐
              │ has_full_pkt 检查   │  ← O(1)，只看 head/tail + 2字节长度
              │ get_recv_pkt 取包   │  ← 返回 iovec 视图，不拷贝
              └─────────┬──────────┘
                        │
              parse_header (7B) ← memcpy 到栈，7字节可忽略
                        │
              payload_view ← 指向 ring buffer，零拷贝
                        │
              ┌─────────┴──────────┐
              │  Response:         │
              │  cb(payload_view)  │ ← 直接传递视图
              │                    │
              │  Request:          │
              │  handler(pv)       │ ← handler 从视图解码
              └────────────────────┘
```

### 12.2 高效写路径

```
              encode_result ← 小 payload 内联到栈 (inline_buf[64])
                        │
              ┌─────────┴──────────┐
              │ iovec[0]: header   │ ← 7B 栈上 buffer
              │ iovec[1]: payload  │ ← inline_buf 或 heap_buf
              └─────────┬──────────┘
                        │
              net::send (prepare_send_vec)
                        │
              ┌─────────┴──────────┐
              │ iovec[0]: uint16_t │ ← 2B 长度前缀（Net层添加）
              │ iovec[1]: header   │
              │ iovec[2]: payload  │
              └─────────┬──────────┘
                        │
              writev (scatter-gather，一次系统调用)
```

### 12.3 热路径无堆分配

| 组件 | 分配策略 |
|------|----------|
| pending_map | 预分配固定数组（MaxPending 个 slot） |
| dispatcher | 预分配固定数组（MAX_METHODS 个 handler） |
| rpc_header | 栈上 7 字节 |
| encode_result | 小 payload 内联 64 字节；超出时堆分配（冷路径） |
| send_req | `new` 分配 ← 唯一热路径堆分配（与 Net 层一致） |
| payload_view | 栈上 iovec 视图 |

**说明**：`send_req` 的 `new` 是 Net 层已有模式，保持一致。后续可优化为 pool allocator。

### 12.4 Cache 友好性

- `pending_map`: `std::array` 线性存储，O(1) 查找（open addressing）
- `dispatcher`: `std::array` 连续存储，method_id 直接索引
- 无指针追逐（无链表、无 `std::unordered_map`）

---

## 十三、扩展点总结

| 扩展点 | 机制 | 使用方式 |
|--------|------|----------|
| Codec | 模板参数 `channel<Codec, ...>` | 协议实现自己的 `decode_payload<T>` / `encode_payload` |
| ProtocolState | 模板参数 `channel<..., ProtocolState>` | `ch->protocol_state.xxx` 访问 per-session 状态 |
| Handler | `dispatcher.on(method_id, handler)` | 运行期注册 |
| Static Dispatch | `static_dispatcher<ProtocolDef>` | 编译期注册（可选） |
| Connection Policy | `channel_config` | 超时、心跳、最大并发请求数 |
| 大消息 | 后续版本扩展 `uint32_t` 长度前缀 + 分片 | 当前 64KB 限制 |
| 流式传输 | Phase 2 预留 | 当前不实现 |

---

## 十四、实现分步计划

### Phase 1：核心骨架

| 步骤 | 内容 | 产出文件 |
|------|------|----------|
| 1 | 消息类型枚举 + RPC header 解析/构造 | `common/message_type.hh`, `common/header.hh` |
| 2 | 错误类型定义 | `common/error.hh` |
| 3 | Codec concept + payload_view + raw_codec | `common/codec_concept.hh`, `codec/raw_codec.hh` |
| 4 | pending_map 实现 | `channel/pending_map.hh` |
| 5 | dispatcher 实现 | `channel/dispatcher.hh` |
| 6 | channel 结构 + make_channel | `channel/channel.hh` |

### Phase 2：Sender 实现

| 步骤 | 内容 | 产出文件 |
|------|------|----------|
| 7 | rpc::call sender + op_pusher/compute_sender_type 特化 | `senders/call.hh` |
| 8 | rpc::notify sender | `senders/notify.hh` |
| 9 | rpc::serve 接收循环 | `senders/serve.hh` |
| 10 | 统一对外头文件 | `rpc.hh` |

### Phase 3：集成与测试

| 步骤 | 内容 | 产出文件 |
|------|------|----------|
| 11 | echo RPC 示例（验证基础功能） | `apps/example/rpc_echo/` |
| 12 | 超时测试 | `apps/test/rpc_timeout_test.cc` |
| 13 | 并发 RPC 调用测试 | `apps/test/rpc_concurrent_test.cc` |
| 14 | 连接断开恢复测试 | `apps/test/rpc_disconnect_test.cc` |

### Phase 4：优化与扩展

| 步骤 | 内容 |
|------|------|
| 15 | 心跳/保活 |
| 16 | 优雅关闭 |
| 17 | send_req pool allocator |
| 18 | 编译期 static_dispatcher |

---

## 十五、关键设计决策记录

| # | 决策 | 理由 |
|---|------|------|
| D1 | Channel 使用 `shared_ptr` 管理 | recv_loop、pending callbacks、用户 pipeline 都持有引用，需要共享所有权 |
| D2 | pending_map 用 open-addressing 数组 | cache 友好、预分配、无堆分配 |
| D3 | 超时用 recv 触发的批量扫描 | 简单高效，避免 per-request 定时器开销 |
| D4 | Codec 只负责 payload | RPC header 固定 7 字节，框架统一处理，Codec 职责单一 |
| D5 | notify 用 `just() >> then(...)` 实现 | fire-and-forget，不需要 pending_map，不需要自定义 op |
| D6 | call 使用自定义 op/sender/op_pusher | 核心异步操作，需要精确控制 pipeline 挂起/恢复 |
| D7 | Response callback 在 session 所在 core 执行 | 与 command.md Q4 一致，避免跨核同步 |
| D8 | Error response 使用独立消息类型 | 与普通 Response 区分，便于框架层统一处理 |
| D9 | dispatcher 数组大小固定 256 | method_id 为 uint16_t，但实际协议方法数通常 <256；如需更多可调整 |
| D10 | encode_result 内联 64 字节 | 大多数小消息（<64B payload）零堆分配 |
| D11 | Handler 统一返回 sender | 同步/异步统一接口，`on()` 只有一个注册方法；同步用 `just()` 包装，异步直接返回 pipeline；与 PUMP sender 语义一致，无需区分 `on()` / `on_async()` |
