# DPDK 支持需求文档

## 1. 目标

为 PUMP 框架的 **UDP scheduler** 和 **KCP scheduler** 增加 DPDK 后端，实现用户态网络栈收发包，绕过内核协议栈，降低延迟、提高吞吐。

### 1.1 设计约束

| 约束 | 说明 |
|------|------|
| 上层 API 不变 | `udp::recv()` / `udp::send()` / `kcp::recv()` / `kcp::send()` 等 sender API 签名和语义完全不变 |
| 现有后端保留 | io_uring / epoll 后端继续工作，DPDK 作为第三种后端并列存在 |
| 单线程模型不变 | 每个 scheduler 实例仍绑定单核单线程运行，DPDK 的 port/queue 也按核心分配 |
| 无锁 | DPDK 后端不得引入任何锁/CAS（DPDK 本身的 per-core TX/RX queue 天然满足） |
| 编译期后端选择 | 与 io_uring / epoll 相同的模板参数化方式选择后端，不引入运行时多态 |

### 1.2 不包含的范围

- TCP scheduler 的 DPDK 支持（需要完整用户态 TCP 栈，复杂度远超本期目标）
- DPDK 的 flow director / RSS 高级特性（初期不涉及，后续按需添加）
- IPv6 支持（与现有 UDP/KCP 的 IPv4 限制一致）
- 多 port（多网卡）支持（初期单 port，后续扩展）

---

## 2. 现有架构分析

### 2.1 分层结构

```
┌─────────────────────────────────────────────────┐
│ Sender API: udp::recv/send, kcp::recv/send/...  │  ← 不变
├─────────────────────────────────────────────────┤
│ Scheduler: udp::scheduler, kcp::scheduler       │  ← 不变（模板参数化 transport）
├─────────────────────────────────────────────────┤
│ dgram transport: dgram::io_uring / dgram::epoll │  ← 新增 dgram::dpdk
├─────────────────────────────────────────────────┤
│ 系统层: io_uring / epoll / socket               │  ← DPDK rte_ethdev
└─────────────────────────────────────────────────┘
```

### 2.2 关键接口：dgram transport

UDP scheduler 和 KCP scheduler 均通过 `dgram::xxx::transport` 操作底层 I/O。当前两个后端的 transport 接口：

```cpp
// dgram::io_uring::transport
int  init(const char* address, uint16_t port, const transport_config& cfg);
void sendto(const char* data, uint32_t len, const sockaddr_in& dest);  // KCP 同步发送
bool enqueue_sendmsg(msghdr* msg, void* user_data);                    // UDP 异步发送
void flush();                                                          // 提交 io_uring SQE
void advance(RecvHandler, SendHandler);                                // 收割完成事件

// dgram::epoll::transport
int     init(const char* address, uint16_t port, const transport_config& cfg);
void    sendto(const char* data, uint32_t len, const sockaddr_in& dest);
ssize_t try_sendmsg(msghdr* msg);
bool    advance(RecvHandler);  // 返回是否 writable
```

### 2.3 UDP scheduler 使用 transport 的方式

- **接收**：`advance()` 中调 `_transport.advance(recv_handler, send_handler)`，recv_handler 收到 `(buf, len, src_addr)` 后复制到 `datagram` 对象分发给上层
- **发送**：
  - io_uring 后端：`enqueue_sendmsg()` + `flush()` 批量异步发送
  - epoll 后端：`try_sendmsg()` 同步发送，EAGAIN 时暂存到 `pending_sends`

### 2.4 KCP scheduler 使用 transport 的方式

- **接收**：`advance()` 中调 `_transport.advance(on_raw_packet_handler, noop_send_handler)`，raw packet 分发到 handshake 或 KCP input
- **发送**：KCP output callback 调 `_transport.sendto()` 同步发送（fire-and-forget）
- **不使用** io_uring 的异步 sendmsg

---

## 3. DPDK transport 层设计

### 3.1 新增文件

```
src/env/scheduler/dgram/
├── common.hh          ← 已有（create_bound_udp_socket 等）
├── epoll.hh           ← 已有
├── io_uring.hh        ← 已有
└── dpdk.hh            ← 新增：dgram::dpdk::transport
```

### 3.2 transport 接口

`dgram::dpdk::transport` 需要提供与现有 transport 相同的接口集合，供 UDP scheduler 和 KCP scheduler 共用：

