
#ifndef ENV_SCHEDULER_NET_COMMON_STRUCT_HH
#define ENV_SCHEDULER_NET_COMMON_STRUCT_HH

#include <cstdint>
#include <atomic>
#include <functional>
#include <algorithm>
#include <bits/types/struct_iovec.h>

#include "pump/core/meta.hh"

namespace pump::scheduler::net::common {
    struct
    packet {
        uint16_t len;
        char* data;
        void
        clear() const {
            if (data != nullptr)
                delete[] data;  // 1.1: fix delete -> delete[]
        }
    };

    struct
    pkt_iovec {
        uint08_t cnt;
        iovec* vec;

        [[nodiscard]]
        auto
        len() const {
            size_t len = 0;
            for (uint08_t i = 0; i < cnt; i++) {
                len+= vec[i].iov_len;
            }
            return len;
        }
    };

    // SPSC (Single-Producer Single-Consumer) ring buffer.
    // Producer: network IO write end (updates _tail)
    // Consumer: data processing/read end (updates _head)
    // No multi-producer or multi-consumer synchronization needed.
    struct
    packet_buffer {
        char *_data;
        size_t _size;
        // 1.2: volatile -> atomic with SPSC memory ordering
        std::atomic<size_t> _head;
        std::atomic<size_t> _tail;
        // 1.4: pre-allocated iovec member to avoid heap allocation
        iovec _iov[2];

        [[nodiscard]]
        size_t
        available() const {
            return this->_size - (this->_tail.load(std::memory_order_relaxed) - this->_head.load(std::memory_order_acquire));
        }

        [[nodiscard]]
        size_t
        size() const {
            return _size;
        }

        [[nodiscard]]
        size_t
        used() const {
            return this->_tail.load(std::memory_order_relaxed) - this->_head.load(std::memory_order_acquire);
        }

        [[nodiscard]]
        size_t
        head() const {
            return _head.load(std::memory_order_relaxed) & (_size - 1);
        }

        [[nodiscard]]
        size_t
        tail() const {
            return _tail.load(std::memory_order_relaxed) & (_size - 1);
        }

        // 1.3 + 1.4: handle wrap-around, return iovec count, use member _iov
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
            // 1.6: fix wrap-around branch - fourth param should be wrap portion length
            return f(data(), size() - head(), _data, len - (size() - head()));
        }

        template <typename func_t>
        [[nodiscard]] auto
        handle_data(const size_t len, func_t&& f) const {
            return handle_data(head(),len, __fwd__(f));
        }

        void
        forward_head(const size_t len) {
            // 1.2: use release for consumer updating _head
            _head.store(_head.load(std::memory_order_relaxed) + len, std::memory_order_release);
        }

        void
        forward_tail(const size_t len) {
            // 1.2: use release for producer updating _tail
            _tail.store(_tail.load(std::memory_order_relaxed) + len, std::memory_order_release);
        }

        explicit
        packet_buffer(const uint32_t _size)
            : _data(new char[_size])
            , _size(_size)
            , _head(0)
            , _tail(0)
            , _iov{} {
        }

        // 1.5: add destructor to release _data
        ~packet_buffer() {
            delete[] _data;
        }
    };

    struct
    conn_req {
        std::move_only_function<void(uint64_t)> cb;
    };

    struct
    recv_req {
        uint64_t session_id;
        std::move_only_function<void(std::variant<packet_buffer*, std::exception_ptr>)> cb;
    };

    struct
    send_req {
        uint64_t session_id;
        iovec* vec;
        size_t cnt;  // 2.2: unified to size_t
        std::move_only_function<void(bool)> cb;
    };

    struct
    join_req {
        uint64_t session_id;
        std::move_only_function<void(bool)> cb;
    };

    struct
    stop_req {
        uint64_t session_id;
        std::move_only_function<void(bool)> cb;
    };

}

#endif //ENV_SCHEDULER_NET_COMMON_STRUCT_HH
