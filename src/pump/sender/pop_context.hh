#ifndef PUMP_SENDER_POP_CONTEXT_HH
#define PUMP_SENDER_POP_CONTEXT_HH

#include <memory>

#include "../core/bind_back.hh"
#include "../core/op_pusher.hh"
#include "../core/compute_sender_type.hh"
#include "../core/tuple_values.hh"
#include "../core/op_tuple_builder.hh"
#include "./push_context.hh"

namespace pump::sender {
    namespace _pop_context {

        template <uint32_t matching_compile_id>
        struct
        op {
            constexpr static uint32_t context_compile_id = matching_compile_id;
            constexpr static bool pop_context_op = true;
            void reset() {}
        };

        template <uint32_t matching_compile_id, typename prev_t>
        struct
        __ncp__(sender) {

            using prev_type = prev_t;
            const static bool pop_context_sender = true;

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
                return op<matching_compile_id>{};
            }

            template<typename context_t>
            auto
            connect() {
                return prev.template connect<context_t>().push_back(make_op());
            }
        };

        struct
        fn {
            template <typename sender_t>
            constexpr
            decltype(auto)
            operator ()(sender_t&& sender) const {
                return _pop_context::sender
                    <
                        ::pump::core::builder::compute_matching_context_compile_id_v<0, sender_t>,
                        sender_t
                    >{ __fwd__(sender) };
            }

            decltype(auto)
            operator ()() const {
                return ::pump::core::bind_back<fn>(fn{});
            }
        };
    }

    inline constexpr _pop_context::fn pop_context{};

    template <uint32_t pos, typename ...content_t>
    auto
    with_context_with_id(content_t&& ...c){
        return [...c = __fwd__(c)](auto&& bind_t) mutable {
            if constexpr (std::is_invocable_v<__typ__(bind_t)>)
                return push_context_with_id<pos>(__fwd__(c)...) >> bind_t() >> pop_context();
            else
                return push_context_with_id<pos>(__fwd__(c)...) >> __fwd__(bind_t) >> pop_context();
        };
    }

#define with_context with_context_with_id<__COUNTER__>
}

namespace pump::core {
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
    && (get_current_op_type_t<pos, scope_t>::pop_context_op_poped)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t>  {

    };

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
    && (get_current_op_type_t<pos, scope_t>::pop_context_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t>  {

        constexpr static uint64_t context_compile_id = get_current_op_type_t<pos, scope_t>::context_compile_id;

        template<typename context_t, typename ...value_t>
        static inline
        void
        push_value(context_t& context, scope_t& scope, value_t&& ...v) {
            static_assert(context_t::element_type::template has_id<context_compile_id>());
            static_assert(context_t::element_type::pop_able,"context is root");
            auto base = context->base_context;
            auto keep_alive = std::unique_ptr<typename context_t::element_type>(context.release());
            op_pusher<pos + 1, __typ__(scope)>::push_value(base, scope, __fwd__(v)...);

        }

        template<typename context_t>
        static inline
        void
        push_exception(context_t& context, scope_t& scope, std::exception_ptr e) {
            if constexpr (!context_t::element_type::template has_id<context_compile_id>()) {
                op_pusher<pos + 1, __typ__(scope)>::push_exception(context, scope, e);
            }
            else {
                static_assert(context_t::element_type::pop_able,"context is root");
                auto base = context->base_context;
                auto keep_alive = std::unique_ptr<typename context_t::element_type>(context.release());
                op_pusher<pos + 1, __typ__(scope)>::push_exception(base, scope, e);
            }

        }

        template<typename context_t>
        static inline
        void
        push_skip(context_t& context, scope_t& scope) {
            if constexpr (!context_t::element_type::template has_id<context_compile_id>()) {
                op_pusher<pos + 1, __typ__(scope)>::push_skip(context, scope);
            }
            else {
                auto base = context->base_context;
                auto keep_alive = std::unique_ptr<typename context_t::element_type>(context.release());
                op_pusher<pos + 1, scope_t>::push_skip(base, scope);
            }
        }

        template<typename context_t>
        static inline
        void
        push_done(context_t& context, scope_t& scope) {
            if constexpr (!context_t::element_type::template has_id<context_compile_id>()) {
                op_pusher<pos + 1, __typ__(scope)>::push_done(context, scope);
            }
            else {
                static_assert(context_t::element_type::pop_able,"context is root");
                auto base = context->base_context;
                auto keep_alive = std::unique_ptr<typename context_t::element_type>(context.release());
                op_pusher<pos + 1, scope_t>::push_done(base, scope);
            }
        }
    };
}

namespace pump::core {
    template <typename context_t, uint32_t pos, typename sender_t>
    struct
    compute_sender_type<context_t, ::pump::sender::_pop_context::sender<pos, sender_t>> {

        consteval static uint32_t
        count_value() {
            return compute_sender_type<context_t, sender_t>::count_value();
        }

        consteval static auto
        get_value_type_identity() {
            return compute_sender_type<context_t, sender_t>::get_value_type_identity();
        }
    };

    template <typename context_t, uint32_t pos, typename sender_t>
    struct
    compute_context_type<context_t, ::pump::sender::_pop_context::sender<pos, sender_t>> {
        using type = compute_context_type<context_t, sender_t>::type::element_type::base_type;
    };
}
#endif //PUMP_SENDER_POP_CONTEXT_HH