```cpp
namespace pump::scheduler::dgram::dpdk {

struct transport {
    // --- 初始化 ---
    // 绑定到指定 DPDK port 的一个 RX/TX queue pair
    // port_id: DPDK ethdev port index
    // queue_id: 该 port 上的 queue index（通常 = core_id）
    // local_ip / local_port: 本端 IP/端口（用于收包过滤和发包构建）
    int init(uint16_t port_id, uint16_t queue_id,
             uint32_t local_ip, uint16_t local_port,
             const transport_config& cfg = {});

    // --- 同步发送（fire-and-forget）---
    // KCP output callback 使用
    void sendto(const char* data, uint32_t len, const sockaddr_in& dest);

    // --- 异步发送（批量）---
    // UDP scheduler (io_uring 风格) 使用
    // 将 payload 构建为完整以太网帧 + IP + UDP，入队到 TX ring
    bool enqueue_sendmsg(msghdr* msg, void* user_data);
    void flush();  // rte_eth_tx_burst

    // --- 同步发送（epoll 风格）---
    // UDP scheduler (epoll 风格) 使用
    ssize_t try_sendmsg(msghdr* msg);

    // --- 轮询收发 ---
    // io_uring 风格: advance(recv_handler, send_handler)
    template<typename RecvHandler, typename SendHandler>
    void advance(RecvHandler&&, SendHandler&&);

    // epoll 风格: advance(recv_handler) -> bool (writable)
    template<typename RecvHandler>
    bool advance(RecvHandler&&);
};

}
```

### 3.3 核心实现要点

#### 3.3.1 接收（RX）

```
rte_eth_rx_burst(port_id, queue_id, mbufs, burst_size)
    │
    ▼ 对每个 mbuf
解析 Ethernet → IP → UDP header
    │
    ├─ 非 UDP / 目标端口不匹配 → rte_pktmbuf_free(mbuf) → 跳过
    │
    └─ 匹配 → 提取 payload 指针 + len + 源 sockaddr_in
           → 调用 recv_handler(payload, len, src_addr)
           → rte_pktmbuf_free(mbuf)
```

- **不需要** `new char[]` 拷贝到 `datagram`，因为 recv_handler 在 advance 回调期间使用 mbuf 内的数据是安全的（与 io_uring recv_slot / epoll recv_buf 行为一致：buf 仅在 callback 期间有效）
- UDP scheduler 的 `on_recv()` 已经会做 `new char[]` 拷贝，DPDK transport 不需要额外操作

#### 3.3.2 发送（TX）

**`sendto()`**（KCP 使用）：
1. 从 mempool 分配 mbuf
2. 填充 Ethernet + IP + UDP header + payload（memcpy）
3. `rte_eth_tx_burst(port_id, queue_id, &mbuf, 1)` 立即发送
4. 若发送失败（ring 满），`rte_pktmbuf_free(mbuf)` 丢弃

**`enqueue_sendmsg()`**（UDP io_uring 风格使用）：
1. 从 mempool 分配 mbuf
2. 从 `msghdr` 提取目标地址和 payload，填充完整帧
3. mbuf 入队到本地 TX 缓冲数组
4. 记录 `user_data` 到回调列表
5. `flush()` 时调 `rte_eth_tx_burst()` 批量发送，对每个完成的 mbuf 触发 send_handler

**`try_sendmsg()`**（UDP epoll 风格使用）：
1. 构建 mbuf + 完整帧
2. `rte_eth_tx_burst()` 立即发送
3. 返回发送字节数或 -1（ring 满时设 `errno = EAGAIN`）

#### 3.3.3 帧构建

transport 内部需要构建完整的 L2-L4 帧：

```
┌──────────┬──────────┬──────────┬─────────┐
│ Ethernet │   IPv4   │   UDP    │ payload │
│  14 B    │  20 B    │  8 B     │  N B    │
└──────────┴──────────┴──────────┴─────────┘
```

- **Ethernet header**：源 MAC = 本端 port MAC（`rte_eth_macaddr_get`），目标 MAC 需要通过 ARP 或静态配置获取
- **IP header**：源 IP = init 时传入的 `local_ip`，目标 IP 从 `sockaddr_in` 获取，TTL=64，protocol=UDP
- **UDP header**：源端口 = `local_port`，目标端口从 `sockaddr_in` 获取
- **Checksum**：利用 DPDK offload（`PKT_TX_IP_CKSUM | PKT_TX_UDP_CKSUM`），无需软件计算

