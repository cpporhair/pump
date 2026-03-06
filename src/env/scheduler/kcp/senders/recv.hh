
#ifndef ENV_SCHEDULER_KCP_SENDERS_RECV_HH
#define ENV_SCHEDULER_KCP_SENDERS_RECV_HH

#include <cstdint>

#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"
#include "env/scheduler/kcp/common/struct.hh"

namespace pump::scheduler::kcp::senders::recv {

    template <typename scheduler_t>
    struct
    op {
        constexpr static bool kcp_sender_recv_op = true;
        scheduler_t* scheduler;
        common::conv_id_t conv;

        op(scheduler_t* s, common::conv_id_t c) : scheduler(s), conv(c) {}
        op(op&& rhs) noexcept : scheduler(rhs.scheduler), conv(rhs.conv) {}

        template<uint32_t pos, typename context_t, typename scope_t>
        auto
        start(context_t& context, scope_t& scope) {
            return scheduler->schedule(
                new common::recv_req{
                    conv,
                    [context = context, scope = scope](
                        std::variant<pump::common::net_frame, std::exception_ptr>&& res
                    ) mutable {
                        if (res.index() == 0) [[likely]] {
                            core::op_pusher<pos + 1, scope_t>::push_value(
                                context, scope,
                                std::move(std::get<0>(res))
                            );
                        } else {
                            core::op_pusher<pos + 1, scope_t>::push_exception(
                                context, scope, std::get<1>(res)
                            );
                        }
                    }
                }
            );
        }
    };

    template <typename scheduler_t>
    struct
    sender {
        scheduler_t* scheduler;
        common::conv_id_t conv;

        sender(scheduler_t* s, common::conv_id_t c) : scheduler(s), conv(c) {}
        sender(sender&& rhs) noexcept : scheduler(rhs.scheduler), conv(rhs.conv) {}

        inline auto
        make_op() {
            return op<scheduler_t>(scheduler, conv);
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
        && (get_current_op_type_t<pos, scope_t>::kcp_sender_recv_op)
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
    compute_sender_type<context_t, scheduler::kcp::senders::recv::sender<scheduler_t>> {
        consteval static uint32_t
        count_value() {
            return 1;
        }

        consteval static auto
        get_value_type_identity() {
            return std::type_identity<pump::common::net_frame>{};
        }
    };
}

#endif //ENV_SCHEDULER_KCP_SENDERS_RECV_HH
