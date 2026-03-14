#ifndef PUMP_SENDER_FLAT_HH
#define PUMP_SENDER_FLAT_HH


#include <utility>
#include <exception>

#include "../core/compute_sender_type.hh"
#include "../core/bind_back.hh"
#include "../core/meta.hh"
#include "../core/op_pusher.hh"
#include "./then.hh"

namespace pump::sender {
    namespace _flat{
        struct
        op {
            constexpr static bool flat_op = true;
            void reset() {}
        };

        template <typename prev_t>
        struct
        sender {
            using prev_type = prev_t;

            prev_t prev;

            explicit
            sender(prev_t s)
                :prev(__fwd__(s)) {
            }

            sender(sender&& o)noexcept
                :prev(__fwd__(o.prev)){
            }

            sender(sender const& o) = delete;

            inline
            auto
            make_op() {
                return op{};
            }

            template<typename context_t>
            auto
            connect(){
                return prev.template connect<context_t>().push_back(make_op());
            }
        };

        struct
        fn {
            template <typename sender_t>
            constexpr
            decltype(auto)
            operator ()(sender_t&& sender) const {
                return _flat::sender<sender_t>{
                    __fwd__(sender)
                };
            }

            /*
            template <typename func_t>
            requires std::is_function_v<func_t>
            constexpr
            decltype(auto)
            operator ()(func_t&& sender) const {
                return _flat::sender<func_t>{
                    __fwd__(sender)
                };
            }
            */

            decltype(auto)
            operator ()() const {
                return ::pump::core::bind_back<fn>(fn{});
            }
        };
    }

    inline constexpr _flat::fn flat{};

    template <typename func_t>
    inline
    auto
    flat_map(func_t&& f) {
        return transform(__fwd__(f)) >> flat();
    }
}

namespace pump::core {

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
    && (get_current_op_type_t<pos, scope_t>::flat_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t>{
        template <typename context_t, typename sender_t>
        static inline
        void
        push_value(context_t& context, scope_t& scope, sender_t&& sender) {

            auto new_scope = make_runtime_scope<runtime_scope_type::other>(
                scope,
                sender.template connect<context_t>().push_back(pop_pusher_scope_op<pos + 1>()).take()
            );

            op_pusher<0, __typ__(new_scope)>::push_value(context, new_scope);
        }

        template <typename context_t, typename ...sender_t>
        static inline
        void
        push_value(context_t& context, scope_t& scope, std::variant<sender_t...>&& any) {
            std::visit(
                [&context, &scope](auto&& sender) mutable {
                    op_pusher::push_value(context, scope, __fwd__(sender));
                },
                any
            );
        }
    };

    template <typename context_t, typename sender_t>
    struct
    compute_sender_type<context_t, sender::_flat::sender<sender_t>> {

        consteval static auto
        get_sender_outer_class() {
            return compute_sender_type<context_t, sender_t>{};
        }

        template<typename sender_outer_class_t>
        consteval static auto
        get_sender_inner_class_0() {
            if constexpr (core::is_variant_v<sender_outer_class_t>)
                return get_sender_inner_class_0<std::variant_alternative_t<0, sender_outer_class_t>>();
            else
                return compute_sender_type<context_t, sender_outer_class_t>::get_value_type_identity();
        }

        consteval static auto
        get_sender_inner_class() {
            return compute_sender_type<context_t, typename decltype(get_sender_inner_class_0<sender_t>())::type>{};
        }

        consteval static uint32_t
        count_value() {
            return decltype(get_sender_inner_class())::count_value();
        }

        consteval static auto
        get_value_type_identity() {
            return decltype(get_sender_inner_class())::get_value_type_identity();
        }
    };
}

#endif //PUMP_SENDER_FLAT_HH
