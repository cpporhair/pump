
#ifndef PUMP_CORE_CONCURRENT_COPY_HH
#define PUMP_CORE_CONCURRENT_COPY_HH

namespace pump::core {
    template <typename obj_t>
    auto
    concurrent_copy(const obj_t& o) ->__typ__(o) {
        if constexpr (requires { const_cast<obj_t&>(o).concurrent_copy(); } ) {
            return const_cast<obj_t&>(o).concurrent_copy();
        } else if constexpr (is_tuple<obj_t>::value) {
            return std::apply(
                []<typename... T0>(T0& ...args) {
                    return std::tuple<T0...>(concurrent_copy<T0>(args)...);
                },
                const_cast<obj_t&>(o)
            );
        } else {
            return obj_t(o);
        }
    }
}

#endif //PUMP_CORE_CONCURRENT_COPY_HH