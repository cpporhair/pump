
#ifndef ENV_SCHEDULER_UDP_COMMON_STRUCT_HH
#define ENV_SCHEDULER_UDP_COMMON_STRUCT_HH

#include <cstdint>
#include <functional>
#include <variant>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "pump/core/meta.hh"

namespace pump::scheduler::udp::common {

    struct
    endpoint {
        sockaddr_in _addr{};

        constexpr endpoint() = default;

        endpoint(const char* ip, uint16_t port) {
            _addr.sin_family = AF_INET;
            _addr.sin_port = htons(port);
            inet_pton(AF_INET, ip, &_addr.sin_addr);
        }

        explicit endpoint(const sockaddr_in& addr) : _addr(addr) {}

        [[nodiscard]] uint16_t port() const { return ntohs(_addr.sin_port); }
        [[nodiscard]] uint32_t ip_raw() const { return _addr.sin_addr.s_addr; }
        [[nodiscard]] const sockaddr_in& raw() const { return _addr; }
        [[nodiscard]] sockaddr_in& raw() { return _addr; }

        bool operator==(const endpoint& o) const {
            return _addr.sin_port == o._addr.sin_port
                && _addr.sin_addr.s_addr == o._addr.sin_addr.s_addr;
        }
    };

    struct
    datagram {
        char* _data;
        uint32_t _len;

        datagram() : _data(nullptr), _len(0) {}
        datagram(char* data, uint32_t len) : _data(data), _len(len) {}

        datagram(datagram&& rhs) noexcept : _data(rhs._data), _len(rhs._len) {
            rhs._data = nullptr;
            rhs._len = 0;
        }

        datagram& operator=(datagram&& rhs) noexcept {
            if (this != &rhs) {
                delete[] _data;
                _data = rhs._data;
                _len = rhs._len;
                rhs._data = nullptr;
                rhs._len = 0;
            }
            return *this;
        }

        datagram(const datagram&) = delete;
        datagram& operator=(const datagram&) = delete;

        ~datagram() { delete[] _data; }

        [[nodiscard]] char* release() noexcept {
            auto* p = _data;
            _data = nullptr;
            _len = 0;
            return p;
        }

        [[nodiscard]] const char* data() const { return _data; }
        [[nodiscard]] uint32_t size() const { return _len; }

        template <typename T>
        [[nodiscard]] const T* as() const { return reinterpret_cast<const T*>(_data); }
    };

    struct
    recv_req {
        std::move_only_function<void(std::variant<std::pair<datagram, endpoint>, std::exception_ptr>)> cb;
    };

    struct
    send_req {
        endpoint target;
        datagram frame;
        std::move_only_function<void(bool)> cb;

        msghdr _msg{};
        iovec _iov{};

        void prepare() {
            _iov = {frame._data, frame._len};
            _msg = {};
            _msg.msg_name = &target.raw();
            _msg.msg_namelen = sizeof(sockaddr_in);
            _msg.msg_iov = &_iov;
            _msg.msg_iovlen = 1;
        }
    };

    struct
    scheduler_config {
        const char* address = "0.0.0.0";
        uint16_t port = 0;
        unsigned queue_depth = 256;
        uint32_t max_datagram_size = 65536;
        uint32_t recv_depth = 32;
    };
}

#endif //ENV_SCHEDULER_UDP_COMMON_STRUCT_HH
