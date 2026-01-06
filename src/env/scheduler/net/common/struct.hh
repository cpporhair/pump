
#ifndef ENV_SCHEDULER_NET_COMMON_STRUCT_HH
#define ENV_SCHEDULER_NET_COMMON_STRUCT_HH

#include <cstdint>
#include <functional>
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
                delete data;
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

    struct
    packet_buffer {
        char *_data;
        size_t _size;
        volatile size_t _head;
        volatile size_t _tail;

        [[nodiscard]]
        size_t
        available() const {
            return this->_size - (this->_tail - this->_head);
        }

        [[nodiscard]]
        size_t
        size() const {
            return _size;
        }

        [[nodiscard]]
        size_t
        used() const {
            return this->_tail - this->_head;
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
        make_iovec() const {
            auto vec = new iovec[2];
            vec[0].iov_base = this->_data + this->tail();
            vec[0].iov_len = available();
            return vec;
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
            return f(data(), size() - head(), _data, (start + len));
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
            , _head(0)
            , _tail(0) {
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
        int cnt;
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