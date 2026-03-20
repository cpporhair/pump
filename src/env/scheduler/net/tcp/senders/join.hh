#ifndef ENV_SCHEDULER_TCP_SENDERS_JOIN_HH
#define ENV_SCHEDULER_TCP_SENDERS_JOIN_HH
#include <cstdint>
#include <bits/move_only_function.h>

#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"

#include "../common/struct.hh"
#include "../common/error.hh"

namespace pump::scheduler::tcp::senders::join {
    template <typename scheduler_t, typename session_t>
    struct
    op {
        constexpr static bool net_sender_join_op = true;
        scheduler_t* scheduler;
        session_t* session;

        op(scheduler_t* s, session_t* sess)
            : scheduler(s)
            , session(sess) {
        }

        op(op&& rhs) noexcept
            : scheduler(rhs.scheduler)
            , session(rhs.session) {
        }

        template<uint32_t pos, typename context_t, typename scope_t>
        auto
        start(context_t &context, scope_t &scope) {
            return scheduler->schedule_join(
                new common::join_req<session_t>{
                    session,
                    [context = context, scope = scope](bool res) mutable {
                        if (res) [[likely]] {
                            core::op_pusher<pos + 1, scope_t>::push_value(context, scope);
                        }
                        else {
                            core::op_pusher<pos + 1, scope_t>::push_exception(context, scope, std::make_exception_ptr(common::join_failed_error()));
                        }
                    }
                }
            );
        }
    };

    template<typename scheduler_t, typename session_t>
    struct
    sender {
        scheduler_t* scheduler;
        session_t* session;

        sender(scheduler_t* s, session_t* sess)
            : scheduler(s)
            , session(sess) {
        }

        sender(sender &&rhs) noexcept
            : scheduler(rhs.scheduler)
            , session(rhs.session) {
        }

        inline
        auto
        make_op(){
            return op<scheduler_t, session_t>(scheduler, session);
        }

        template<typename context_t>
        auto
        connect() {
            return core::builder::op_list_builder<0>().push_back(make_op());
        }
    };
}

namespace pump::core {
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
    && (get_current_op_type_t<pos, scope_t>::net_sender_join_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename context_t>
        static void
        push_value(context_t& context, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(context, scope);
        }
    };

    template <typename context_t, typename scheduler_t, typename session_t>
    struct
    compute_sender_type<context_t, scheduler::tcp::senders::join::sender<scheduler_t, session_t>> {
        consteval static uint32_t
        count_value() {
            return 0;
        }
    };
}

#endif //ENV_SCHEDULER_TCP_SENDERS_JOIN_HH
