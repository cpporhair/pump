
#ifndef ENV_SCHEDULER_KCP_SENDERS_CONNECT_HH
#define ENV_SCHEDULER_KCP_SENDERS_CONNECT_HH

#include <cstdint>
#include <arpa/inet.h>

#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"
#include "env/scheduler/net/kcp/common/struct.hh"

namespace pump::scheduler::kcp::senders::connect {

    template <typename scheduler_t>
    struct
    op {
        constexpr static bool kcp_sender_connect_op = true;
        scheduler_t* scheduler;
        sockaddr_in target;

        op(scheduler_t* s, const char* ip, uint16_t port)
            : scheduler(s)
        {
            target = {};
            target.sin_family = AF_INET;
            target.sin_port = htons(port);
            inet_pton(AF_INET, ip, &target.sin_addr);
        }

        op(op&& rhs) noexcept : scheduler(rhs.scheduler), target(rhs.target) {}

        template<uint32_t pos, typename context_t, typename scope_t>
        auto
        start(context_t& context, scope_t& scope) {
            return scheduler->schedule(
                new common::connect_req{
                    target,
                    [context = context, scope = scope, sche = scheduler](
                        std::variant<common::conv_id_t, std::exception_ptr>&& res
                    ) mutable {
                        if (res.index() == 0) [[likely]] {
                            auto* session = sche->get_session(std::get<0>(res));
                            core::op_pusher<pos + 1, scope_t>::push_value(
                                context, scope, session
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
        const char* ip;
        uint16_t port;

        sender(scheduler_t* s, const char* ip_, uint16_t port_)
            : scheduler(s), ip(ip_), port(port_) {}

        sender(sender&& rhs) noexcept
            : scheduler(rhs.scheduler), ip(rhs.ip), port(rhs.port) {}

        inline auto
        make_op() {
            return op<scheduler_t>(scheduler, ip, port);
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
        && (get_current_op_type_t<pos, scope_t>::kcp_sender_connect_op)
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
    compute_sender_type<context_t, scheduler::kcp::senders::connect::sender<scheduler_t>> {
        consteval static uint32_t
        count_value() {
            return 1;
        }

        consteval static auto
        get_value_type_identity() {
            return std::type_identity<typename scheduler_t::address_type>{};
        }
    };
}

#endif //ENV_SCHEDULER_KCP_SENDERS_CONNECT_HH
