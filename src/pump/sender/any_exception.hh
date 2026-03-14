
#ifndef PUMP_SENDER_ANY_EXCEPTION_HH
#define PUMP_SENDER_ANY_EXCEPTION_HH

#include "../core/op_pusher.hh"
#include "../core/bind_back.hh"
#include "../core/compute_sender_type.hh"

#include "./flat.hh"
#include "./submit.hh"
#include "pump/core/concurrent_copy.hh"

namespace pump::sender {
    namespace _any_exception {
        template <typename func_t>
        struct
        op {
            constexpr static bool any_exception_op = true;
            func_t func;

            explicit
            op(func_t&& f)
                : func(__fwd__(f)){
            }

            op(op&& rhs)noexcept
                : func(__fwd__(rhs.func)) {
            }

            op(const op& rhs) = delete;

            auto
            concurrent_copy() const {
                return op(core::concurrent_copy(func));
            }

            void reset() {}
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
            connect(){
                return prev.template connect<context_t>().push_back(make_op());
            }
        };

        struct fn {
            template <typename sender_t,typename func_t>
            constexpr
            decltype(auto)
            operator ()(sender_t&& sender,func_t&& func) const {
                return _any_exception::sender<sender_t, func_t>{
                    __fwd__(sender),
                    __fwd__(func)
                };
            }

            template <typename func_t>
            constexpr
            decltype(auto)
            operator ()(func_t&& func) const requires core::must_rvalue<decltype(func)>::v {
                return core::bind_back<fn,func_t>(fn{}, __fwd__(func));
            }
        };
    }



    inline constexpr _any_exception::fn any_exception{};

    template<typename exception_t,typename func_t>
    auto
    catch_exception(func_t&& f) {
        return any_exception([f = __fwd__(f)](std::exception_ptr e) mutable {
            try {
                std::rethrow_exception(e);
            }
            catch (exception_t& exp) {
                return f(__mov__(exp));
            }
        });
    }

    auto
    ignore_all_exception() {
        return any_exception([](std::exception_ptr e) mutable {
            try {
                std::rethrow_exception(e);
            }
            catch (std::exception& e) {
                return just();
            }
        });
    }


    template <typename bind_back_t>
    auto
    ignore_inner_exception(bind_back_t&& bb) {
        return flat_map([bb = __fwd__(bb)](auto &&...args) mutable {
            return just()
                >> forward_value(__fwd__(args)...)
                >> __mov__(bb)
                >> ignore_all_exception();
        });
    }
}

namespace pump::core {
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
    && (get_current_op_type_t<pos, scope_t>::any_exception_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t>{

        template<typename context_t>
        static inline
        void
        push_exception(context_t& context, scope_t& scope, std::exception_ptr e) {
            try {
                decltype(auto) new_sender = std::get<pos>(scope->get_op_tuple()).func(e);
                auto new_scope = make_runtime_scope<runtime_scope_type::other>(
                    scope,
                    new_sender.template connect<context_t>().push_back(pop_pusher_scope_op<pos + 1>()).take()
                );
                op_pusher<0, __typ__(new_scope)>::push_value(context, new_scope);
            }
            catch (std::exception exp) {
                op_pusher<pos + 1, scope_t>::push_exception(context, scope, std::current_exception());
            }
        }
    };

    template <typename context_t, typename sender_t, typename func_t>
    struct
    compute_sender_type<context_t, pump::sender::_any_exception::sender<sender_t, func_t>> {
        consteval static auto
        get_sender_value_class() {
            return compute_sender_type<context_t, sender_t>{};
        }

        consteval static uint32_t
        count_value() {
            return decltype(get_sender_value_class())::count_value();
        }

        consteval static auto
        get_value_type_identity() {
            static_assert(count_value() != 0, "no value_type");
            return std::type_identity<typename decltype(get_sender_value_class())::value_type>{};
        }
    };
}

#endif //PUMP_SENDER_ANY_EXCEPTION_HH