#### 3.3.4 ARP 处理

DPDK 绕过内核，需要自行处理 ARP：

**方案 A：静态 ARP 表（初期推荐）**
- `init()` 时传入目标 MAC 或静态 ARP 表
- 适合同一 L2 交换域内的已知节点部署
- 足够简单，满足大部分低延迟场景（同机房/同 VLAN）

**方案 B：简单 ARP 协议实现（后续扩展）**
- 在 `advance()` 的 RX 路径中识别 ARP request/reply
- 维护 IP→MAC 映射缓存
- 发送前查 ARP 表，miss 时发 ARP request 并暂存 packet

初期采用方案 A，后续可按需升级到方案 B。

#### 3.3.5 mempool 管理

- 每个 transport 实例使用独立的 `rte_mempool`（per-core，无竞争）
- mbuf 数量 = `cfg.recv_depth`（RX 描述符）+ TX ring size + headroom
- mbuf data room = `cfg.max_dgram_size` + L2/L3/L4 header 开销（42 字节）

### 3.4 transport_config 扩展

```cpp
struct transport_config {
    unsigned queue_depth = 256;         // 现有：io_uring ring size
    uint32_t max_dgram_size = 65536;    // 现有：max recv datagram size
    uint32_t recv_depth = 32;           // 现有：recv slot count

    // DPDK 扩展
    uint16_t dpdk_port_id = 0;         // ethdev port index
    uint16_t dpdk_queue_id = 0;        // RX/TX queue index
    uint32_t dpdk_mbuf_count = 8192;   // mempool mbuf 数量
    uint16_t dpdk_rx_burst = 32;       // 每次 rx_burst 最大收包数
    uint16_t dpdk_tx_burst = 32;       // 每次 tx_burst 最大发包数
};
```

也可以为 DPDK 定义独立的 config struct（`dpdk_transport_config`），避免不相关字段干扰。

---

## 4. UDP scheduler DPDK 后端

### 4.1 新增文件

```
src/env/scheduler/udp/
├── common/struct.hh        ← 不变
├── senders/recv.hh         ← 不变
├── senders/send.hh         ← 不变
├── udp.hh                  ← 不变
├── epoll/scheduler.hh      ← 不变
├── io_uring/scheduler.hh   ← 不变
└── dpdk/scheduler.hh       ← 新增
```

### 4.2 实现方式

`udp::dpdk::scheduler` 与现有的 `udp::io_uring::scheduler` 结构完全相同，唯一区别是将 `dgram::io_uring::transport` 替换为 `dgram::dpdk::transport`：

```cpp
namespace pump::scheduler::udp::dpdk {
    template <
        template<typename> class recv_op_t,
        template<typename> class send_op_t
    >
    struct scheduler {
        // 唯一区别：transport 类型
        dgram::dpdk::transport _transport;

        // 以下字段和方法与 io_uring::scheduler 完全一致
        core::per_core::queue<common::recv_req*, 2048> recv_q;
        core::per_core::queue<common::send_req*, 2048> send_q;
        // ... drain_recv_q(), drain_send_q(), on_recv(), advance() 等
    };
}
```

由于 UDP scheduler 的两个现有后端（io_uring / epoll）逻辑高度重复，可以考虑在此次变更中将 scheduler 的公共逻辑提取为模板基类，transport 类型作为模板参数：

```cpp
template <typename transport_t, ...>
struct scheduler_base {
    transport_t _transport;
    // 公共逻辑：recv_q, send_q, drain_recv_q, drain_send_q, on_recv...
};

// io_uring 后端
namespace udp::io_uring {
    using scheduler = scheduler_base<dgram::io_uring::transport, ...>;
}
// dpdk 后端
namespace udp::dpdk {
    using scheduler = scheduler_base<dgram::dpdk::transport, ...>;
}
```

**是否做此重构由实现时决定**。不做也完全可以，只是代码重复。

### 4.3 init 接口

```cpp
// 新的 init 重载（DPDK 特有参数）
int init(uint16_t dpdk_port_id, uint16_t dpdk_queue_id,
         uint32_t local_ip, uint16_t local_port,
         const dpdk_config& cfg = {});
```

注意：与 io_uring / epoll 的 `init(address, port)` 签名不同，因为 DPDK 不使用 socket。

### 4.4 advance 行为差异

