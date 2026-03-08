
#ifndef ENV_SCHEDULER_DGRAM_DPDK_HH
#define ENV_SCHEDULER_DGRAM_DPDK_HH

#include <cstdint>
#include <cstring>
#include <unordered_map>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include "./common.hh"

namespace pump::scheduler::dgram::dpdk {

    // L2+L3+L4 header total size: Ethernet(14) + IPv4(20) + UDP(8) = 42
    static constexpr uint32_t HEADER_SIZE = 14 + 20 + 8;

    struct dpdk_config {
        uint16_t port_id = 0;
        uint16_t queue_id = 0;
        uint32_t local_ip = 0;        // network byte order
        uint16_t local_port = 0;      // host byte order
        uint32_t max_dgram_size = 1500;
        uint32_t mbuf_count = 1024;
        uint16_t rx_burst = 32;
        uint16_t tx_burst = 32;
    };

    // Static ARP table: IP (network order) -> MAC
    struct arp_entry {
        uint32_t ip;                   // network byte order
        rte_ether_addr mac;
    };

    // Initialize a DPDK ethdev port with the given number of RX/TX queue pairs.
    // Creates a shared mempool for RX descriptors internally.
    // Must be called after rte_eal_init() and before creating transport instances.
    // Returns 0 on success, < 0 on failure.
    inline int
    init_port(uint16_t port_id, uint16_t num_queues,
              uint16_t rx_ring_size = 1024, uint16_t tx_ring_size = 1024,
              uint32_t mbuf_count = 1024) {
        // Create shared mempool for RX queues
        char pool_name[64];
        snprintf(pool_name, sizeof(pool_name), "dpdk_rx_pool_%u", port_id);
        auto* rx_pool = rte_pktmbuf_pool_create(
            pool_name, mbuf_count, 256, 0,
            RTE_MBUF_DEFAULT_BUF_SIZE,
            rte_eth_dev_socket_id(port_id));
        if (!rx_pool) return -1;

        rte_eth_conf port_conf{};
        port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
        port_conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;

        int ret = rte_eth_dev_configure(port_id, num_queues, num_queues, &port_conf);
        if (ret < 0) return ret;

        for (uint16_t q = 0; q < num_queues; ++q) {
            ret = rte_eth_rx_queue_setup(port_id, q, rx_ring_size,
                                         rte_eth_dev_socket_id(port_id), nullptr, rx_pool);
            if (ret < 0) return ret;

            ret = rte_eth_tx_queue_setup(port_id, q, tx_ring_size,
                                         rte_eth_dev_socket_id(port_id), nullptr);
            if (ret < 0) return ret;
        }

        ret = rte_eth_dev_start(port_id);
        if (ret < 0) return ret;

        rte_eth_promiscuous_enable(port_id);
        return 0;
    }

    struct
    transport {
    private:
        uint16_t _port_id = 0;
        uint16_t _queue_id = 0;
        uint32_t _local_ip = 0;       // network byte order
        uint16_t _local_port_be = 0;  // network byte order
        uint16_t _rx_burst = 32;
        uint16_t _tx_burst = 32;

        rte_mempool* _mbuf_pool = nullptr;
        bool _own_pool = false;
        rte_ether_addr _local_mac{};

        // Static ARP table: IP (network order) -> MAC
        std::unordered_map<uint32_t, rte_ether_addr> _arp_table;

        // TX batch buffer
        rte_mbuf* _tx_bufs[64]{};
        void* _tx_user_data[64]{};
        uint16_t _tx_count = 0;

        void
        fill_headers(char* pkt, uint32_t dst_ip, uint16_t dst_port_be,
                     uint32_t payload_len) {
            // --- Ethernet header ---
            auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt);
            rte_ether_addr_copy(&_local_mac, &eth->src_addr);

            auto it = _arp_table.find(dst_ip);
            if (it != _arp_table.end()) {
                rte_ether_addr_copy(&it->second, &eth->dst_addr);
            } else {
                memset(&eth->dst_addr, 0xFF, sizeof(rte_ether_addr));
            }
            eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

            // --- IPv4 header ---
            auto* ip = reinterpret_cast<rte_ipv4_hdr*>(pkt + sizeof(rte_ether_hdr));
            ip->version_ihl = 0x45;
            ip->type_of_service = 0;
            ip->total_length = rte_cpu_to_be_16(20 + 8 + payload_len);
            ip->packet_id = 0;
            ip->fragment_offset = 0;
            ip->time_to_live = 64;
            ip->next_proto_id = IPPROTO_UDP;
            ip->hdr_checksum = 0;
            ip->src_addr = _local_ip;
            ip->dst_addr = dst_ip;
            ip->hdr_checksum = rte_ipv4_cksum(ip);

            // --- UDP header ---
            auto* udph = reinterpret_cast<rte_udp_hdr*>(
                pkt + sizeof(rte_ether_hdr) + sizeof(rte_ipv4_hdr));
            udph->src_port = _local_port_be;
            udph->dst_port = dst_port_be;
            udph->dgram_len = rte_cpu_to_be_16(8 + payload_len);
            udph->dgram_cksum = 0;
        }

