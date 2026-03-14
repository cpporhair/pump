#ifndef PUMP_SENDER_SEQUENTIAL_HH
#define PUMP_SENDER_SEQUENTIAL_HH


#include "../core/op_tuple_builder.hh"
#include "../core/lock_free_queue.hh"

#include "./submit.hh"
#include "./concurrent.hh"

namespace pump::sender {

    namespace _sequential {
        // Backpressure-aware lock-free state machine states
        constexpr uint32_t STATE_IDLE = 0;
        constexpr uint32_t STATE_BUSY = 1;
        constexpr uint32_t STATE_DONE = 2;

        // Anti-recursion drain flag bits
        constexpr uint32_t DRAIN_ACTIVE = 1;     // drain in progress
        constexpr uint32_t DRAIN_REQUESTED = 2;  // re-drain requested while active

        template <typename value_t, typename stream_op_tuple_t, size_t max_cache_size = 1024>
        struct
        source_cache {
            // Adaptive storage: variant stored inline for small types, via unique_ptr for large types
            using storage_type = core::adaptive_storage_t<value_t>;
            constexpr static bool sequential_source_cache = true;
            constexpr static bool uses_inline_storage = std::is_same_v<storage_type, value_t>;

            std::unique_ptr<core::mpmc::queue<storage_type, max_cache_size>> cache;
            stream_op_tuple_t stream_op_tuple;
            // State machine: STATE_IDLE, STATE_BUSY, STATE_DONE
            std::atomic<uint32_t> state{STATE_IDLE};
            // Atomic anti-recursion flag: DRAIN_ACTIVE | DRAIN_REQUESTED
            std::atomic<uint32_t> drain_flag{0};

            explicit
            source_cache(stream_op_tuple_t&& ops)
                : cache(new core::mpmc::queue<storage_type, max_cache_size>)
                , stream_op_tuple(__fwd__(ops)) {
                __must_rval__(ops);
            }

            source_cache(source_cache&& o) noexcept
                : cache(__fwd__(o.cache))
                , stream_op_tuple(__fwd__(o.stream_op_tuple))
                , state(o.state.load(std::memory_order_relaxed))
                , drain_flag(o.drain_flag.load(std::memory_order_relaxed)) {
                __must_rval__(o);
            }

            source_cache(const source_cache&) = delete;

            void
            reset() {
                state.store(STATE_IDLE, std::memory_order_relaxed);
                drain_flag.store(0, std::memory_order_relaxed);
            }
        };
    }

    namespace _sequential {
        template <uint32_t pos, typename cache_vaule_t, typename parent_builder_t, typename stream_builder_t>
        struct
        sequential_starter_builder {

            constexpr static uint32_t cur_pos = pos;

            parent_builder_t parent_builder;
            stream_builder_t stream_builder;

            sequential_starter_builder(parent_builder_t&& p, stream_builder_t&& s)
                : parent_builder(__fwd__(p))
                , stream_builder(__fwd__(s)){
                __must_rval__(p);
                __must_rval__(s);
            }

            sequential_starter_builder(sequential_starter_builder&& rhs) noexcept
                : parent_builder(__fwd__(rhs.parent_builder))
                , stream_builder(__fwd__(rhs.stream_builder)){
            }

            sequential_starter_builder(const sequential_starter_builder&) = delete;

            template<typename pushed_op_t>
            auto
            push_back(pushed_op_t&& op){
                return
                    sequential_starter_builder
                        <
                            pos + 1,
                            cache_vaule_t,
                            __typ__(parent_builder),
                            __typ__(stream_builder.push_back(__fwd__(op)))
                        >
                        (
                            __mov__(parent_builder),
                            stream_builder.push_back(__fwd__(op))
                        );
            }

            auto
            make_source_cache() {
                return _sequential::source_cache<cache_vaule_t, __typ__(stream_builder.take())> ( __mov__(stream_builder.take()));
            }

            template<typename loop_end_op_op_t>
            auto
            push_ctrl_op(loop_end_op_op_t&& op) {
                if constexpr (std::tuple_size_v<__typ__(stream_builder.take())> == 0)
                    return parent_builder.push_ctrl_op(__fwd__(op));
                else
                    return parent_builder.push_ctrl_op(make_source_cache()).push_ctrl_op(__fwd__(op));
            }

            template<typename reduce_op_t>
            auto
            push_reduce_op(reduce_op_t&& op) {
                if constexpr (std::tuple_size_v<__typ__(stream_builder.take())> == 0)
                    return parent_builder.push_reduce_op(__fwd__(op));
                else
                    return parent_builder.push_ctrl_op(make_source_cache()).push_reduce_op(__fwd__(op));
            }

            constexpr static uint32_t
            get_reduce_op_next_pos() {
                return parent_builder_t::get_reduce_op_next_pos();
            }
        };

