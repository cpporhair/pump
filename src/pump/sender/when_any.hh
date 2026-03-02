#ifndef PUMP_SENDER_WHEN_ANY_HH
#define PUMP_SENDER_WHEN_ANY_HH

#include <variant>
#include <atomic>

#include "../core/context.hh"
#include "../core/compute_sender_type.hh"
#include "../core/op_pusher.hh"
#include "../core/op_tuple_builder.hh"
#include "../core/helper.hh"
#include "./get_context.hh"
#include "./flat.hh"

namespace pump::sender {
    template <typename ...value_t>
    struct
    when_any_res {
    };

    template <typename value_t>
    struct
    when_any_res<value_t> {
        using type = std::variant<std::monostate, std::exception_ptr, value_t>;
    };

    template <>
    struct
    when_any_res<void> {
        using type = std::variant<std::monostate, std::exception_ptr, nullptr_t>;
    };

    template <typename context_t, typename sender_t>
    struct
    when_any_compute {
        using sender_value_class = core::compute_sender_type<context_t, sender_t>;

        consteval static uint32_t
        count_prev_value() {
            return sender_value_class::count_value();
        }

        consteval static uint32_t
        count_value() {
            return 1;
        }

        consteval static auto
        get_value_type_identity() {
            if constexpr (count_prev_value() == 0)
                return std::type_identity<void>{};
            else
                return sender_value_class::get_value_type_identity();
        }
    };

    namespace _when_any {
        template <uint32_t pos, typename parent_context_t, typename parent_scope_t>
        struct
        parent_pusher_status {
            constexpr static uint32_t parent_pusher_pos = pos;
            parent_context_t context;
            parent_scope_t scope;
        };

        // ================================================================
        // race_wrapper: core competition logic for when_any
        // ================================================================
        template <uint32_t max, typename result_variant_t, typename parent_pusher_status_t>
        struct
        alignas(64)
        race_wrapper {
            // === packed state: bit 63 = finished flag, bits [0,62] = ref_count ===
            // Merging finished + ref_count into a single atomic<uint64_t> eliminates
            // any observation window between the two operations. For losers, a single
            // fetch_sub is enough (finished bit is already set, ref_count decrements).
            static constexpr uint64_t FINISHED_BIT = uint64_t(1) << 63;
            static constexpr uint64_t REF_MASK    = ~FINISHED_BIT;

            std::atomic<uint64_t> state;

            // === cold data (written once by winner, then read-only) ===
            uint32_t winner_index;
            result_variant_t result;

            // === set at construction, read-only afterwards ===
            parent_pusher_status_t parent_pusher_status;

            race_wrapper(uint32_t branch_count, parent_pusher_status_t&& p)
                : state(uint64_t(branch_count))
                , winner_index(0)
                , parent_pusher_status(__fwd__(p)) {
            }

            race_wrapper(race_wrapper&&) = delete;

            race_wrapper(const race_wrapper&) = delete;

            inline
            void
            release_ref() {
                if ((state.fetch_sub(1, std::memory_order_acq_rel) & REF_MASK) == 1) [[unlikely]] {
                    delete this;
                }
            }

            template <uint32_t index, typename ...value_t>
            inline
            void
            set_value(value_t&& ...v) {
                uint64_t old = state.fetch_or(FINISHED_BIT, std::memory_order_acq_rel);
                if (!(old & FINISHED_BIT)) {
                    winner_index = index;
                    if constexpr (sizeof...(v) == 0)
                        result.template emplace<2>(nullptr);
                    else
                        result.template emplace<2>(__fwd__(v)...);
                    core::op_pusher<
                        parent_pusher_status_t::parent_pusher_pos,
                        __typ__(parent_pusher_status.scope)
                    >::
                    push_value(
                        parent_pusher_status.context,
                        parent_pusher_status.scope,
                        uint32_t(winner_index),
                        __mov__(result)
                    );
                }
                release_ref();
            }

            template <uint32_t index>
            inline
            void
            set_error(std::exception_ptr e) {
                uint64_t old = state.fetch_or(FINISHED_BIT, std::memory_order_acq_rel);
                if (!(old & FINISHED_BIT)) {
                    winner_index = index;
                    result.template emplace<1>(e);
                    core::op_pusher<
                        parent_pusher_status_t::parent_pusher_pos,
                        __typ__(parent_pusher_status.scope)
                    >::
                    push_value(
                        parent_pusher_status.context,
                        parent_pusher_status.scope,
                        uint32_t(winner_index),
                        __mov__(result)
                    );
                }
                release_ref();
            }

            template <uint32_t index>
            inline
            void
            set_done() {
                uint64_t old = state.fetch_or(FINISHED_BIT, std::memory_order_acq_rel);
                if (!(old & FINISHED_BIT)) {
                    winner_index = index;
                    result.template emplace<0>(std::monostate{});
                    core::op_pusher<
                        parent_pusher_status_t::parent_pusher_pos,
                        __typ__(parent_pusher_status.scope)
                    >::
                    push_value(
                        parent_pusher_status.context,
                        parent_pusher_status.scope,
                        uint32_t(winner_index),
                        __mov__(result)
                    );
                }
                release_ref();
            }

            template <uint32_t index>
            inline
            void
            set_skip() {
                uint64_t old = state.fetch_or(FINISHED_BIT, std::memory_order_acq_rel);
                if (!(old & FINISHED_BIT)) {
                    winner_index = index;
                    result.template emplace<0>(std::monostate{});
                    core::op_pusher<
                        parent_pusher_status_t::parent_pusher_pos,
                        __typ__(parent_pusher_status.scope)
                    >::
                    push_value(
                        parent_pusher_status.context,
                        parent_pusher_status.scope,
                        uint32_t(winner_index),
                        __mov__(result)
                    );
                }
                release_ref();
            }
        };

