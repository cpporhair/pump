#ifndef PUMP_CORE_TUPLE_VALUES_HH
#define PUMP_CORE_TUPLE_VALUES_HH

#include <tuple>

namespace pump::core {
    template <typename ...value_t>
    struct
    tuple_values {
        std::tuple<value_t...> values;
        tuple_values(value_t&& ...v) : values(__fwd__(v)...){}
        tuple_values(tuple_values&& rhs) : values(__fwd__(rhs.values)){}
        tuple_values(const tuple_values& rhs) : values(rhs.values){}
    };
}
#endif //PUMP_CORE_TUPLE_VALUES_HH
