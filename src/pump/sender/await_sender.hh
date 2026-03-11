//
//
//

#ifndef PUMP_SENDER_AWAIT_SENDER_HH
#define PUMP_SENDER_AWAIT_SENDER_HH

#include <coroutine>
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

#include "../core/bind_back.hh"
#include "../core/context.hh"
#include "../core/compute_sender_type.hh"
#include "../core/meta.hh"
#include "../core/op_pusher.hh"
#include "../core/scope.hh"

namespace pump::sender {
    namespace detail {
        template <typename value_t>
        struct
        await_state_value {
            using storage_t = std::conditional_t<
                std::is_reference_v<value_t>,
                std::reference_wrapper<std::remove_reference_t<value_t>>,
                value_t
            >;

            std::optional<storage_t> value;
            std::exception_ptr exception{};
            bool skipped = false;
            bool completed = false;
            bool in_await_suspend = false;
            std::coroutine_handle<> continuation{};
            std::shared_ptr<void> context_holder;
            std::unique_ptr<core::scope_holder_base> scope_holder;

            template <typename... value_args_t>
            void
            set_value(value_args_t&& ...v) {
                if constexpr (sizeof...(value_args_t) == 0) {
                    complete();
                    return;
                } else if constexpr (sizeof...(value_args_t) == 1) {
                    store_value(__fwd__(v)...);
                } else {
                    store_value(std::tuple<value_args_t...>(__fwd__(v)...));
                }
                complete();
            }

            void
            set_exception(std::exception_ptr e) {
                exception = e;
                complete();
            }

            void
            set_skip() {
                skipped = true;
                complete();
            }

            void
            set_done() {
                complete();
            }

            value_t
            take_value() {
                if constexpr (std::is_reference_v<value_t>) {
                    return value.value().get();
                } else {
                    return __mov__(value.value());
                }
            }

        private:
            template <typename value_arg_t>
            void
            store_value(value_arg_t&& v) {
                if constexpr (std::is_reference_v<value_t>) {
                    value.emplace(std::ref(v));
                } else {
                    value.emplace(__fwd__(v));
                }
            }

            void
            complete() {
                if (completed)
                    return;
                completed = true;
                if (!in_await_suspend && continuation) {
                    auto handle = continuation;
                    continuation = {};
                    handle.resume();
                }
            }
        };

        template <>
        struct
        await_state_value<void> {
            std::exception_ptr exception{};
            bool skipped = false;
            bool completed = false;
            bool in_await_suspend = false;
            std::coroutine_handle<> continuation{};
            std::shared_ptr<void> context_holder;
            std::unique_ptr<core::scope_holder_base> scope_holder;

            template <typename... value_args_t>
            void
            set_value(value_args_t&& ...v) {
                (void)sizeof...(v);
                complete();
            }

            void
            set_exception(std::exception_ptr e) {
                exception = e;
                complete();
            }

            void
            set_skip() {
                skipped = true;
                complete();
            }

            void
            set_done() {
                complete();
            }

        private:
            void
            complete() {
                if (completed)
                    return;
                completed = true;
                if (!in_await_suspend && continuation) {
                    auto handle = continuation;
                    continuation = {};
                    handle.resume();
                }
            }
        };
    }

    template <typename state_t>
    struct
    await_receiver {
        constexpr static bool await_receiver_op = true;
        state_t* state = nullptr;
    };

    namespace detail {
        template <typename sender_t, typename context_t, bool has_value>
        struct
        sender_awaitable_impl;

        template <typename sender_t, typename context_t>
        struct
        sender_awaitable_impl<sender_t, context_t, true> {
            using sender_type = std::decay_t<sender_t>;
            using compute_t = core::compute_sender_type<context_t, sender_type>;

            consteval static auto
            compute_sender_type_identity() {
                if constexpr (compute_t::count_value() == 0)
                    return std::type_identity<void>{};
                else
                    return compute_t::get_value_type_identity();
            }

            using value_type = decltype(compute_sender_type_identity())::type;


            sender_type sender;
            context_t context;
            await_state_value<value_type> state;

            explicit
            sender_awaitable_impl(sender_t&& s, context_t&& ctx)
                : sender(__fwd__(s))
                , context(__fwd__(ctx)) {
            }

            bool
            await_ready() const noexcept {
                return false;
            }

            bool
            await_suspend(std::coroutine_handle<> h) {
                state.continuation = h;
                state.in_await_suspend = true;
                start_sender();
                state.in_await_suspend = false;
                return !state.completed;
            }

            value_type
            await_resume() {
                if (state.exception)
                    std::rethrow_exception(state.exception);
                if (state.skipped)
                    throw std::runtime_error("awaited sender skipped value");
                return state.take_value();
            }

        private:
            void
            start_sender() {
                using receiver_t = await_receiver<await_state_value<value_type>>;
                auto ops = sender.template connect<context_t>()
                    .push_back(receiver_t{&state})
                    .take();
                using scope_element_t = core::root_scope<__typ__(ops)>;
                auto* raw = new scope_element_t(__mov__(ops));
                state.context_holder = context;
                state.scope_holder = std::make_unique<core::scope_holder_impl<scope_element_t>>(raw);
                auto scope = core::scope_ptr<scope_element_t>(raw);
                core::op_pusher<0, __typ__(scope)>::push_value(context, scope);
            }
        };