        // ================================================================
        // reducer: routes signals to race_wrapper
        // ================================================================
        template <uint32_t index, typename race_wrapper_t>
        struct
        reducer {
            constexpr static uint32_t pos = index;
            constexpr static bool when_any_reducer = true;
            race_wrapper_t* wrapper;

            explicit
            reducer(race_wrapper_t* c)
                : wrapper(c) {
            }

            reducer(reducer&& rhs) noexcept
                : wrapper(rhs.wrapper) {
            }

            reducer(const reducer& rhs) = delete;

            template <typename ...value_t>
            inline
            auto
            set_value(value_t&& ...value) {
                wrapper->template set_value<index>(__fwd__(value)...);
            }

            void
            set_error(std::exception_ptr e) {
                wrapper->template set_error<index>(e);
            }

            void
            set_done() {
                wrapper->template set_done<index>();
            }

            void
            set_skip() {
                wrapper->template set_skip<index>();
            }
        };

        // ================================================================
        // starter: creates race_wrapper and launches all branches
        // ================================================================
        template <typename result_t, typename ...op_list_t>
        struct
        starter {
            constexpr static bool when_any_starter = true;
            std::tuple<op_list_t...> all_inner_op_list;

            starter(op_list_t&& ...opt)
                : all_inner_op_list(std::make_tuple(__fwd__(opt)...)) {
            }

            starter(starter&& rhs)
                : all_inner_op_list(__fwd__(rhs.all_inner_op_list)) {
            }

            starter(const starter& rhs) = delete;

            // Multi-branch path: submit with exception safety
            template <uint32_t index, typename context_t, typename scope_t, typename wrapper_t>
            inline
            void
            submit_senders(context_t& context, scope_t& scope, wrapper_t* wrapper) {
                auto new_scope = make_runtime_scope<core::runtime_scope_type::other>(
                    scope,
                    push_back_to_op_list(
                        core::tuple_to_tie(std::get<index>(all_inner_op_list)),
                        reducer<index, wrapper_t>(wrapper)
                    )
                );

                try {
                    core::op_pusher<0, __typ__(new_scope)>::push_value(context, new_scope);
                } catch (...) {
                    // Compensate ref_count for this and all unstarted branches
                    constexpr uint32_t remaining = sizeof...(op_list_t) - index;
                    for (uint32_t i = 0; i < remaining; ++i) {
                        wrapper->release_ref();
                    }
                    throw;
                }

                if constexpr ((index + 1) < sizeof...(op_list_t))
                    submit_senders<index + 1>(context, scope, wrapper);
            }

            template <typename context_t, typename scope_t, typename parent_pusher_status_t>
            auto
            start(context_t& context, scope_t& scope, parent_pusher_status_t&& s) {
                constexpr uint32_t branch_count = sizeof...(op_list_t);
                auto* wrapper = new race_wrapper<branch_count, result_t, parent_pusher_status_t>(
                    branch_count, __fwd__(s)
                );
                submit_senders<0>(context, scope, wrapper);
            }
        };

