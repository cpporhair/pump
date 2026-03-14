#ifndef PUMP_SENDER_JUST_HH
#define PUMP_SENDER_JUST_HH

#include "../core/op_pusher.hh"
#include "../core/compute_sender_type.hh"
#include "../core/tuple_values.hh"
#include "../core/op_tuple_builder.hh"

namespace pump::sender {
    namespace _just {
        template <typename ...value_t>
        struct
        op {

            constexpr static bool just_op = true;
            constexpr static bool empty = (sizeof...(value_t) == 0);
            constexpr static bool is_exception() {
                if constexpr (sizeof...(value_t) != 1)
                    return false;
                else
                    return std::is_same_v<std::exception_ptr, std::tuple_element_t<0, std::tuple<value_t...>>>;
            }

            ::pump::core::tuple_values<value_t...> values;

            explicit
            op(::pump::core::tuple_values<value_t...>&& v)
                : values(__fwd__(v)){
            }

            op(op&& o)noexcept
                : values(__fwd__(o.values)){
            }

            op(const op&) = delete;

            void reset() {}
        };

        template <typename ...value_t>
        struct
        __ncp__(sender) {
            constexpr static bool start_sender = true;
            ::pump::core::tuple_values<value_t...> values;
            explicit
            sender(value_t&& ...v)
                :values(__fwd__(v)...){
            }

            sender(sender&& o)
                :values(__fwd__(o.values)){
            }

            inline
            auto
            make_op() {
                return op<value_t...>(__mov__(values));
            }

            template<typename context_t>
            auto
            connect(){
                return ::pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };

        struct
        fn {
            template <typename ...arg_t>
            constexpr
            decltype(auto)
            operator ()(arg_t&& ...arg) const {
                __all_must_rval__(arg);
                return _just::sender<__typ__(arg)...>{
                    __fwd__(arg)...
                };
            }

            template <typename arg_t>
            constexpr
            decltype(auto)
            operator ()(arg_t&& arg) const {
                return _just::sender<__typ__(arg)>{
                    __fwd__(arg)
                };
            }

            template <typename arg_t>
            constexpr
            decltype(auto)
            operator ()(arg_t* arg) const {
                return _just::sender<arg_t*>{
                    __fwd__(arg)
                };
            }

            decltype(auto)
            operator ()(void) const {
                return _just::sender<>{
                };
            }
        };
    }

    inline constexpr _just::fn just{};
}

namespace pump::core {
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
    && (get_current_op_type_t<pos, scope_t>::just_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {

        template<typename context_t>
        static inline
        void
        push_value(context_t& context, scope_t& scope)
        requires (get_current_op_type_t<pos, scope_t>::empty) {
            op_pusher<pos + 1, scope_t>::push_value(context, scope);
        }

        template<typename context_t>
        static inline
        void
        push_value(context_t& context, scope_t& scope)
        requires (get_current_op_type_t<pos, scope_t>::is_exception()) {
            std::apply(
                [&context, &scope](auto&& ...args) mutable {
                    op_pusher<pos + 1, scope_t>::push_exception(context, scope , __fwd__(args)...);
                },
                std::get<pos>(scope->get_op_tuple()).values.values
            );
        }

        template<typename context_t>
        static inline
        void
        push_value(context_t& context, scope_t& scope) {
            auto& a = std::get<pos>(scope->get_op_tuple());
            auto& b = a.values;
            auto& c = a.values.values;
            std::apply(
                [&context, &scope](auto&& ...args) mutable {
                    op_pusher<pos + 1, scope_t>::push_value(context, scope , __fwd__(args)...);
                },
                std::get<pos>(scope->get_op_tuple()).values.values
            );
        }
    };
}

namespace pump::core {
    template <typename context_t, typename value_t>
    struct
    compute_context_type<context_t, ::pump::sender::_just::sender<value_t>> {
        using type = context_t;
    };

    template <typename context_t>
    struct
    compute_context_type<context_t, ::pump::sender::_just::sender<>> {
        using type = context_t;
    };

    template <typename context_t, typename ...value_t>
    struct
    compute_sender_type<context_t, sender::_just::sender<value_t...>> {

        consteval static uint32_t
        count_value() {
            return sizeof...(value_t);
        }

        consteval static auto
        get_value_type_identity() {
            static_assert(count_value() != 0, "no value_type");
            if constexpr (count_value() == 1)
                return std::type_identity<core::first_elm_of<value_t...>>{};
            else
                return std::type_identity<std::tuple<value_t...>>{};
        }
    };
}

#endif //PUMP_SENDER_JUST_HH
