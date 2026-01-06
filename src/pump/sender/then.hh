#ifndef PUMP_SENDER_THEN_HH
#define PUMP_SENDER_THEN_HH

#include <utility>
#include <exception>
#include <tuple>

#include "../core/meta.hh"

#include "../core//bind_back.hh"
#include "../core//tuple_values.hh"
#include "../core/compute_sender_type.hh"
#include "../core/op_pusher.hh"

#include "./just.hh"


namespace pump::sender {
    namespace _then{
        template <typename func_t>
        struct
        op {
            constexpr static bool then_op = true;
            func_t func;

            template<typename ...values_t>
            constexpr static bool is_void = std::is_void_v<std::invoke_result<func_t,values_t&&...>>;

            op(func_t&& f)
                : func(__fwd__(f)){
            }

            op(op&& o) noexcept
                : func(__fwd__(o.func)){
            }

            op(const op& o) noexcept
                : func(o.func){
            }
        };

        template <typename prev_t,typename func_t>
        struct
        __ncp__(sender) {
            using prev_type = prev_t;
            prev_t prev;
            func_t func;

            sender(prev_t&& s,func_t&& f)
                :prev(__fwd__(s))
                ,func(__fwd__(f)){
            }

            sender(sender&& o)noexcept
                :prev(__fwd__(o.prev))
                ,func(__fwd__(o.func)){
            }

            inline
            auto
            make_op() {
                return op<func_t>(__mov__(func));
            }

            template<typename context_t>
            auto
            connect() {
                return prev.template connect<context_t>().push_back(make_op());
            }
        };


        struct
        fn {
            template <typename sender_t,typename func_t>
            constexpr
            decltype(auto)
            operator ()(sender_t&& sender,func_t&& func) const {
                return _then::sender<sender_t, func_t>{
                    __fwd__(sender),
                    __fwd__(func)
                };
            }

            template <typename func_t>
            constexpr
            decltype(auto)
            operator ()(func_t&& func) const requires std::is_rvalue_reference_v<decltype(func)> {
                return core::bind_back<fn, func_t>(fn{}, __fwd__(func));
            }

            template <typename func_t>
            constexpr
            decltype(auto)
            operator ()(func_t&& func) const {
                return core::bind_back<fn, func_t>(fn{}, __fwd__(func));
            }
        };

        struct
        f0 {
            template <typename sender_t>
            constexpr
            decltype(auto)
            operator ()(sender_t&& sender) const {
                return __fwd__(sender);
            }

            template<typename nothing_t = nullptr_t>
            constexpr
            decltype(auto)
            operator ()() const {
                return core::bind_back<f0>(f0{});
            }
        };
    }

    inline constexpr _then::fn then{};

    /* In order for the brackets at the end of then to be aligned in the auto-typesetting */
    inline constexpr _then::f0 un_compiled{};

    static
    auto
    ignore_args() {
        return then([](...) mutable {});
    }

    inline
    auto
    ignore_results() {
        return ignore_args();
    }

    template <size_t size>
    inline
    auto
    assert_args_count() {
        return then([](auto&& ...args) {
            static_assert(size == sizeof...(args), "assert_no_args");
        });
    }

    inline
    auto
    assert_no_args() {
        return assert_args_count<0>();
    }

    template <typename ...value_t>
    auto
    forward_value(value_t&& ...v) {
        return then([...v = __fwd__(v)](...) mutable { __forward_values__(v); });
    }

    template <typename exception_t>
    static
    auto
    just_exception(exception_t&& e) {
        return just() >> then([e = __fwd__(e)] mutable { throw __fwd__(e); });
    }

    template <typename exception_t>
    static
    auto
    then_exception(exception_t&& e) {
        return then([e = __fwd__(e)] (...) mutable { throw __fwd__(e); });
    }

    template <typename func_t>
    inline
    auto
    transform(func_t&& f) {
        return then([f = __fwd__(f)](auto &&...a) mutable { return f(__fwd__(a)...); });
    }

    inline
    auto
    false_to_exception(auto&& exp) {
        return then([exp = __fwd__(exp)](bool b) mutable { if (!b)[[unlikely]] throw __fwd__(exp); return true; });
    }
}

namespace pump::core {
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
    && (get_current_op_type_t<pos, scope_t>::then_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t>  {
        template <typename context_t, typename ...value_t>
        static inline
        void
        push_value(context_t& context, scope_t& scope, tuple_values<value_t...>&& values){
            std::apply(
                [&context, &scope](auto&& ...args) mutable {
                    op_pusher::push_value(context, scope, __fwd__(args)...);
                },
                __fwd__(values.values)
            );
        }

        template <typename context_t, typename ...value_t>
        static inline
        void
        push_value(context_t& context, scope_t& scope, value_t&& ...v)
        requires std::is_void_v<__typ__(std::get<pos>(scope->get_op_tuple()).func(__fwd__(v)...))>{
            try {
                auto b = scope.use_count();
                auto &op = std::get<pos>(scope->get_op_tuple());
                op.func(__fwd__(v)...);
                op_pusher<pos + 1, scope_t>::push_value(context, scope);
            }
            catch (...) {
                op_pusher<pos + 1, scope_t>::push_exception(context, scope, std::current_exception());
            }
        }

        template <typename context_t, typename ...value_t>
        static inline
        void
        push_value(context_t& context, scope_t& scope, value_t&& ...v)
        requires (!std::is_void_v<__typ__(std::get<pos>(scope->get_op_tuple()).func(__fwd__(v)...))>){
            try {
                auto& op = std::get<pos>(scope->get_op_tuple());
                op_pusher<pos + 1, scope_t>::push_value(context, scope, op.func(__fwd__(v)...));
            }
            catch (...) {
                op_pusher<pos + 1, scope_t>::push_exception(context, scope, std::current_exception());
            }
        }
    };

    template<typename context_t, typename sender_t, typename func_t>
    struct
    compute_sender_type<context_t, sender::_then::sender<sender_t, func_t>> {
        using sender_value_class = compute_sender_type<context_t, sender_t>;

        consteval static uint32_t
        count_prev_value() {
            return sender_value_class::count_value();
        }

        consteval static auto
        get_func_value_types() {
            if constexpr (count_prev_value() == 0) {
                return std::type_identity<std::invoke_result_t<func_t>>{};
            }
            else if constexpr (count_prev_value() == 1) {
                using arg_type = decltype(sender_value_class::get_value_type_identity())::type;
                return std::type_identity<std::invoke_result_t<func_t, arg_type>>{};
            }
            else {
                using arg_type = decltype(sender_value_class::get_value_type_identity())::type;
                using t0 = std::invoke_result_t<apply, func_t, arg_type>;
                return std::type_identity<t0>{};
            }
        }

        consteval static uint32_t
        count_value() {
            if constexpr (std::is_void_v<decltype(get_func_value_types())>)
                return 0;
            else
                return 1;
        }

        consteval static auto
        get_value_type_identity() {
            return get_func_value_types();
        }
    };
}

#endif //PUMP_SENDER_THEN_HH