
#ifndef ENV_COMMON_FRAME_HH
#define ENV_COMMON_FRAME_HH

#include <cstdint>

namespace pump::scheduler::net {

    struct
    net_frame {
        char* _data;
        uint32_t _len;

        net_frame() : _data(nullptr), _len(0) {}
        net_frame(char* data, const uint32_t len) : _data(data), _len(len) {}

        net_frame(net_frame&& rhs) noexcept : _data(rhs._data), _len(rhs._len) {
            rhs._data = nullptr;
            rhs._len = 0;
        }

        net_frame& operator=(net_frame&& rhs) noexcept {
            if (this != &rhs) {
                delete[] _data;
                _data = rhs._data;
                _len = rhs._len;
                rhs._data = nullptr;
                rhs._len = 0;
            }
            return *this;
        }

        net_frame(const net_frame&) = delete;
        net_frame& operator=(const net_frame&) = delete;

        ~net_frame() {
            delete[] _data;
        }

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

}

#endif //ENV_COMMON_FRAME_HH
