
#ifndef ENV_COMMON_SEND_SENDER_HH
#define ENV_COMMON_SEND_SENDER_HH

#include <cstdint>

#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"
#include "env/scheduler/net/common/session_tags.hh"
#include "env/scheduler/net/common/frame_receiver.hh"

namespace pump::scheduler::net::senders::send {

    template <typename session_t>
    struct
    op {
        constexpr static bool common_send_op = true;
        session_t* session;
        void* data;
        uint32_t len;

        op(session_t* s, void* d, uint32_t l)
            : session(s), data(d), len(l) {}

        op(op&& rhs) noexcept
            : session(rhs.session), data(rhs.data), len(rhs.len)
        {
            rhs.data = nullptr;
            rhs.len = 0;
        }

        template<uint32_t pos, typename context_t, typename scope_t>
        auto
        start(context_t& context, scope_t& scope) {
            auto* req = new frame_send_req{
                data, len,
                [context = context, scope = scope](bool ok) mutable {
                    core::op_pusher<pos + 1, scope_t>::push_value(
                        context, scope, ok
                    );
                }
            };
            data = nullptr;
            len = 0;
            session->invoke(do_send, req);
        }
    };

    template <typename session_t>
    struct
    sender {
        session_t* session;
        void* data;
        uint32_t len;

        sender(session_t* s, void* d, uint32_t l)
            : session(s), data(d), len(l) {}

        sender(sender&& rhs) noexcept
            : session(rhs.session), data(rhs.data), len(rhs.len)
        {
            rhs.data = nullptr;
            rhs.len = 0;
        }

        inline auto
        make_op() {
            auto* d = data;
            auto l = len;
            data = nullptr;
            len = 0;
            return op<session_t>(session, d, l);
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
    send(session_t* session, void* data, uint32_t len) {
        return senders::send::sender<session_t>(session, data, len);
    }
}

namespace pump::core {

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::common_send_op)
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
    compute_sender_type<context_t, scheduler::net::senders::send::sender<session_t>> {
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

#endif //ENV_COMMON_SEND_SENDER_HH
