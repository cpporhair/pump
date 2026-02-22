//
// Created by null on 2025/7/21.
//

#ifndef ENV_SCHEDULER_NET_EPOLL_HH
#define ENV_SCHEDULER_NET_EPOLL_HH
#include <cstring>
#include <unistd.h>
#include <sys/epoll.h>
#include <array>

namespace pump::scheduler::net::epoll::detail {

    static constexpr int MAX_EVENTS = 16;

    struct
    poller_epoll {
    private:
        int epoll_fd;
        epoll_event ev;
    public:
        // 3.9: use std::array to avoid heap allocation and manual delete
        std::array<epoll_event, MAX_EVENTS> events;
    public:
        poller_epoll()
            : epoll_fd(epoll_create1(EPOLL_CLOEXEC))
            , ev()
            , events{} {
        }

        explicit
        poller_epoll(int fd)
            : epoll_fd(fd)
            , ev()
            , events{} {
        }

        ~poller_epoll(){
            ::close(epoll_fd);
        }

        int
        add_event(int fd, epoll_event* e) const {
            return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, e);
        }

        int
        del_event(int fd, epoll_event* e) const {
            return epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, e);
        }

        int
        mod_event(int fd, epoll_event* e) const {
            return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, e);
        }

        int
        wait_event(epoll_event *es, const int size, const int ms) const {
            return epoll_wait(epoll_fd, es, size, ms);
        }

        [[nodiscard]]
        int
        wait_event(const int ms) {
            return epoll_wait(epoll_fd, events.data(), MAX_EVENTS, ms);
        }
    };
}

#endif //ENV_SCHEDULER_NET_EPOLL_HH
