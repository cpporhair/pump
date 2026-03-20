#ifndef PUMP_CORE_HELPER_HH
#define PUMP_CORE_HELPER_HH

#include <tuple>

namespace pump::core {
    inline
    auto
    tuple_to_tie(auto& t) {
        return std::apply([](auto &...v) mutable { return std::tie(v...); }, t);
    }

    // Reset all ops in a tuple before reuse (loop iterations).
    // Every op must provide reset() — empty for stateless ops.
    template<std::size_t I = 0>
    inline
    void
    reset_stream_ops(auto& t) {
        if constexpr (I < std::tuple_size_v<std::decay_t<decltype(t)>>) {
            std::get<I>(t).reset();
            reset_stream_ops<I + 1>(t);
        }
    }
}

#endif //PUMP_CORE_HELPER_HH
