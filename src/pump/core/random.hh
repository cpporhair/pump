
#ifndef PUMP_CORE_RANDOM_HH
#define PUMP_CORE_RANDOM_HH

#include <cstdint>
#include <chrono>
namespace pump::core {
    static
    auto
    fast_random_uint32(const uint32_t max) {
        thread_local const unsigned int seed = static_cast<unsigned int>(
            std::chrono::steady_clock::now().time_since_epoch().count() & 0xFFFFFFFF);

        return ((1664525 * seed + 1013904223) % (1U << 31)) % max;
    }
}

#endif //PUMP_CORE_RANDOM_HH