        template <typename sender_t, typename context_t>
        struct
        sender_awaitable_impl<sender_t, context_t, false> {
            using sender_type = std::decay_t<sender_t>;
            using compute_t = core::compute_sender_type<context_t, sender_type>;
            using value_type = void;

            sender_type sender;
            context_t context;
            await_state_value<void> state;

            explicit
            sender_awaitable_impl(sender_t&& s, context_t&& ctx)
                : sender(__fwd__(s))
                , context(__fwd__(ctx)) {
            }

            bool
            await_ready() const noexcept {
                return false;
            }

            bool
            await_suspend(std::coroutine_handle<> h) {
                state.continuation = h;
                state.in_await_suspend = true;
                start_sender();
                state.in_await_suspend = false;
                return !state.completed;
            }

            void
            await_resume() {
                if (state.exception)
                    std::rethrow_exception(state.exception);
                if (state.skipped)
                    throw std::runtime_error("awaited sender skipped value");
            }

        private:
            void
            start_sender() {
                using receiver_t = await_receiver<await_state_value<void>>;
                auto ops = sender.template connect<context_t>()
                    .push_back(receiver_t{&state})
                    .take();
                using scope_element_t = core::root_scope<__typ__(ops)>;
                auto* raw = new scope_element_t(__mov__(ops));
                state.context_holder = context;
                state.scope_holder = std::make_unique<core::scope_holder_impl<scope_element_t>>(raw);
                auto scope = core::scope_ptr<scope_element_t>(raw);
                core::op_pusher<0, __typ__(scope)>::push_value(context, scope);
            }
        };
    }

    template <typename sender_t>
    auto
    await_sender(sender_t&& sender) {
        using context_t = decltype(core::make_root_context());
        using sender_type = std::decay_t<sender_t>;

        constexpr bool has_value = (core::compute_sender_type<context_t, sender_type>::count_value() != 0);
        return detail::sender_awaitable_impl<sender_t, context_t, has_value>(
            __fwd__(sender),
            core::make_root_context()
        );
    }

    template <typename sender_t, typename context_t>
    auto
    await_sender(sender_t&& sender, context_t&& context) {
        using context_type = std::decay_t<context_t>;
        using sender_type = std::decay_t<sender_t>;
        constexpr bool has_value = (core::compute_sender_type<context_t, sender_type>::count_value() != 0);
        return detail::sender_awaitable_impl<sender_t, context_type, has_value>(
            __fwd__(sender),
            __fwd__(context)
        );
    }

    namespace _await_able {
        struct
        fn {
            template <typename sender_t>
            requires requires (sender_t&& s) { s.template connect<decltype(core::make_root_context())>(); }
            constexpr
            decltype(auto)
            operator ()(sender_t&& sender) const {
                return await_sender(__fwd__(sender));
            }

            template <typename sender_t, typename context_t>
            requires requires (sender_t&& s) { s.template connect<std::decay_t<context_t>>(); }
            constexpr
            decltype(auto)
            operator ()(sender_t&& sender, context_t&& context) const {
                return await_sender(__fwd__(sender), __fwd__(context));
            }

            constexpr
            decltype(auto)
            operator ()() const {
                return core::bind_back<fn>(fn{});
            }

            template <typename context_t>
            requires requires (context_t&& ctx) {
                typename std::decay_t<context_t>::element_type;
                std::decay_t<context_t>::element_type::root_flag;
            }
            constexpr
            decltype(auto)
            operator ()(context_t&& context) const {
                return core::bind_back<fn, context_t>(fn{}, __fwd__(context));
            }
        };
    }

    inline constexpr _await_able::fn await_able{};
}

namespace pump::core {
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
    && (get_current_op_type_t<pos, scope_t>::await_receiver_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template <typename context_t, typename ...value_t>
        static inline
        void
        push_value(context_t& context, scope_t& scope, value_t&& ...v) {
            (void)context;
            auto& op = std::get<pos>(scope->get_op_tuple());
            op.state->set_value(__fwd__(v)...);
        }

        template <typename context_t>
        static inline
        void
        push_exception(context_t& context, scope_t& scope, std::exception_ptr e) {
            (void)context;
            auto& op = std::get<pos>(scope->get_op_tuple());
            op.state->set_exception(e);
        }

        template <typename context_t>
        static inline
        void
        push_skip(context_t& context, scope_t& scope) {
            (void)context;
            auto& op = std::get<pos>(scope->get_op_tuple());
            op.state->set_skip();
        }

        template <typename context_t>
        static inline
        void
        push_done(context_t& context, scope_t& scope) {
            (void)context;
            auto& op = std::get<pos>(scope->get_op_tuple());
            op.state->set_done();
        }
    };
}

#endif //PUMP_SENDER_AWAIT_SENDER_HH
