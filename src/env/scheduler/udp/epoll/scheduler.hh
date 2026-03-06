
#ifndef ENV_SCHEDULER_UDP_EPOLL_SCHEDULER_HH
#define ENV_SCHEDULER_UDP_EPOLL_SCHEDULER_HH

#include <cstdint>
#include <cstring>
#include <list>
#include <netinet/in.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>

#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"

#include "../common/struct.hh"
#include "./epoll.hh"

namespace pump::scheduler::udp::epoll {

    template <
        template<typename> class recv_op_t,
        template<typename> class send_op_t
    >
    struct
    scheduler {
        friend struct recv_op_t<scheduler>;
        friend struct send_op_t<scheduler>;
    private:
        int _fd = -1;
        detail::poller_epoll _poller;
        uint32_t _max_datagram_size = 65536;

        core::per_core::queue<common::recv_req*, 2048> recv_q;
        core::per_core::queue<common::send_req*, 2048> send_q;

        core::local::queue<common::recv_req*> pending_recv;

        // Completed datagrams waiting for recv_req
        struct ready_entry {
            common::datagram dg;
            common::endpoint ep;
        };
        core::local::queue<ready_entry*> ready_q;

        // Sends that got EAGAIN, retry on EPOLLOUT
        std::list<common::send_req*> pending_sends;

        std::atomic<bool> _shutdown{false};

        // Reusable recv buffer
        char* _recv_buf = nullptr;

    private:
        void
        schedule(common::recv_req* req) {
            if (!recv_q.try_enqueue(req)) {
                req->cb(std::make_exception_ptr(std::runtime_error("udp recv queue full")));
                delete req;
            }
        }

        void
        schedule(common::send_req* req) {
            if (!send_q.try_enqueue(req)) {
                req->cb(false);
                delete req;
            }
        }

        void
        deliver_datagram(common::recv_req* req, char* data, uint32_t len, const sockaddr_in& src) {
            auto* copy = new char[len];
            memcpy(copy, data, len);
            req->cb(std::make_pair(
                common::datagram{copy, len},
                common::endpoint{src}
            ));
            delete req;
        }

        void
        drain_recv_q() {
            recv_q.drain([this](common::recv_req* req) {
                if (auto opt = ready_q.try_dequeue()) {
                    auto* entry = opt.value();
                    req->cb(std::make_pair(
                        std::move(entry->dg),
                        std::move(entry->ep)
                    ));
                    delete req;
                    delete entry;
                } else {
                    pending_recv.try_enqueue(req);
                }
            });
        }

        bool
        try_sendmsg(common::send_req* req) {
            req->prepare();
            auto res = ::sendmsg(_fd, &req->_msg, MSG_DONTWAIT);
            if (res >= 0) {
                req->cb(true);
                delete req;
                return true;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return false;
            }
            // Other error
            req->cb(false);
            delete req;
            return true;
        }

        void
        drain_send_q() {
            send_q.drain([this](common::send_req* req) {
                if (!try_sendmsg(req)) {
                    pending_sends.push_back(req);
                }
            });
        }

        void
        flush_pending_sends() {
            auto it = pending_sends.begin();
            while (it != pending_sends.end()) {
                if (try_sendmsg(*it)) {
                    it = pending_sends.erase(it);
                } else {
                    break;  // Still blocked
                }
            }
        }

        // Read datagrams until EAGAIN
        void
        handle_read_event() {
            while (true) {
                sockaddr_in src_addr{};
                iovec iov{_recv_buf, _max_datagram_size};
                msghdr msg{};
                msg.msg_name = &src_addr;
                msg.msg_namelen = sizeof(src_addr);
                msg.msg_iov = &iov;
                msg.msg_iovlen = 1;

                auto res = ::recvmsg(_fd, &msg, MSG_DONTWAIT);
                if (res <= 0) {
                    // EAGAIN or error — stop reading
                    return;
                }

                auto len = static_cast<uint32_t>(res);
                if (auto opt = pending_recv.try_dequeue()) {
                    deliver_datagram(opt.value(), _recv_buf, len, src_addr);
                } else {
                    auto* copy = new char[len];
                    memcpy(copy, _recv_buf, len);
                    ready_q.try_enqueue(new ready_entry{
                        common::datagram{copy, len},
                        common::endpoint{src_addr}
                    });
                }
            }
        }

        void
        handle_events() {
            const int cnt = _poller.wait_event(0);
            for (int i = 0; i < cnt; ++i) {
                if (_poller.events[i].events & EPOLLIN)
                    handle_read_event();
                if (_poller.events[i].events & EPOLLOUT)
                    flush_pending_sends();
            }
        }

        void
        drain_on_shutdown() {
            auto err = std::make_exception_ptr(std::runtime_error("udp scheduler shutdown"));
            while (auto opt = pending_recv.try_dequeue()) {
                opt.value()->cb(err);
                delete opt.value();
            }
            recv_q.drain([&err](common::recv_req* req) {
                req->cb(err);
                delete req;
            });
            send_q.drain([](common::send_req* req) {
                req->cb(false);
                delete req;
            });
            for (auto* req : pending_sends) {
                req->cb(false);
                delete req;
            }
            pending_sends.clear();
            while (auto opt = ready_q.try_dequeue()) {
                delete opt.value();
            }
        }

    public:
        scheduler() = default;

        int
        init(const common::scheduler_config& cfg) {
            return init(cfg.address, cfg.port, cfg.max_datagram_size);
        }

        int
        init(const char* address, uint16_t port,
             uint32_t max_datagram_size = 65536) {
            _fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (_fd < 0)
                return -1;

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

            _max_datagram_size = max_datagram_size;
            _recv_buf = new char[max_datagram_size];

            epoll_event ev{};
            ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
            ev.data.fd = _fd;
            _poller.add_event(_fd, &ev);

            return 0;
        }

        ~scheduler() {
            delete[] _recv_buf;
            while (auto opt = ready_q.try_dequeue())
                delete opt.value();
            if (_fd >= 0)
                ::close(_fd);
        }

        void
        shutdown() {
            _shutdown.store(true, std::memory_order_release);
        }

        auto
        advance() {
            if (_shutdown.load(std::memory_order_acquire)) [[unlikely]] {
                drain_on_shutdown();
                return false;
            }
            drain_recv_q();
            drain_send_q();
            handle_events();
            return true;
        }

        template<typename runtime_t>
        auto
        advance(const runtime_t&) {
            return advance();
        }
    };
}

#endif //ENV_SCHEDULER_UDP_EPOLL_SCHEDULER_HH
