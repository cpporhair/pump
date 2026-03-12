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

30 行代码，5 次跨域跳转，体现了 Pump 的核心优势：

- **声明式编排** — 读起来就是自上而下的线性流程，没有回调地狱和状态机
- **零锁并发** — 每个 scheduler 单线程运行，跨域通信走 per-core SPSC 队列，无 CAS 竞争
- **编译期类型安全** — `if constexpr` 分支在编译时展开，Op 平铺到 `std::tuple`，零虚函数开销
- **结构化异常安全** — `ignore_all_exception()` + cache 构成类 RAII 保证，异常路径编译期可见
- **Scheduler = 单线程状态容器** — Leader/Follower 合并写在 `handle()` 中，就是普通顺序代码

完整代码见 `apps/kv/`。

## 核心思路

### 扁平 Tuple，位置驱动

Pump 和 stdexec 的根本区别。stdexec 用递归 Receiver 模型，每层操作嵌套调用 `set_value`，类型深度随 pipeline 长度线性增长。

Pump 在 `connect()` 时把所有算子展平到一个 `std::tuple<Op0, Op1, Op2, ...>` 里——扁平数组，不是递归嵌套。执行时用编译期位置索引 `push_value<pos>` 直接跳到 `pos+1`，在连续内存上线性推进。没有递归深度问题，缓存局部性好，`repeat`、异常恢复只需调整位置索引。

### Share-Nothing 调度

每个 scheduler 实例绑定一个线程，内部无同步原语。跨域通信走 per-core SPSC 队列（每个源核心一条独立队列 + bitmap），连 CAS 都没有。多核通过每核独立实例实现，主循环轮询 `advance()` 驱动所有 scheduler。

框架内置 Task、TCP、UDP、KCP、NVMe、RPC、CUDA 七类调度器，覆盖 CPU 计算、网络 IO、磁盘 IO、GPU 计算四个执行域。网络层支持 io_uring / epoll / DPDK 三种后端。

### 栈式 Context

Pipeline 跨越多个 scheduler 和多层 lambda 时，数据传递不靠闭包捕获——`push_context` / `get_context<T>` / `pop_context` 形成编译期类型安全的栈结构，状态随 pipeline 流动，生命周期自动管理。
