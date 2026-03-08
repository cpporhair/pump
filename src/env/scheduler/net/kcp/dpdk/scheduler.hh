
#ifndef ENV_SCHEDULER_KCP_DPDK_SCHEDULER_HH
#define ENV_SCHEDULER_KCP_DPDK_SCHEDULER_HH

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <list>

#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"

#include "../common/struct.hh"
#include "../common/ikcp.hh"
#include "../common/layers.hh"
#include "../kcp.hh"

#include "env/scheduler/net/dgram/dpdk.hh"

namespace pump::scheduler::kcp::dpdk {

    template <
        typename factory_t,
        template<typename> class accept_op_t,
        template<typename> class connect_op_t
    >
    struct
    scheduler {
        friend struct accept_op_t<scheduler>;
        friend struct connect_op_t<scheduler>;

        using session_t = typename factory_t::template session_type<scheduler>;

    private:
        struct kcp_connection {
            common::ikcp kcp;
            sockaddr_in peer_addr{};
            bool connected = false;
            session_t* session = nullptr;
        };

        dgram::dpdk::transport _transport;

        core::per_core::queue<common::send_req*, 2048>    send_q;
        core::per_core::queue<common::accept_req*, 256>   accept_q;
        core::per_core::queue<common::connect_req*, 256>  connect_q;

        std::unordered_map<uint32_t, kcp_connection> _connections;

        std::list<common::accept_req*> _pending_accepts;
        std::list<common::conv_id_t> _ready_accepts;

        struct pending_connect {
            common::connect_req* req;
            uint32_t conv;
            sockaddr_in target;
            uint32_t retries = 0;
            uint32_t next_retry_ts = 0;
        };
        std::list<pending_connect> _pending_connects;

        uint32_t _next_conv = 1;

    private:
        void
        schedule(common::accept_req* req) {
            accept_q.try_enqueue(req);
        }

        void
        schedule(common::connect_req* req) {
            connect_q.try_enqueue(req);
        }

        void
        send_handshake(uint8_t type, uint32_t conv, const sockaddr_in& addr) {
            common::handshake_pkt pkt{};
            pkt.type = type;
            pkt.conv = conv;
            _transport.sendto(reinterpret_cast<const char*>(&pkt), common::HANDSHAKE_PKT_SIZE, addr);
        }

        session_t*
        _make_session(common::conv_id_t conv) {
            return factory_t::create(conv, this);
        }

        kcp_connection&
        create_connection(uint32_t conv, const sockaddr_in& peer) {
            auto& conn = _connections[conv];
            conn.peer_addr = peer;
            conn.connected = true;

            conn.kcp = common::ikcp(conv, [this, conv](const char* buf, uint32_t len) {
                auto it = _connections.find(conv);
                if (it != _connections.end()) {
                    _transport.sendto(buf, len, it->second.peer_addr);
                }
            });

            conn.kcp.set_nodelay(1, 10, 2, 1);
            conn.kcp.set_wndsize(128, 128);

            conn.session = _make_session(common::conv_id_t{conv});

            return conn;
        }

        void
        drain_accept_q() {
            accept_q.drain([this](common::accept_req* req) {
                if (!_ready_accepts.empty()) {
                    auto conv = _ready_accepts.front();
                    _ready_accepts.pop_front();
                    req->cb(conv);
                    delete req;
                } else {
                    _pending_accepts.push_back(req);
                }
            });
        }

        void
        drain_connect_q(uint32_t now_ms) {
            connect_q.drain([this, now_ms](common::connect_req* req) {
                uint32_t conv = _next_conv++;
                send_handshake(common::HANDSHAKE_SYN, conv, req->target);
                _pending_connects.push_back({req, conv, req->target, 1, now_ms + 500});
            });
        }

        void
        drain_send_q() {
            send_q.drain([this](common::send_req* req) {
                auto it = _connections.find(req->conv.value);
                if (it == _connections.end()) {
                    req->cb(false);
                    delete req;
                    return;
                }

                auto& conn = it->second;
                int ret = conn.kcp.send(req->data, static_cast<int>(req->len));
                delete[] req->data;
                req->cb(ret == 0);
                delete req;
            });
        }

        void
        handle_syn(uint32_t client_conv, const sockaddr_in& peer) {
            if (_connections.count(client_conv)) {
                send_handshake(common::HANDSHAKE_ACK, client_conv, peer);
                return;
            }

            create_connection(client_conv, peer);
            send_handshake(common::HANDSHAKE_ACK, client_conv, peer);

            common::conv_id_t cid{client_conv};
            if (!_pending_accepts.empty()) {
                auto* req = _pending_accepts.front();
                _pending_accepts.pop_front();
                req->cb(cid);
                delete req;
            } else {
                _ready_accepts.push_back(cid);
            }
        }

        void
        handle_ack(uint32_t conv, const sockaddr_in& peer) {
            for (auto it = _pending_connects.begin(); it != _pending_connects.end(); ++it) {
                if (it->conv == conv) {
                    create_connection(conv, peer);
                    common::conv_id_t cid{conv};
                    it->req->cb(cid);
                    delete it->req;
                    _pending_connects.erase(it);
                    return;
                }
            }
        }

        void
        on_raw_packet(const char* buf, uint32_t len, const sockaddr_in& src) {
            if (len == common::HANDSHAKE_PKT_SIZE) {
                auto* pkt = reinterpret_cast<const common::handshake_pkt*>(buf);
                if (pkt->type == common::HANDSHAKE_SYN) {
                    handle_syn(pkt->conv, src);
                    return;
                } else if (pkt->type == common::HANDSHAKE_ACK) {
                    handle_ack(pkt->conv, src);
                    return;
                }
            }

            if (len < common::IKCP_OVERHEAD) return;
            uint32_t conv = common::decode32u(buf);

            auto it = _connections.find(conv);
            if (it == _connections.end()) return;

            auto& conn = it->second;
            conn.kcp.input(buf, static_cast<int>(len));
            deliver_received(conn);
        }

        void
        deliver_received(kcp_connection& conn) {
            while (true) {
                int peeklen = conn.kcp.peeksize();
                if (peeklen <= 0) break;

                auto* buf = new char[peeklen];
                int ret = conn.kcp.recv(buf, peeklen);
                if (ret <= 0) {
                    delete[] buf;
                    break;
                }

                pump::scheduler::net::net_frame frame(buf, static_cast<uint32_t>(ret));
                conn.session->invoke(::pump::scheduler::net::on_frame, std::move(frame));
            }
        }

        void
        retry_connects(uint32_t now_ms) {
            auto it = _pending_connects.begin();
            while (it != _pending_connects.end()) {
                if (common::itimediff(now_ms, it->next_retry_ts) >= 0) {
                    if (it->retries >= 10) {
                        it->req->cb(std::make_exception_ptr(
                            std::runtime_error("kcp: connect timeout")));
                        delete it->req;
                        it = _pending_connects.erase(it);
                        continue;
                    }
                    send_handshake(common::HANDSHAKE_SYN, it->conv, it->target);
                    it->retries++;
                    it->next_retry_ts = now_ms + 500;
                }
                ++it;
            }
        }

        void
        update_all_kcp(uint32_t now_ms) {
            for (auto& [conv, conn] : _connections) {
                conn.kcp.update(now_ms);
            }
        }

    public:
        using address_type = session_t*;

        static uint64_t
        address_raw(address_type session) {
            return reinterpret_cast<uint64_t>(session);
        }

        void
        schedule_send(common::send_req* req) {
            send_q.try_enqueue(req);
        }

        session_t*
        get_session(common::conv_id_t conv) {
            auto it = _connections.find(conv.value);
            if (it != _connections.end()) return it->second.session;
            return nullptr;
        }

        scheduler() = default;

        int
        init(const dgram::dpdk::dpdk_config& cfg) {
            return _transport.init(cfg);
        }

        void add_arp(uint32_t ip_net_order, const rte_ether_addr& mac) {
            _transport.add_arp(ip_net_order, mac);
        }

        void add_arp(const char* ip_str, const uint8_t mac[6]) {
            _transport.add_arp(ip_str, mac);
        }

        ~scheduler() {
            for (auto* req : _pending_accepts) delete req;
            for (auto& pc : _pending_connects) delete pc.req;
            for (auto& [conv, conn] : _connections) {
                if (conn.session) {
                    conn.session->broadcast(::pump::scheduler::net::on_error,
                        std::make_exception_ptr(std::runtime_error("kcp: scheduler destroyed")));
                    delete conn.session;
                }
            }
        }

        bool
        advance(uint32_t now_ms) {
            drain_accept_q();
            drain_connect_q(now_ms);
            drain_send_q();

            _transport.advance(
                [this](const char* buf, uint32_t len, const sockaddr_in& src) {
                    on_raw_packet(buf, len, src);
                }
            );

            retry_connects(now_ms);
            update_all_kcp(now_ms);

            return true;
        }

        template<typename runtime_t>
        auto
        advance(const runtime_t&) {
            return advance(clock_ms());
        }
    };

}  // namespace pump::scheduler::kcp::dpdk

#endif //ENV_SCHEDULER_KCP_DPDK_SCHEDULER_HH
