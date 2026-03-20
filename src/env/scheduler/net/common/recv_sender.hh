
#ifndef ENV_COMMON_RECV_SENDER_HH
#define ENV_COMMON_RECV_SENDER_HH

#include <cstdint>

#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"
#include "env/scheduler/net/common/frame.hh"
#include "env/scheduler/net/common/frame_receiver.hh"
#include "env/scheduler/net/common/session_tags.hh"

namespace pump::scheduler::net::senders::recv {

    template <typename session_t>
    struct
    op {
        constexpr static bool common_recv_op = true;
        session_t* session;

        op(session_t* sess) : session(sess) {}
        op(op&& rhs) noexcept : session(rhs.session) {}

        template<uint32_t pos, typename context_t, typename scope_t>
        auto
        start(context_t& context, scope_t& scope) {
            return session->invoke(
                do_recv,
                new frame_recv_req{
                    [context = context, scope = scope](net_frame&& frame) mutable {
                        core::op_pusher<pos + 1, scope_t>::push_value(context, scope, std::move(frame));
                    },
                    [context = context, scope = scope](std::exception_ptr ex) mutable {
                        core::op_pusher<pos + 1, scope_t>::push_exception(context, scope, ex);
                    }
                }
            );
        }
    };

    template <typename session_t>
    struct
    sender {
        session_t* session;

        sender(session_t* sess) : session(sess) {}
        sender(sender&& rhs) noexcept : session(rhs.session) {}

        inline auto
        make_op() {
            return op<session_t>(session);
        }

        template <typename context_t>
        auto
        connect() {
            return core::builder::op_list_builder<0>().push_back(make_op());
        }
    };

}

namespace pump::scheduler::net {
    template <typename session_t>
    inline auto
    recv(session_t* session) {
        return senders::recv::sender<session_t>(session);
    }
}

namespace pump::core {

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::common_recv_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename context_t>
        static void
        push_value(context_t& context, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(context, scope);
        }
    };

    template <typename context_t, typename session_t>
    struct
    compute_sender_type<context_t, scheduler::net::senders::recv::sender<session_t>> {
        consteval static uint32_t
        count_value() {
            return 1;
        }

        consteval static auto
        get_value_type_identity() {
            return std::type_identity<scheduler::net::net_frame>{};
        }
    };
}

#endif //ENV_COMMON_RECV_SENDER_HH