| 步骤 | io_uring | epoll | DPDK |
|------|----------|-------|------|
| 收包 | `io_uring_peek_cqe` | `epoll_wait` + `recvfrom` | `rte_eth_rx_burst` |
| 发包提交 | `io_uring_submit` | `sendmsg` | `rte_eth_tx_burst` |
| 发送完成通知 | CQE + send_handler | 同步返回 | burst 返回即完成 |
| 返回值 | void | bool (writable) | 跟随 io_uring 风格（void/advance 合一） |

DPDK 发送是同步的（`rte_eth_tx_burst` 立刻把 mbuf 推入 NIC TX ring），不需要异步 send_handler 回调。`advance()` 中对 send 完成的 mbuf 直接触发回调即可。

---

## 5. KCP scheduler DPDK 后端

### 5.1 新增文件

```
src/env/scheduler/kcp/
├── common/{struct.hh, ikcp.hh}  ← 不变
├── senders/{recv,send,accept,connect}.hh  ← 不变
├── kcp.hh           ← 不变
├── transport.hh     ← 不变
├── epoll/scheduler.hh     ← 不变
├── io_uring/scheduler.hh  ← 不变
└── dpdk/scheduler.hh      ← 新增
```

### 5.2 实现方式

与 UDP 相同，KCP 的两个现有后端也高度重复，DPDK 后端只替换 transport：

```cpp
namespace pump::scheduler::kcp::dpdk {
    template <
        template<typename> class recv_op_t,
        template<typename> class send_op_t,
        template<typename> class accept_op_t,
        template<typename> class connect_op_t
    >
    struct scheduler {
        dgram::dpdk::transport _transport;
        // 其余完全复用 kcp::epoll::scheduler / kcp::io_uring::scheduler 的逻辑
    };
}
```

### 5.3 KCP 特殊考虑

- KCP 的 `sendto()` 是同步的 fire-and-forget，DPDK 天然适合（`rte_eth_tx_burst` 也是同步的）
- KCP 不使用 io_uring 的异步 sendmsg，DPDK 后端的 `advance()` 只需要 recv_handler（与 epoll 风格一致）
- handshake 包的 `sendto()` 也走 DPDK，无需特殊处理

---

## 6. DPDK 全局初始化

### 6.1 EAL 初始化

DPDK 需要全局初始化一次（`rte_eal_init`），这应在 `share_nothing::run()` 之前由应用层调用：

```cpp
// 应用层 main() 中
int main(int argc, char** argv) {
    // 1. DPDK EAL 初始化（全局一次）
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) return -1;
    argc -= ret; argv += ret;

    // 2. 配置 ethdev port
    dpdk::port_config port_cfg{};
    port_cfg.port_id = 0;
    port_cfg.num_queues = num_cores;  // 每核一个 queue pair
    dpdk::init_port(port_cfg);

    // 3. 正常创建 scheduler + runtime
    // 每个核心的 scheduler 使用对应的 queue_id
}
```

### 6.2 辅助函数（可选）

在 `dgram/dpdk.hh` 或独立的 `env/dpdk/init.hh` 中提供：

```cpp
namespace pump::scheduler::dgram::dpdk {
    // port 配置 & 初始化
    int init_port(uint16_t port_id, uint16_t num_queues,
                  uint16_t rx_ring_size = 1024, uint16_t tx_ring_size = 1024);

    // 获取 port MAC
    void get_mac(uint16_t port_id, uint8_t mac[6]);
}
```

### 6.3 与 share_nothing 的集成

`share_nothing.hh` 中 `run()` 为每个核心设置 `this_core_id` 并运行 advance 循环。DPDK 后端无需修改 share_nothing，因为：
- DPDK 的 `rte_eal_init` 在 `run()` 之前完成
- 每个核心的 scheduler 在 `run()` 中通过自己的 queue_id 独立操作，不存在跨核竞争
- `advance()` 中的 `rte_eth_rx_burst` / `rte_eth_tx_burst` 本身是 per-queue 无锁的

---

## 7. 配置与构建

### 7.1 CMake

```cmake
pkg_check_modules(DPDK REQUIRED libdpdk)
include_directories(${DPDK_INCLUDE_DIRS})
link_directories(${DPDK_LIBRARY_DIRS})
```

- DPDK 作为必须依赖，构建环境需安装 libdpdk

### 7.2 头文件包含