        template <typename prev_t>
        struct
        __ncp__(sender) {
            using prev_type = prev_t;

            prev_t prev;

            explicit
            sender(prev_t&& p)
                : prev(__fwd__(p)){
                __must_rval__(p);
            }

            sender(sender&& o)
                : prev(__fwd__(o.prev)){
            }

            template<typename context_t>
            auto
            connect() {
                if constexpr (core::compute_sender_type<context_t, prev_t>::count_value() == 0) {
                    using cache_value_type = std::variant<std::monostate, std::exception_ptr, nullptr_t>;
                    return sequential_starter_builder
                        <
                            1,
                            cache_value_type,
                            __typ__(prev.template connect<context_t>()),
                            ::pump::core::builder::op_list_builder<0>
                        >
                        (
                            prev.template connect<context_t>(),
                            ::pump::core::builder::op_list_builder<0>()
                        );
                }
                else {
                    using cache_value_type = std::variant<
                        std::monostate,
                        std::exception_ptr,
                        typename decltype(core::compute_sender_type<context_t, prev_t>::get_value_type_identity())::type
                    >;
                    return sequential_starter_builder
                        <
                            1,
                            cache_value_type,
                            __typ__(prev.template connect<context_t>()),
                            ::pump::core::builder::op_list_builder<0>
                        >
                        (
                            prev.template connect<context_t>(),
                            ::pump::core::builder::op_list_builder<0>()
                        );
                }

            }
        };

        struct
        fn {
            template <typename sender_t>
            constexpr
            decltype(auto)
            operator ()(sender_t&& s) const {
                __must_rval__(s);
                return sender<sender_t>{__fwd__(s)};
            }


            decltype(auto)
            operator ()() const {
                return ::pump::core::bind_back<fn>(fn{});
            }
        };
    }

    inline constexpr _sequential::fn sequential{};

    template <typename  bind_back_t>
    inline
    auto
    with_concurrent(bind_back_t&& b){
        return concurrent() >> __fwd__(b) >> sequential() ;
    }
}

