
#ifndef ENV_SCHEDULER_TCP_COMMON_STRUCT_HH
#define ENV_SCHEDULER_TCP_COMMON_STRUCT_HH

#include <cassert>
#include <cstdint>
#include <functional>
#include <algorithm>
#include <string>
#include <atomic>
#include <concepts>
#include <bits/types/struct_iovec.h>

#include "pump/core/meta.hh"
#include "pump/core/lock_free_queue.hh"
#include "env/scheduler/net/common/frame.hh"
#include "env/scheduler/net/common/errors.hh"

namespace pump::scheduler::tcp::common {
    using net_frame = pump::scheduler::net::net_frame;

    // Ring buffer for TCP recv data.
    // Both read and write happen in scheduler's single advance() thread,
    // so no atomics needed.
    struct
    packet_buffer {
        char *_data;
        size_t _size;
        size_t _head = 0;
        size_t _tail = 0;
        iovec _iov[2];

        [[nodiscard]]
        size_t
        available() const {
            return _size - (_tail - _head);
        }

        [[nodiscard]]
        size_t
        size() const {
            return _size;
        }

        [[nodiscard]]
        size_t
        used() const {
            return _tail - _head;
        }

        [[nodiscard]]
        size_t
        head() const {
            return _head & (_size - 1);
        }

        [[nodiscard]]
        size_t
        tail() const {
            return _tail & (_size - 1);
        }

        [[nodiscard]]
        auto
        make_iovec() {
            const size_t t = tail();
            const size_t avail = available();
            const size_t first_len = _size - t;
            if (avail <= first_len) {
                _iov[0].iov_base = this->_data + t;
                _iov[0].iov_len = avail;
                return 1;
            } else {
                _iov[0].iov_base = this->_data + t;
                _iov[0].iov_len = first_len;
                _iov[1].iov_base = this->_data;
                _iov[1].iov_len = avail - first_len;
                return 2;
            }
        }

        [[nodiscard]]
        iovec*
        iov() {
            return _iov;
        }

        [[nodiscard]]
        const char *
        data() const {
            return this->_data + this->head();
        }

        template <typename func_t>
        [[nodiscard]] auto
        handle_data(const size_t start, const size_t len, func_t&& f) const {
            if (used() < len)
                return f();
            if ((start + len) <= size())
                return f(data(), len);
            return f(data(), size() - head(), _data, len - (size() - head()));
        }

        template <typename func_t>
        [[nodiscard]] auto
        handle_data(const size_t len, func_t&& f) const {
            return handle_data(head(),len, __fwd__(f));
        }

        void
        forward_head(const size_t len) {
            _head += len;
        }

        void
        forward_tail(const size_t len) {
            _tail += len;
        }

        explicit
        packet_buffer(const uint32_t _size)
            : _data(new char[_size])
            , _size(_size)
            , _iov{} {
            assert((_size & (_size - 1)) == 0 && "packet_buffer size must be power of 2");
        }

        packet_buffer(packet_buffer&& rhs) noexcept
            : _data(rhs._data)
            , _size(rhs._size)
            , _head(rhs._head)
            , _tail(rhs._tail)
            , _iov{} {
            rhs._data = nullptr;
            rhs._size = 0;
            rhs._head = 0;
            rhs._tail = 0;
        }

        packet_buffer(const packet_buffer&) = delete;
        packet_buffer& operator=(const packet_buffer&) = delete;
        packet_buffer& operator=(packet_buffer&&) = delete;

        ~packet_buffer() {
            delete[] _data;
        }
    };

    // Scheduler configuration
    struct
    scheduler_config {
        const char* address = "0.0.0.0";
        uint16_t port = 8080;
        unsigned queue_depth = 256;
        size_t recv_buffer_size = 4096;
    };

    // accept_scheduler delivers raw fd to upper layer
    struct
    conn_req {
        std::move_only_function<void(int)> cb;
    };

    // connect_scheduler delivers raw fd to upper layer
    struct
    connect_req {
        std::string address;
        uint16_t port;
        std::move_only_function<void(int)> cb;
    };

    // send_req: carries fd + frame, no session reference needed by scheduler
    struct
    send_req {
        int fd;
        net_frame frame;
        std::move_only_function<void(bool)> cb;

        iovec _send_vec[4] = {};
        size_t _send_cnt = 0;
        char _hdr[8] = {};  // stable storage for prepare_send (e.g., length prefix)
    };

    // join_req: upper layer passes session pointer
    template <typename session_t>
    struct
    join_req {
        session_t* session;
        std::move_only_function<void(bool)> cb;
    };

    // stop_req: upper layer passes session pointer
    template <typename session_t>
    struct
    stop_req {
        session_t* session;
        std::move_only_function<void(bool)> cb;
    };

    enum class session_status { normal, closed, errors };
}

#endif //ENV_SCHEDULER_TCP_COMMON_STRUCT_HH
