
#ifndef ENV_SCHEDULER_DGRAM_COMMON_HH
#define ENV_SCHEDULER_DGRAM_COMMON_HH

#include <cstdint>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>

namespace pump::scheduler::dgram {

    struct
    transport_config {
        unsigned queue_depth = 256;         // io_uring ring size
        uint32_t max_dgram_size = 65536;    // max recv datagram size
        uint32_t recv_depth = 32;           // io_uring recv slot count
    };

    // Create a non-blocking UDP socket bound to address:port.
    // Returns fd on success, -1 on failure.
    inline int
    create_bound_udp_socket(const char* address, uint16_t port) {
        int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
            return -1;

        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, address, &addr.sin_addr);

        if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd);
            return -1;
        }

        return fd;
    }

}  // namespace pump::scheduler::dgram

#endif //ENV_SCHEDULER_DGRAM_COMMON_HH
