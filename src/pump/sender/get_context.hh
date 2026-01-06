#ifndef PUMP_SENDER_GET_CONTEXT_HH
#define PUMP_SENDER_GET_CONTEXT_HH

#include <type_traits>
#include "../core/bind_back.hh"
#include "../core/op_pusher.hh"
#include "../core/compute_sender_type.hh"

namespace pump::sender {
    namespace _get_context {
        template <typename ...env_t>
        struct
        op {
            constexpr static bool get_context_op = true;

            constexpr static bool is_get_all = (sizeof...(env_t) == 0);

            template <typename context_t>
            inline
            decltype(auto)
            get_from_context(context_t& context) {
                return get_all_from_context<context_t, env_t...>(context);
            }
        };

        template <typename prev_t,typename ...env_t>
        struct
        __ncp__(sender) {
            using prev_type = prev_t;
            prev_t prev;

            explicit
            sender(prev_t&& s)
                :prev(__fwd__(s)){
            }

            sender(sender&& o)noexcept
                :prev(__fwd__(o.prev)){
            }

            inline
            auto
            make_op() {
                return op<env_t...>{};
            }

            template<typename context_t>
            auto
            connect(){
                return prev.template connect<context_t>().push_back(make_op());
            }
        };

        template <typename ...env_t>
        struct
        fn {
            template <typename sender_t>
            constexpr
            decltype(auto)
            operator ()(sender_t&& sender) const {
                return _get_context::sender<sender_t,env_t...>{ __fwd__(sender) };
            }

            decltype(auto)
            operator ()() const {
                return core::bind_back<fn<env_t...>>(fn<env_t...>{});
            }
        };
    }

    template <typename ...env_t>
    inline constexpr _get_context::fn<env_t...> get_context{};

    inline constexpr _get_context::fn<> get_full_context_object{};
}

namespace pump::core {
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
    && (get_current_op_type_t<pos, scope_t>::get_context_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename context_t, typename ...value_t>
        requires get_current_op_type_t<pos, scope_t>::is_get_all
        static inline
        void
        push_value(context_t& context, scope_t& scope, value_t&& ...v) {
            op_pusher<pos + 1, scope_t>::push_value(context, scope, context, __fwd__(v)...);
        }

        template<typename context_t, typename ...value_t>
        static inline
        void
        push_value(context_t& context, scope_t& scope, value_t&& ...v) {
            auto &op = std::get<pos>(scope->get_op_tuple());
            return std::apply(
            [...v = __fwd__(v), &context, &scope](auto&& ...args) mutable {
                op_pusher<pos + 1, scope_t>::push_value(context, scope, __fwd__(args)..., __fwd__(v)...);
            },
            op.get_from_context(context)
            );
        }
    };

    template<typename context_t, typename sender_t, typename ...env_t>
    struct
    compute_sender_type<context_t, sender::_get_context::sender<sender_t, env_t ...>> {
        using sender_value_class = compute_sender_type<context_t, sender_t>;

        consteval static uint32_t
        count_prev_value() {
            return sender_value_class::count_value();
        }

        consteval static uint32_t
        count_this_value() {
            return sizeof...(env_t);
        }

        consteval static uint32_t
        count_value() {
            return count_prev_value() + count_this_value();
        }

        consteval static auto
        get_env_type()
            -> std::conditional_t<count_this_value() == 1,
                std::tuple_element_t<0, std::tuple<env_t...> > &,
                std::tuple<env_t &...>
            > {
            return {};
        }

        consteval static auto
        get_value_type_identity() {
            static_assert(count_this_value() != 0, "no value_type");
            using t0 = decltype(get_env_type());

            if constexpr (count_prev_value() == 0)
                return std::type_identity<t0>{};

            if constexpr (core::is_tuple<t0>::value)
                return std::type_identity<
                    decltype(
                        std::tuple_cat<
                            t0,
                            typename decltype(sender_value_class::get_value_type_identity())::type
                        >
                    )
                >{};
            else
                return std::type_identity<
                    std::tuple<
                        t0,
                        typename decltype(sender_value_class::get_value_type_identity())::type
                    >
                >{};

        }
    };

}
#endif //PUMP_SENDER_GET_CONTEXT_HH
