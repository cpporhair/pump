
#ifndef ENV_SCHEDULER_UDP_SENDERS_SEND_HH
#define ENV_SCHEDULER_UDP_SENDERS_SEND_HH

#include <cstdint>

#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"
#include "env/scheduler/udp/common/struct.hh"

namespace pump::scheduler::udp::senders::send {

    template <typename scheduler_t>
    struct
    op {
        constexpr static bool udp_sender_send_op = true;
        scheduler_t* scheduler;
        common::endpoint target;
        common::datagram frame;

        op(scheduler_t* s, common::endpoint ep, common::datagram&& f)
            : scheduler(s)
            , target(ep)
            , frame(__fwd__(f)) {}

        op(op&& rhs) noexcept
            : scheduler(rhs.scheduler)
            , target(rhs.target)
            , frame(__fwd__(rhs.frame)) {}

        template<uint32_t pos, typename context_t, typename scope_t>
        auto
        start(context_t& context, scope_t& scope) {
            return scheduler->schedule(
                new common::send_req{
                    target,
                    __mov__(frame),
                    [context = context, scope = scope](bool ok) mutable {
                        core::op_pusher<pos + 1, scope_t>::push_value(
                            context, scope, ok
                        );
                    }
                }
            );
        }
    };

    template <typename scheduler_t>
    struct
    sender {
        scheduler_t* scheduler;
        common::endpoint target;
        common::datagram frame;

        sender(scheduler_t* s, common::endpoint ep, common::datagram&& f)
            : scheduler(s)
            , target(ep)
            , frame(__fwd__(f)) {}

        sender(sender&& rhs) noexcept
            : scheduler(rhs.scheduler)
            , target(rhs.target)
            , frame(__fwd__(rhs.frame)) {}

        inline auto
        make_op() {
            return op<scheduler_t>(scheduler, target, __mov__(frame));
        }

        template <typename context_t>
        auto
        connect() {
            return core::builder::op_list_builder<0>().push_back(make_op());
        }
    };
}

namespace pump::core {
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::udp_sender_send_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename context_t>
        static void
        push_value(context_t& context, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(context, scope);
        }
    };

    template <typename context_t, typename scheduler_t>
    struct
    compute_sender_type<context_t, scheduler::udp::senders::send::sender<scheduler_t>> {
        consteval static uint32_t
        count_value() {
            return 1;
        }

        consteval static auto
        get_value_type_identity() {
            return std::type_identity<bool>{};
        }
    };
}

#endif //ENV_SCHEDULER_UDP_SENDERS_SEND_HH