        rte_mbuf*
        build_udp_packet(const char* payload, uint32_t payload_len,
                         uint32_t dst_ip, uint16_t dst_port_be) {
            auto* mbuf = rte_pktmbuf_alloc(_mbuf_pool);
            if (!mbuf) [[unlikely]] return nullptr;

            uint32_t total = HEADER_SIZE + payload_len;
            auto* pkt = rte_pktmbuf_append(mbuf, total);
            if (!pkt) [[unlikely]] {
                rte_pktmbuf_free(mbuf);
                return nullptr;
            }

            fill_headers(pkt, dst_ip, dst_port_be, payload_len);
            memcpy(pkt + HEADER_SIZE, payload, payload_len);

            mbuf->l2_len = sizeof(rte_ether_hdr);
            mbuf->l3_len = sizeof(rte_ipv4_hdr);

            return mbuf;
        }

        bool
        parse_udp_packet(rte_mbuf* mbuf,
                         const char*& payload, uint32_t& payload_len,
                         sockaddr_in& src_addr) {
            auto total_len = rte_pktmbuf_pkt_len(mbuf);
            if (total_len < HEADER_SIZE) return false;

            auto* pkt = rte_pktmbuf_mtod(mbuf, const char*);

            // Check Ethernet type
            auto* eth = reinterpret_cast<const rte_ether_hdr*>(pkt);
            if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
                return false;

            // Check IP protocol
            auto* ip = reinterpret_cast<const rte_ipv4_hdr*>(
                pkt + sizeof(rte_ether_hdr));
            if (ip->next_proto_id != IPPROTO_UDP)
                return false;

            // Check destination port
            auto* udph = reinterpret_cast<const rte_udp_hdr*>(
                pkt + sizeof(rte_ether_hdr) + sizeof(rte_ipv4_hdr));
            if (udph->dst_port != _local_port_be)
                return false;

            // Extract source address
            src_addr.sin_family = AF_INET;
            src_addr.sin_addr.s_addr = ip->src_addr;
            src_addr.sin_port = udph->src_port;

            // Extract payload
            payload = pkt + HEADER_SIZE;
            payload_len = total_len - HEADER_SIZE;

            return true;
        }

    public:
        transport() = default;

        transport(const transport&) = delete;
        transport& operator=(const transport&) = delete;

        int
        init(const dpdk_config& cfg) {
            _port_id = cfg.port_id;
            _queue_id = cfg.queue_id;
            _local_ip = cfg.local_ip;
            _local_port_be = htons(cfg.local_port);
            _rx_burst = cfg.rx_burst;
            _tx_burst = cfg.tx_burst;

            rte_eth_macaddr_get(_port_id, &_local_mac);

            // Create per-queue mempool
            char pool_name[64];
            snprintf(pool_name, sizeof(pool_name), "dpdk_pool_%u_%u",
                     _port_id, _queue_id);

            uint32_t mbuf_size = RTE_MBUF_DEFAULT_BUF_SIZE;
            if (cfg.max_dgram_size + HEADER_SIZE + RTE_PKTMBUF_HEADROOM > mbuf_size)
                mbuf_size = cfg.max_dgram_size + HEADER_SIZE + RTE_PKTMBUF_HEADROOM;

            _mbuf_pool = rte_pktmbuf_pool_create(
                pool_name, cfg.mbuf_count, 256 /* cache size */,
                0 /* priv size */, mbuf_size,
                rte_eth_dev_socket_id(_port_id));

            if (!_mbuf_pool)
                return -1;

            _own_pool = true;
            return 0;
        }