namespace pump::core {
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
    && (get_current_op_type_t<pos, scope_t>::sequential_source_cache)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {

        // Dispatch a variant to the downstream op_pusher
        template<uint32_t start_pos, typename context_t, typename this_scope_t>
        static
        void
        dispatch_variant(context_t& context, this_scope_t& scope, auto&& var) {
            using var_t = std::decay_t<decltype(var)>;
            switch(var.index()) {
                case 0:
                    op_pusher<start_pos, this_scope_t>::push_skip(context, scope);
                    return;
                case 1:
                    op_pusher<start_pos, this_scope_t>::push_exception(context, scope, __mov__(std::get<1>(var)));
                    return;
                case 2:
                    if constexpr (std::is_null_pointer_v<std::variant_alternative_t<2, var_t>>) {
                        op_pusher<start_pos, this_scope_t>::push_value(context, scope);
                    } else {
                        op_pusher<start_pos, this_scope_t>::push_value(context, scope, __mov__(std::get<2>(var)));
                    }
                    return;
                default: ;
            }
        }

        // Push downstream from a dequeued storage item (handles both inline and unique_ptr)
        template<uint32_t start_pos, typename context_t, typename this_scope_t, typename storage_item_t>
        static
        void
        push_from_storage(context_t& context, this_scope_t& scope, storage_item_t&& item) {
            using op_t = get_current_op_type_t<pos, scope_t>;
            if constexpr (op_t::uses_inline_storage) {
                dispatch_variant<start_pos>(context, scope, __mov__(item));
            } else {
                dispatch_variant<start_pos>(context, scope, __mov__(*item));
            }
        }

        // Enqueue a variant into the cache with adaptive storage wrapping
        template<typename variant_val_t>
        static
        bool
        enqueue_to_cache(auto& op, variant_val_t&& var) {
            using op_t = std::decay_t<decltype(op)>;
            if constexpr (op_t::uses_inline_storage) {
                return op.cache->try_enqueue(__fwd__(var));
            } else {
                return op.cache->try_enqueue(
                    std::make_unique<std::decay_t<variant_val_t>>(__fwd__(var)));
            }
        }

        template<typename this_scope_t>
        static auto
        make_new_scope(this_scope_t& scope) {
            if constexpr (this_scope_t::element_type::scope_type == runtime_scope_type::other && pos == 0) {
                return scope;
            } else {
                auto& op = std::get<pos>(scope->get_op_tuple());
                // Use stream scope as base (not the transient caller scope like element child scope).
                // The caller scope may be deleted while the buffer scope is still alive (async path).
                auto& stream_scope = find_stream_starter(scope);
                return make_runtime_scope<runtime_scope_type::other>(
                    stream_scope,
                    std::tuple_cat(
                        std::tie(op),
                        tuple_to_tie(op.stream_op_tuple),
                        std::tie(std::get<__typ__(op)::pos + 1>(stream_scope->get_op_tuple()))
                    )
                );
            }
        }

        // Core drain logic: attempt to dequeue and push one item downstream.
        // Returns after pushing one item (non-blocking) or transitions state.
        // Anti-recursion: if downstream synchronously calls poll_next, the
        // drain_flag atomic flattens the recursion into a loop.
        template<typename context_t>
        static
        void
        drain_single(context_t& context, scope_t& scope) {
            auto& op = std::get<pos>(scope->get_op_tuple());

            // Anti-recursion: if already draining, atomically set DRAIN_REQUESTED and return
            uint32_t df = op.drain_flag.load(std::memory_order_acquire);
            if (df & sender::_sequential::DRAIN_ACTIVE) {
                // Another invocation is active — request re-drain via atomic CAS
                while (df & sender::_sequential::DRAIN_ACTIVE) {
                    if (op.drain_flag.compare_exchange_weak(
                            df, df | sender::_sequential::DRAIN_REQUESTED,
                            std::memory_order_acq_rel, std::memory_order_relaxed))
                        return;
                }
                // DRAIN_ACTIVE cleared while we were trying — fall through to normal path
            }

            auto new_scope = make_new_scope(scope);

            // Loop to flatten synchronous recursive poll_next calls
            while (true) {
                if (auto item = op.cache->try_dequeue()) {
                    // Got an item - mark drain active, push downstream
                    op.drain_flag.store(sender::_sequential::DRAIN_ACTIVE, std::memory_order_release);
                    push_from_storage<1>(context, new_scope, __mov__(*item));
                    // Atomically clear flags and check if re-drain was requested
                    if (op.drain_flag.exchange(0, std::memory_order_acq_rel)
                        & sender::_sequential::DRAIN_REQUESTED)
                        continue;
                    // Async path: downstream did not call poll_next synchronously.
                    // Return and wait for async poll_next callback.
                    // DON'T delete new_scope — async pipeline still using it.
                    return;
                }

                // Queue empty - check state
                uint32_t s = op.state.load(std::memory_order_acquire);

                if (s >= sender::_sequential::STATE_DONE) {
                    // DONE signal received - check if queue truly empty (recheck for race)
                    if (auto late_item = op.cache->try_dequeue()) {
                        op.drain_flag.store(sender::_sequential::DRAIN_ACTIVE, std::memory_order_release);
                        push_from_storage<1>(context, new_scope, __mov__(*late_item));
                        if (op.drain_flag.exchange(0, std::memory_order_acq_rel)
                            & sender::_sequential::DRAIN_REQUESTED)
                            continue;
                        // Async path: don't delete new_scope
                        return;
                    }
                    // Queue confirmed empty. Push done exactly once via terminal state.
                    // Use STATE_DONE|STATE_BUSY (= 3) as terminal to prevent re-entry.
                    uint32_t expected = sender::_sequential::STATE_DONE;
                    if (op.state.compare_exchange_strong(expected,
                        sender::_sequential::STATE_DONE | sender::_sequential::STATE_BUSY,
                        std::memory_order_acq_rel, std::memory_order_relaxed)) {
                        op_pusher<1, __typ__(new_scope)>::push_done(context, new_scope);
                        // After push_done, stream scope may be deleted by pop_to_loop_starter.
                        // op is a reference into stream scope — don't access it anymore.
                        delete new_scope.get();
                    }
                    return;
                }

                // Queue empty, not done (state == BUSY) - transition back to IDLE
                if (op.state.compare_exchange_strong(s, sender::_sequential::STATE_IDLE,
                    std::memory_order_release, std::memory_order_relaxed)) {
                    // Successfully went IDLE. Check for race: new data or DONE
                    // may have arrived between our failed dequeue and IDLE transition.
                    if (!op.cache->empty() || op.state.load(std::memory_order_acquire) >= sender::_sequential::STATE_DONE) {
                        uint32_t idle = sender::_sequential::STATE_IDLE;
                        if (op.state.compare_exchange_strong(idle, sender::_sequential::STATE_BUSY,
                            std::memory_order_acq_rel, std::memory_order_relaxed)) {
                            continue;
                        }
                    }
                    // No more work, delete buffer scope
                    delete new_scope.get();
                    return;
                }
                // CAS failed - state changed (DONE arrived), re-loop
            }
        }

        // Poll path: downstream signals readiness, trigger drain
        template<typename context_t>
        static
        void
        poll_next(context_t& context, scope_t& scope) {
            drain_single(context, scope);
        }

        // Push path helper: enqueue variant and try to acquire push right
        template<typename context_t, typename var_t>
        static
        void
        enqueue_and_drain(context_t& context, scope_t& scope, var_t&& var) {
            auto& op = std::get<pos>(scope->get_op_tuple());

            // Enqueue data (lock-free)
            enqueue_to_cache(op, __fwd__(var));

            // Try to acquire BUSY from IDLE (push right)
            uint32_t expected = sender::_sequential::STATE_IDLE;
            if (op.state.compare_exchange_strong(expected, sender::_sequential::STATE_BUSY,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
                // Acquired push right - drain one item
                drain_single(context, scope);
            }
            // If failed (already BUSY or DONE), data stays in queue.
            // The current BUSY holder will pick it up.
        }

        template<typename context_t, typename value_t>
        static
        void
        push_value(context_t& context, scope_t& scope, value_t&& v) {
            using op_t = get_current_op_type_t<pos, scope_t>;
            using var_t = std::decay_t<typename op_t::storage_type>;
            // Determine the actual variant type
            if constexpr (op_t::uses_inline_storage) {
                using actual_var_t = typename op_t::storage_type;
                enqueue_and_drain(context, scope, actual_var_t(__fwd__(v)));
            } else {
                using actual_var_t = typename op_t::storage_type::element_type;
                enqueue_and_drain(context, scope, actual_var_t(__fwd__(v)));
            }
        }

        template<typename context_t>
        static
        void
        push_value(context_t& context, scope_t& scope) {
            using op_t = get_current_op_type_t<pos, scope_t>;
            if constexpr (op_t::uses_inline_storage) {
                using actual_var_t = typename op_t::storage_type;
                enqueue_and_drain(context, scope, actual_var_t(nullptr));
            } else {
                using actual_var_t = typename op_t::storage_type::element_type;
                enqueue_and_drain(context, scope, actual_var_t(nullptr));
            }
        }

        template<typename context_t>
        static
        void
        push_done(context_t& context, scope_t& scope) {
            auto& op = std::get<pos>(scope->get_op_tuple());
            // Signal DONE. No more values will be enqueued after this.
            uint32_t prev = op.state.exchange(sender::_sequential::STATE_DONE, std::memory_order_acq_rel);
            if (prev == sender::_sequential::STATE_IDLE) {
                // Was idle - we need to drain the DONE signal
                drain_single(context, scope);
            }
            // If prev was BUSY, the current drainer will detect DONE and push it.
        }

        template<typename context_t>
        static
        void
        push_stream_done(context_t& context, scope_t& scope, uint32_t) {
            auto& op = std::get<pos>(scope->get_op_tuple());
            // Stream ended — same as push_done: no more values will arrive.
            uint32_t prev = op.state.exchange(sender::_sequential::STATE_DONE, std::memory_order_acq_rel);
            if (prev == sender::_sequential::STATE_IDLE) {
                drain_single(context, scope);
            }
        }

        template<typename context_t>
        static inline
        void
        push_exception(context_t& context, scope_t& scope, std::exception_ptr e) {
            using op_t = get_current_op_type_t<pos, scope_t>;
            if constexpr (op_t::uses_inline_storage) {
                using actual_var_t = typename op_t::storage_type;
                enqueue_and_drain(context, scope, actual_var_t(__fwd__(e)));
            } else {
                using actual_var_t = typename op_t::storage_type::element_type;
                enqueue_and_drain(context, scope, actual_var_t(__fwd__(e)));
            }
        }

        template<typename context_t>
        static inline
        void
        push_skip(context_t& context, scope_t& scope) {
            using op_t = get_current_op_type_t<pos, scope_t>;
            if constexpr (op_t::uses_inline_storage) {
                using actual_var_t = typename op_t::storage_type;
                enqueue_and_drain(context, scope, actual_var_t(std::monostate()));
            } else {
                using actual_var_t = typename op_t::storage_type::element_type;
                enqueue_and_drain(context, scope, actual_var_t(std::monostate()));
            }
        }
    };

    template <typename context_t, typename sender_t>
    struct
    compute_sender_type<context_t, sender::_sequential::sender<sender_t>> {
        consteval static uint32_t
        count_value() {
            return compute_sender_type<context_t, sender_t>::count_value();
        }

        consteval static auto
        get_value_type_identity() {
            return compute_sender_type<context_t, sender_t>::get_value_type_identity();
        }
    };
}


#endif //PUMP_SENDER_SEQUENTIAL_HH
