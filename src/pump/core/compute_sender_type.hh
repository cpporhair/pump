//
//
//

#ifndef PUMP_CORE_COMPUTE_SENDER_TYPE_HH
#define PUMP_CORE_COMPUTE_SENDER_TYPE_HH

#include "./meta.hh"

namespace pump::core {
    template <typename context_t, typename sender_t>
    struct
    compute_context_type {
        using type = compute_context_type<context_t, typename sender_t::prev_type>::type;
    };

    template <typename C, typename T>
    struct
    compute_sender_type {
    };

    template <typename ...T>
    struct
    tuple_push_front{};

    template <typename A0, typename ...A1>
    struct
    tuple_push_front<A0,std::tuple<A1...>> {
        using type = std::tuple<A0,A1...>;
    };

    struct
    apply {
        template <typename F, typename T>
        constexpr
        decltype(auto)
        operator()(F&& f,T&& t){
            return std::apply(
                [f = __fwd__(f)](auto&& ...args) mutable {
                    return f(__fwd__(args)...);
                },
                __mov__(t)
            );
        }
    };
}

#endif //PUMP_CORE_COMPUTE_SENDER_TYPE_HH
