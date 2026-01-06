#ifndef PUMP_CORE_HELPER_HH
#define PUMP_CORE_HELPER_HH

#include <tuple>

namespace pump::core {
    inline
    auto
    tuple_to_tie(auto& t) {
        return std::apply([](auto &...v) mutable { return std::tie(v...); }, t);
    }
}

#endif //PUMP_CORE_HELPER_HH
