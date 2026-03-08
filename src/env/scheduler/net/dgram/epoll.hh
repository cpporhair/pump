
#ifndef ENV_SCHEDULER_DGRAM_EPOLL_HH
#define ENV_SCHEDULER_DGRAM_EPOLL_HH

#include <cstdint>
#include <cstring>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "./common.hh"

namespace pump::scheduler::dgram::epoll {

    static constexpr int MAX_EVENTS = 16;

    struct
    transport {
    private:
        int _fd = -1;
        int _epoll_fd = -1;
        char* _recv_buf = nullptr;
        uint32_t _max_dgram_size = 65536;
        epoll_event _events[MAX_EVENTS]{};

    public:
        transport() = default;

        transport(const transport&) = delete;
        transport& operator=(const transport&) = delete;

        int
        init(const char* address, uint16_t port, const transport_config& cfg = {}) {
            _fd = create_bound_udp_socket(address, port);
            if (_fd < 0)
                return -1;

            _max_dgram_size = cfg.max_dgram_size;
            _recv_buf = new char[cfg.max_dgram_size];

            _epoll_fd = epoll_create1(EPOLL_CLOEXEC);
            if (_epoll_fd < 0) {
                delete[] _recv_buf;
                _recv_buf = nullptr;
                ::close(_fd);
                _fd = -1;
                return -1;
            }

            epoll_event ev{};
            ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
            ev.data.fd = _fd;
            epoll_ctl(_epoll_fd, EPOLL_CTL_ADD, _fd, &ev);

            return 0;
        }

        ~transport() {
            delete[] _recv_buf;
            if (_epoll_fd >= 0) ::close(_epoll_fd);
            if (_fd >= 0) ::close(_fd);
        }

        [[nodiscard]] int fd() const { return _fd; }

        // Sync sendto — fire-and-forget, used by KCP's ikcp output callback
        void
        sendto(const char* data, uint32_t len, const sockaddr_in& dest) {
            ::sendto(_fd, data, len, MSG_DONTWAIT,
                     reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
        }

        // Try sendmsg — returns bytes sent on success, -1 on error (check errno).
        ssize_t
        try_sendmsg(msghdr* msg) {
            return ::sendmsg(_fd, msg, MSG_DONTWAIT);
        }

        // Process epoll events.
        // recv_handler: void(const char* buf, uint32_t len, const sockaddr_in& src)
        //   buf is valid only during the callback.
        // Returns true if EPOLLOUT was seen (socket writable).
        template<typename RecvHandler>
        bool
        advance(RecvHandler&& recv_handler) {
            bool writable = false;

            int cnt = epoll_wait(_epoll_fd, _events, MAX_EVENTS, 0);
            for (int i = 0; i < cnt; ++i) {
                if (_events[i].events & EPOLLIN) {
                    // Read datagrams until EAGAIN
                    while (true) {
                        sockaddr_in src_addr{};
                        socklen_t addrlen = sizeof(src_addr);
                        auto res = ::recvfrom(_fd, _recv_buf, _max_dgram_size, MSG_DONTWAIT,
                                              reinterpret_cast<sockaddr*>(&src_addr), &addrlen);
                        if (res <= 0)
                            break;
                        recv_handler(_recv_buf, static_cast<uint32_t>(res), src_addr);
                    }
                }
                if (_events[i].events & EPOLLOUT) {
                    writable = true;
                }
            }

            return writable;
        }
    };

}  // namespace pump::scheduler::dgram::epoll

#endif //ENV_SCHEDULER_DGRAM_EPOLL_HH