应用层按需选择后端：

```cpp
// 传统后端
#include "env/scheduler/udp/io_uring/scheduler.hh"

// DPDK 后端
#include "env/scheduler/udp/dpdk/scheduler.hh"
```

---

## 8. 示例

### 8.1 UDP echo (DPDK)

```cpp
#include "env/scheduler/udp/dpdk/scheduler.hh"
#include "env/scheduler/udp/udp.hh"

int main(int argc, char** argv) {
    rte_eal_init(argc, argv);
    dgram::dpdk::init_port(0, 1);

    using scheduler_t = udp::dpdk::scheduler<
        udp::senders::recv::op,
        udp::senders::send::op
    >;

    scheduler_t sche;
    sche.init(/* dpdk_port_id */ 0, /* queue_id */ 0,
              /* local_ip */ inet_addr("10.0.0.1"), /* local_port */ 9000);

    auto ctx = make_root_context();

    // echo: recv → send back
    udp::recv(&sche)
        >> then([&sche](udp::common::datagram&& dg, udp::common::endpoint ep) {
            return udp::send(&sche, ep, dg.release(), dg.size());
        })
        >> flat()
        >> submit(ctx);

    while (sche.advance()) {}
}
```

---

## 9. 测试计划

### 9.1 单元测试

| 测试项 | 说明 |
|--------|------|
| 帧构建 | 验证 Ethernet + IP + UDP header 构建正确性（checksum、字段填充） |
| 帧解析 | 验证 RX 帧解析：提取 payload、源地址、端口过滤 |
| mbuf 生命周期 | 验证 mbuf 分配/释放无泄漏 |
| sendto 语义 | 验证 fire-and-forget 发送（KCP 场景） |
| enqueue + flush | 验证批量发送（UDP io_uring 风格） |

### 9.2 集成测试

| 测试项 | 说明 |
|--------|------|
| UDP echo (DPDK) | DPDK ↔ DPDK 或 DPDK ↔ 内核 echo |
| KCP echo (DPDK) | KCP over DPDK 双端 echo |
| RPC over KCP+DPDK | 验证 RPC transport trait 在 KCP+DPDK 上工作 |
| 混合部署 | 一端 DPDK、一端 io_uring/epoll |

### 9.3 性能对比

| 指标 | 对比基线 |
|------|---------|
| 单包 RTT 延迟 | DPDK vs io_uring vs epoll |
| 小包吞吐（64B） | PPS 对比 |
| 大包吞吐（1KB） | Gbps 对比 |
| CPU 占用 | 满载时 CPU 利用率 |

---

## 10. 实现阶段

### Phase 1：dgram::dpdk transport（核心）
1. 实现 `dgram/dpdk.hh`：init、RX burst + 解析、TX 帧构建 + burst
2. 静态 ARP 表
3. 帧构建/解析单元测试

### Phase 2：UDP dpdk scheduler
1. `udp/dpdk/scheduler.hh`（复制 io_uring 版本，替换 transport 类型）
2. UDP echo 示例 + 集成测试

### Phase 3：KCP dpdk scheduler
1. `kcp/dpdk/scheduler.hh`（复制 io_uring 版本，替换 transport 类型）
2. KCP echo 示例 + 集成测试

### Phase 4：（可选）重构去重
1. 将 UDP scheduler 的三个后端提取为 `scheduler_base<transport_t>`
2. 将 KCP scheduler 的三个后端提取为 `scheduler_base<transport_t>`
3. 更新 CMake 和示例

---

## 11. 风险与注意事项

| 风险 | 应对 |
|------|------|
| DPDK 大页内存配置 | 文档说明 hugepages 配置要求，示例提供 setup 脚本 |
| ARP 表维护 | 初期静态配置足够；跨子网需要网关 MAC 或 ARP 协议实现 |
| mbuf 耗尽 | mempool 大小需要根据并发量合理配置，耗尽时 sendto 丢包（与 UDP 语义一致） |
| 非 DPDK 对端通信 | DPDK 构建的 UDP 帧是标准以太网帧，内核协议栈的 socket 可以正常收发 |
| 帧大小限制 | MTU 1500 时 payload 最大 1472 字节（1500 - 20 IP - 8 UDP），大包需要 IP 分片或 jumbo frame |
| DPDK 版本兼容 | 依赖 `rte_ethdev.h` 等稳定 API，避免使用 experimental API |
