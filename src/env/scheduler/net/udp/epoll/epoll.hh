
#ifndef ENV_SCHEDULER_UDP_EPOLL_HH
#define ENV_SCHEDULER_UDP_EPOLL_HH

#include <cstring>
#include <unistd.h>
#include <sys/epoll.h>
#include <array>

namespace pump::scheduler::udp::epoll::detail {

    static constexpr int MAX_EVENTS = 16;

    struct
    poller_epoll {
    private:
        int epoll_fd;
    public:
        std::array<epoll_event, MAX_EVENTS> events;
    public:
        poller_epoll()
            : epoll_fd(epoll_create1(EPOLL_CLOEXEC))
            , events{} {}

        ~poller_epoll() {
            if (epoll_fd >= 0)
                ::close(epoll_fd);
        }

        int
        add_event(int fd, epoll_event* e) const {
            return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, e);
        }

        int
        mod_event(int fd, epoll_event* e) const {
            return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, e);
        }

        [[nodiscard]] int
        wait_event(int ms) {
            return epoll_wait(epoll_fd, events.data(), MAX_EVENTS, ms);
        }
    };
}

#endif //ENV_SCHEDULER_UDP_EPOLL_HH