        int
        init(const char* address, uint16_t port, const transport_config& cfg = {}) {
            dpdk_config dcfg{};
            dcfg.local_ip = inet_addr(address);
            dcfg.local_port = port;
            dcfg.max_dgram_size = cfg.max_dgram_size;
            return init(dcfg);
        }

        ~transport() {
            if (_own_pool && _mbuf_pool) {
                rte_mempool_free(_mbuf_pool);
            }
        }

        // Add a static ARP entry
        void
        add_arp(uint32_t ip_net_order, const rte_ether_addr& mac) {
            _arp_table[ip_net_order] = mac;
        }

        void
        add_arp(const char* ip_str, const uint8_t mac[6]) {
            uint32_t ip = inet_addr(ip_str);
            rte_ether_addr addr;
            memcpy(addr.addr_bytes, mac, 6);
            _arp_table[ip] = addr;
        }

        // Set external mempool (e.g., shared pool from init_port).
        // Must be called before init() if you want to share a pool.
        void
        set_mempool(rte_mempool* pool) {
            _mbuf_pool = pool;
            _own_pool = false;
        }

        [[nodiscard]] uint16_t port_id() const { return _port_id; }
        [[nodiscard]] uint16_t queue_id() const { return _queue_id; }

        // --- Sync sendto (fire-and-forget, used by KCP) ---
        void
        sendto(const char* data, uint32_t len, const sockaddr_in& dest) {
            auto* mbuf = build_udp_packet(data, len,
                                          dest.sin_addr.s_addr, dest.sin_port);
            if (!mbuf) [[unlikely]] return;

            if (rte_eth_tx_burst(_port_id, _queue_id, &mbuf, 1) == 0) {
                rte_pktmbuf_free(mbuf);
            }
        }

        // --- Async sendmsg (io_uring style, used by UDP scheduler) ---
        bool
        enqueue_sendmsg(msghdr* msg, void* user_data) {
            if (_tx_count >= 64) [[unlikely]] return false;

            auto* sa = reinterpret_cast<const sockaddr_in*>(msg->msg_name);
            uint32_t total_payload = 0;
            for (size_t i = 0; i < msg->msg_iovlen; ++i)
                total_payload += msg->msg_iov[i].iov_len;

            auto* mbuf = rte_pktmbuf_alloc(_mbuf_pool);
            if (!mbuf) [[unlikely]] return false;

            auto* pkt = rte_pktmbuf_append(mbuf, HEADER_SIZE + total_payload);
            if (!pkt) [[unlikely]] {
                rte_pktmbuf_free(mbuf);
                return false;
            }

            fill_headers(pkt, sa->sin_addr.s_addr, sa->sin_port, total_payload);

            // Copy payload from iov
            char* dst = pkt + HEADER_SIZE;
            for (size_t i = 0; i < msg->msg_iovlen; ++i) {
                memcpy(dst, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len);
                dst += msg->msg_iov[i].iov_len;
            }

            mbuf->l2_len = sizeof(rte_ether_hdr);
            mbuf->l3_len = sizeof(rte_ipv4_hdr);

            _tx_bufs[_tx_count] = mbuf;
            _tx_user_data[_tx_count] = user_data;
            _tx_count++;
            return true;
        }

        void
        flush() {
            if (_tx_count == 0) return;

            uint16_t sent = rte_eth_tx_burst(_port_id, _queue_id,
                                             _tx_bufs, _tx_count);
            // Free unsent mbufs
            for (uint16_t i = sent; i < _tx_count; ++i) {
                rte_pktmbuf_free(_tx_bufs[i]);
            }
            _tx_count = 0;
        }