        // ================================================================
        // sender: connects prev pipeline with inner senders
        // ================================================================
        template <typename prev_t, typename ...sender_t>
        struct
        sender {

            using prev_type = prev_t;

            std::tuple<sender_t...> inner_senders;
            prev_t prev;

            sender(prev_t&& p, sender_t&& ...inner_sender)
                : prev(__fwd__(p))
                , inner_senders(std::make_tuple(__fwd__(inner_sender)...)) {
            }

            sender(sender&& o) noexcept
                : prev(__fwd__(o.prev))
                , inner_senders(__fwd__(o.inner_senders)) {
            }

            sender(const sender& o) = delete;

            template <uint32_t index, typename context_t>
            inline
            auto
            connect_inner_sender() {
                if constexpr (size_t(index + 1) == sizeof...(sender_t))
                    return std::make_tuple(std::get<index>(inner_senders).template connect<context_t>().take());
                else
                    return std::tuple_cat(
                        std::make_tuple(std::get<index>(inner_senders).template connect<context_t>().take()),
                        connect_inner_sender<index + 1, context_t>()
                    );
            }

            template <typename context_t>
            auto
            connect() {
                // All branches must produce the same value type
                using first_sender_t = std::tuple_element_t<0, std::tuple<sender_t...>>;
                using first_value_type = typename decltype(when_any_compute<context_t, first_sender_t>::get_value_type_identity())::type;

                static_assert(
                    (std::is_same_v<
                        first_value_type,
                        typename decltype(when_any_compute<context_t, sender_t>::get_value_type_identity())::type
                    > && ...),
                    "when_any: all branches must produce the same value type"
                );

                using result_t = typename when_any_res<first_value_type>::type;

                return prev.template connect<context_t>().push_back(
                    std::apply(
                        [](auto&& ...args) {
                            return starter<result_t, __typ__(args)...>(__fwd__(args)...);
                        },
                        connect_inner_sender<uint32_t(0), context_t>()
                    )
                );
            }
        };

        struct
        fn2 {
            template <typename prev_t, typename ...inner_sender_t>
            constexpr
            decltype(auto)
            operator()(prev_t&& p, inner_sender_t&& ...i) const {
                return _when_any::sender<prev_t, inner_sender_t...>{__fwd__(p), __fwd__(i)...};
            }
        };

        struct
        fn1 {
            template <typename ...sender_t>
            constexpr
            decltype(auto)
            operator()(sender_t&& ...senders) const {
                __all_must_rval__(senders);
                static_assert(sizeof...(senders) > 0, "no senders");
                return core::bind_back<fn2, sender_t...>(fn2{}, __fwd__(senders)...);
            }
        };
    }

    inline constexpr _when_any::fn1 when_any{};
}

namespace pump::core {
    // ================================================================
    // op_pusher specialization for when_any reducer
    // ================================================================
    template <uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
    && (get_current_op_type_t<pos, scope_t>::when_any_reducer)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template <typename context_t, typename ...value_t>
        static inline
        void
        push_value(context_t& context, scope_t& scope, value_t&& ...v) {
            std::get<pos>(scope->get_op_tuple()).set_value(__fwd__(v)...);
        }

        template <typename context_t>
        static inline
        void
        push_exception(context_t& context, scope_t& scope, std::exception_ptr e) {
            std::get<pos>(scope->get_op_tuple()).set_error(e);
        }

        template <typename context_t>
        static inline
        void
        push_skip(context_t& context, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).set_skip();
        }

        template <typename context_t>
        static inline
        void
        push_done(context_t& context, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).set_done();
        }
    };

    // ================================================================
    // op_pusher specialization for when_any starter
    // ================================================================
    template <uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
    && (get_current_op_type_t<pos, scope_t>::when_any_starter)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template <typename context_t, typename ...value_t>
        static inline
        void
        push_value(context_t& context, scope_t& scope, value_t&& ...v) {
            std::get<pos>(scope->get_op_tuple())
                .start(context, scope,
                       sender::_when_any::parent_pusher_status<pos + 1, context_t, scope_t>{context, scope});
        }
    };

    // ================================================================
    // compute_sender_type specialization for when_any sender
    // ================================================================
    template <typename context_t, typename prev_t, typename ...sender_t>
    struct
    compute_sender_type<context_t, sender::_when_any::sender<prev_t, sender_t...>> {

        consteval static uint32_t
        count_value() {
            return 2;  // winner_index + result
        }

        consteval static auto
        get_value_type_identity() {
            static_assert(count_value() != 0, "no value_type");

            using first_sender_t = std::tuple_element_t<0, std::tuple<sender_t...>>;
            using result_variant_t = typename sender::when_any_res<
                typename decltype(sender::when_any_compute<context_t, first_sender_t>::get_value_type_identity())::type
            >::type;

            using t0 = std::tuple<uint32_t, result_variant_t>;
            return std::type_identity<t0>{};
        }
    };
}

#endif //PUMP_SENDER_WHEN_ANY_HH
