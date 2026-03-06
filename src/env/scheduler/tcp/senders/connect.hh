#ifndef ENV_SCHEDULER_TCP_SENDERS_CONNECT_HH
#define ENV_SCHEDULER_TCP_SENDERS_CONNECT_HH
#include <cstdint>
#include <bits/move_only_function.h>

#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"

#include "../common/struct.hh"

namespace pump::scheduler::tcp::senders::connect {

    template <typename scheduler_t>
    struct
    op {
        using request_t = common::connect_req;
        constexpr static bool net_sender_connect_op = true;
        scheduler_t* scheduler;
        const char* address;
        uint16_t port;

        op(scheduler_t* s, const char* addr, uint16_t p)
            : scheduler(s), address(addr), port(p) {
        }

        op(op&& rhs) noexcept
            : scheduler(rhs.scheduler), address(rhs.address), port(rhs.port) {
        }

        template<uint32_t pos, typename context_t, typename scope_t>
        auto
        start(context_t &context, scope_t &scope) {
            return scheduler->schedule(
                new common::connect_req{
                    address, port,
                    [context = context, scope = scope](common::session_id_t sid) mutable {
                        core::op_pusher<pos + 1, scope_t>::push_value(context, scope, sid);
                    }
                }
            );
        }
    };

    template<typename scheduler_t>
    struct
    sender {
        scheduler_t* scheduler;
        const char* address;
        uint16_t port;

        sender(scheduler_t* s, const char* addr, uint16_t p)
            : scheduler(s), address(addr), port(p) {
        }

        sender(sender &&rhs) noexcept
            : scheduler(rhs.scheduler), address(rhs.address), port(rhs.port) {
        }

        auto
        make_op(){
            return op<scheduler_t>(scheduler, address, port);
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
    && (get_current_op_type_t<pos, scope_t>::net_sender_connect_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename context_t>
        static void
        push_value(context_t &context, scope_t &scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(context, scope);
        }
    };

    template <typename context_t, typename scheduler_t>
    struct
    compute_sender_type<context_t, scheduler::tcp::senders::connect::sender<scheduler_t>> {
        consteval static uint32_t
        count_value() {
            return 1;
        }

        consteval static auto
        get_value_type_identity() {
            return std::type_identity<scheduler::tcp::common::session_id_t>{};
        }
    };
}

#endif //ENV_SCHEDULER_TCP_SENDERS_CONNECT_HH