        // --- Sync sendmsg (epoll style, used by UDP epoll scheduler) ---
        ssize_t
        try_sendmsg(msghdr* msg) {
            auto* sa = reinterpret_cast<const sockaddr_in*>(msg->msg_name);

            uint32_t total_payload = 0;
            for (size_t i = 0; i < msg->msg_iovlen; ++i)
                total_payload += msg->msg_iov[i].iov_len;

            auto* mbuf = rte_pktmbuf_alloc(_mbuf_pool);
            if (!mbuf) [[unlikely]] {
                errno = EAGAIN;
                return -1;
            }

            auto* pkt = rte_pktmbuf_append(mbuf, HEADER_SIZE + total_payload);
            if (!pkt) [[unlikely]] {
                rte_pktmbuf_free(mbuf);
                errno = EAGAIN;
                return -1;
            }

            // Build headers
            fill_headers(pkt, sa->sin_addr.s_addr, sa->sin_port, total_payload);

            // Copy payload from iov
            char* dst = pkt + HEADER_SIZE;
            for (size_t i = 0; i < msg->msg_iovlen; ++i) {
                memcpy(dst, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len);
                dst += msg->msg_iov[i].iov_len;
            }

            mbuf->l2_len = sizeof(rte_ether_hdr);
            mbuf->l3_len = sizeof(rte_ipv4_hdr);

            if (rte_eth_tx_burst(_port_id, _queue_id, &mbuf, 1) == 0) {
                rte_pktmbuf_free(mbuf);
                errno = EAGAIN;
                return -1;
            }
            return static_cast<ssize_t>(total_payload);
        }

        // --- Advance: io_uring style (recv_handler + send_handler) ---
        // recv_handler: void(const char* buf, uint32_t len, const sockaddr_in& src)
        // send_handler: void(void* user_data, int res)
        template<typename RecvHandler, typename SendHandler>
        void
        advance(RecvHandler&& recv_handler, SendHandler&& send_handler) {
            // RX burst
            rte_mbuf* rx_bufs[64];
            uint16_t nb_rx = rte_eth_rx_burst(_port_id, _queue_id,
                                              rx_bufs, _rx_burst > 64 ? 64 : _rx_burst);

            for (uint16_t i = 0; i < nb_rx; ++i) {
                const char* payload;
                uint32_t payload_len;
                sockaddr_in src_addr{};

                if (parse_udp_packet(rx_bufs[i], payload, payload_len, src_addr)) {
                    recv_handler(payload, payload_len, src_addr);
                }
                rte_pktmbuf_free(rx_bufs[i]);
            }

            // Flush pending TX and notify send completions
            if (_tx_count > 0) {
                uint16_t sent = rte_eth_tx_burst(_port_id, _queue_id,
                                                 _tx_bufs, _tx_count);
                for (uint16_t i = 0; i < sent; ++i) {
                    send_handler(_tx_user_data[i], 1);
                }
                for (uint16_t i = sent; i < _tx_count; ++i) {
                    rte_pktmbuf_free(_tx_bufs[i]);
                    send_handler(_tx_user_data[i], -1);
                }
                _tx_count = 0;
            }
        }

        // --- Advance: epoll style (recv_handler only, returns writable) ---
        template<typename RecvHandler>
        bool
        advance(RecvHandler&& recv_handler) {
            rte_mbuf* rx_bufs[64];
            uint16_t nb_rx = rte_eth_rx_burst(_port_id, _queue_id,
                                              rx_bufs, _rx_burst > 64 ? 64 : _rx_burst);

            for (uint16_t i = 0; i < nb_rx; ++i) {
                const char* payload;
                uint32_t payload_len;
                sockaddr_in src_addr{};

                if (parse_udp_packet(rx_bufs[i], payload, payload_len, src_addr)) {
                    recv_handler(payload, payload_len, src_addr);
                }
                rte_pktmbuf_free(rx_bufs[i]);
            }

            // DPDK TX is always "writable" (ring-based, no EAGAIN concept)
            return true;
        }
    };

}  // namespace pump::scheduler::dgram::dpdk

#endif //ENV_SCHEDULER_DGRAM_DPDK_HH
