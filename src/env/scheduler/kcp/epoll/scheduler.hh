
#ifndef ENV_SCHEDULER_KCP_EPOLL_SCHEDULER_HH
#define ENV_SCHEDULER_KCP_EPOLL_SCHEDULER_HH

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <list>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>

#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"

#include "../common/struct.hh"
#include "../common/ikcp.hh"
#include "../kcp.hh"

namespace pump::scheduler::kcp::epoll {

    namespace detail {
        static constexpr int MAX_EVENTS = 16;
        static constexpr uint32_t MAX_UDP_SIZE = 65536;
    }

    // Per-connection state
    struct kcp_connection {
        common::ikcp kcp;
        sockaddr_in peer_addr{};
        bool connected = false;

        std::list<common::recv_req*> pending_recv;
        std::list<pump::common::net_frame> ready_frames;
    };

    template <
        template<typename> class recv_op_t,
        template<typename> class send_op_t,
        template<typename> class accept_op_t,
        template<typename> class connect_op_t
    >
    struct
    scheduler {
        friend struct recv_op_t<scheduler>;
        friend struct send_op_t<scheduler>;
        friend struct accept_op_t<scheduler>;
        friend struct connect_op_t<scheduler>;

    private:
        int _fd = -1;
        int _epoll_fd = -1;
        epoll_event _events[detail::MAX_EVENTS]{};

        core::per_core::queue<common::recv_req*, 2048>    recv_q;
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
        char* _recv_buf = nullptr;

    private:
        void
        schedule(common::recv_req* req) {
            recv_q.try_enqueue(req);
        }

        void
        schedule(common::send_req* req) {
            send_q.try_enqueue(req);
        }

        void
        schedule(common::accept_req* req) {
            accept_q.try_enqueue(req);
        }

        void
        schedule(common::connect_req* req) {
            connect_q.try_enqueue(req);
        }

        void
        udp_sendto(const char* data, uint32_t len, const sockaddr_in& addr) {
            ::sendto(_fd, data, len, MSG_DONTWAIT,
                     reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        }

        void
        send_handshake(uint8_t type, uint32_t conv, const sockaddr_in& addr) {
            common::handshake_pkt pkt{};
            pkt.type = type;
            pkt.conv = conv;
            udp_sendto(reinterpret_cast<const char*>(&pkt), common::HANDSHAKE_PKT_SIZE, addr);
        }

        kcp_connection&
        create_connection(uint32_t conv, const sockaddr_in& peer) {
            auto& conn = _connections[conv];
            conn.peer_addr = peer;
            conn.connected = true;

            conn.kcp = common::ikcp(conv, [this, conv](const char* buf, uint32_t len) {
                auto it = _connections.find(conv);
                if (it != _connections.end()) {
                    udp_sendto(buf, len, it->second.peer_addr);
                }
            });

            conn.kcp.set_nodelay(1, 10, 2, 1);
            conn.kcp.set_wndsize(128, 128);

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
        drain_recv_q() {
            recv_q.drain([this](common::recv_req* req) {
                auto it = _connections.find(req->conv.value);
                if (it == _connections.end()) {
                    req->cb(std::make_exception_ptr(
                        std::runtime_error("kcp: unknown conv_id")));
                    delete req;
                    return;
                }

                auto& conn = it->second;
                if (!conn.ready_frames.empty()) {
                    auto frame = std::move(conn.ready_frames.front());
                    conn.ready_frames.pop_front();
                    req->cb(std::move(frame));
                    delete req;
                } else {
                    conn.pending_recv.push_back(req);
                }
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
        handle_read_event(uint32_t now_ms) {
            while (true) {
                sockaddr_in src_addr{};
                socklen_t addrlen = sizeof(src_addr);
                auto res = ::recvfrom(_fd, _recv_buf, detail::MAX_UDP_SIZE, MSG_DONTWAIT,
                                      reinterpret_cast<sockaddr*>(&src_addr), &addrlen);
                if (res <= 0) return;

                auto len = static_cast<uint32_t>(res);

                if (len == common::HANDSHAKE_PKT_SIZE) {
                    auto* pkt = reinterpret_cast<const common::handshake_pkt*>(_recv_buf);
                    if (pkt->type == common::HANDSHAKE_SYN) {
                        handle_syn(pkt->conv, src_addr);
                        continue;
                    } else if (pkt->type == common::HANDSHAKE_ACK) {
                        handle_ack(pkt->conv, src_addr);
                        continue;
                    }
                }

                if (len < common::IKCP_OVERHEAD) continue;
                uint32_t conv = common::decode32u(_recv_buf);

                auto it = _connections.find(conv);
                if (it == _connections.end()) continue;

                auto& conn = it->second;
                conn.kcp.input(_recv_buf, static_cast<int>(len));

                deliver_received(conn, now_ms);
            }
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
        deliver_received(kcp_connection& conn, uint32_t /*now_ms*/) {
            while (true) {
                int peeklen = conn.kcp.peeksize();
                if (peeklen <= 0) break;

                auto* buf = new char[peeklen];
                int ret = conn.kcp.recv(buf, peeklen);
                if (ret <= 0) {
                    delete[] buf;
                    break;
                }

                pump::common::net_frame frame(buf, static_cast<uint32_t>(ret));

                if (!conn.pending_recv.empty()) {
                    auto* req = conn.pending_recv.front();
                    conn.pending_recv.pop_front();
                    req->cb(std::move(frame));
                    delete req;
                } else {
                    conn.ready_frames.push_back(std::move(frame));
                }
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
        scheduler() = default;

        int
        init(const char* address, uint16_t port) {
            _fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (_fd < 0) return -1;

            int opt = 1;
            setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            inet_pton(AF_INET, address, &addr.sin_addr);

            if (bind(_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
                ::close(_fd);
                _fd = -1;
                return -1;
            }

            _recv_buf = new char[detail::MAX_UDP_SIZE];

            _epoll_fd = epoll_create1(EPOLL_CLOEXEC);
            if (_epoll_fd < 0) {
                delete[] _recv_buf;
                ::close(_fd);
                _fd = -1;
                return -1;
            }

            epoll_event ev{};
            ev.events = EPOLLIN | EPOLLET;
            ev.data.fd = _fd;
            epoll_ctl(_epoll_fd, EPOLL_CTL_ADD, _fd, &ev);

            return 0;
        }

        ~scheduler() {
            delete[] _recv_buf;
            if (_epoll_fd >= 0) ::close(_epoll_fd);
            if (_fd >= 0) ::close(_fd);

            for (auto* req : _pending_accepts) delete req;
            for (auto& pc : _pending_connects) delete pc.req;
            for (auto& [conv, conn] : _connections) {
                for (auto* req : conn.pending_recv) {
                    req->cb(std::make_exception_ptr(
                        std::runtime_error("kcp: scheduler destroyed")));
                    delete req;
                }
            }
        }

        bool
        advance(uint32_t now_ms) {
            drain_accept_q();
            drain_connect_q(now_ms);
            drain_recv_q();
            drain_send_q();

            int cnt = epoll_wait(_epoll_fd, _events, detail::MAX_EVENTS, 0);
            for (int i = 0; i < cnt; ++i) {
                if (_events[i].events & EPOLLIN) {
                    handle_read_event(now_ms);
                }
            }

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

}  // namespace pump::scheduler::kcp::epoll

#endif //ENV_SCHEDULER_KCP_EPOLL_SCHEDULER_HH
