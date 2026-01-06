#ifndef ENV_SCHEDULER_NET_SENDERS_SEND_HH
#define ENV_SCHEDULER_NET_SENDERS_SEND_HH
#include <cstdint>
#include <bits/move_only_function.h>

#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"
#include "../common/struct.hh"

namespace pump::scheduler::net::senders::send {
    template <typename scheduler_t>
    struct
    op {
        constexpr static bool net_sender_send_op = true;
        scheduler_t* scheduler;
        uint64_t session_id;
        iovec* vec;
        int cnt;

        op(scheduler_t* s, uint64_t sid, iovec* vec, int cnt)
            : scheduler(s)
            , session_id(sid)
            , vec(vec)
            , cnt(cnt){
        }

        op(op &&rhs) noexcept
            : scheduler(rhs.scheduler)
            , session_id(rhs.session_id)
            , vec(rhs.vec)
            , cnt(rhs.cnt) {
        }



        template<uint32_t pos, typename context_t, typename scope_t>
        auto
        start(context_t &context, scope_t &scope) {
            return scheduler->schedule(
                new common::send_req{
                    session_id,
                    vec,
                    cnt,
                    [context = context, scope = scope](bool succeed) mutable {
                        core::op_pusher<pos + 1, scope_t>::push_value(
                            context,
                            scope,
                            succeed
                        );
                    }
                }
            );
        }
    };

    template<typename scheduler_t>
    struct
    sender {
        scheduler_t* scheduler;
        uint64_t session_id;
        iovec* vec;
        int cnt;

        sender(scheduler_t* s, uint64_t sid, iovec* vec, int cnt)
            : scheduler(s)
            , session_id(sid)
            , vec(vec)
            , cnt(cnt){
        }

        sender(sender &&rhs) noexcept
            : scheduler(rhs.scheduler)
            , session_id(rhs.session_id)
            , vec(rhs.vec)
            , cnt(rhs.cnt){
        }

        inline
        auto
        make_op(){
            return op<scheduler_t>(scheduler, session_id, vec, cnt);
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
    && (get_current_op_type_t<pos, scope_t>::net_sender_send_op)
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
    compute_sender_type<context_t, scheduler::net::senders::send::sender<scheduler_t>> {
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

#endif //ENV_SCHEDULER_NET_SENDERS_SEND_HH