
#ifndef ENV_SCHEDULER_RPC_SENDERS_RECV_HH
#define ENV_SCHEDULER_RPC_SENDERS_RECV_HH

#include <cstdint>
#include <variant>

#include "pump/core/meta.hh"
#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"

#include "env/scheduler/net/common/struct.hh"
#include "../common/struct.hh"
#include "../service/service_type.hh"
#include "../service/service.hh"

namespace pump::scheduler::rpc::senders::recv {

    // rpc::recv sender（plan.md §五, F-API-3）
    // 接收下一条消息：net::recv → 解析 rpc_header → decode payload → message_type
    template <service::service_type st, typename scheduler_t>
    struct
    op {
        constexpr static bool rpc_sender_recv_op = true;
        scheduler_t* scheduler;
        net::common::session_id_t session_id;

        op(scheduler_t* s, net::common::session_id_t sid)
            : scheduler(s)
            , session_id(sid) {
        }

        op(op&& rhs) noexcept
            : scheduler(rhs.scheduler)
            , session_id(rhs.session_id) {
        }

        template<uint32_t pos, typename context_t, typename scope_t>
        auto
        start(context_t& context, scope_t& scope) {
            return scheduler->schedule(
                new net::common::recv_req{
                    session_id,
                    [context = context, scope = scope](
                        std::variant<net::common::recv_frame, std::exception_ptr>&& res
                    ) mutable {
                        if (res.index() == 1) [[unlikely]] {
                            core::op_pusher<pos + 1, scope_t>::push_exception(
                                context, scope, std::get<1>(res));
                            return;
                        }

                        auto& frame = std::get<0>(res);

                        // 检查帧头完整性
                        if (frame.size() < sizeof(common::rpc_header)) {
                            core::op_pusher<pos + 1, scope_t>::push_exception(
                                context, scope,
                                std::make_exception_ptr(
                                    common::frame_error("frame too small for rpc_header")));
                            return;
                        }

                        // 解析 rpc_header
                        auto* hdr = frame.as<common::rpc_header>();
                        auto payload = common::payload_ptr(hdr);
                        auto plen = common::payload_len(hdr);

                        // 解码 payload
                        try {
                            using svc_t = service::service<st>;
                            auto msg = svc_t::decode(hdr->msg_type, payload, plen);
                            core::op_pusher<pos + 1, scope_t>::push_value(
                                context, scope, __mov__(msg));
                        } catch (...) {
                            core::op_pusher<pos + 1, scope_t>::push_exception(
                                context, scope, std::current_exception());
                        }
                    }
                }
            );
        }
    };

    template <service::service_type st, typename scheduler_t>
    struct
    sender {
        scheduler_t* scheduler;
        net::common::session_id_t session_id;

        sender(scheduler_t* s, net::common::session_id_t sid)
            : scheduler(s)
            , session_id(sid) {
        }

        sender(sender&& rhs) noexcept
            : scheduler(rhs.scheduler)
            , session_id(rhs.session_id) {
        }

        inline
        auto
        make_op() {
            return op<st, scheduler_t>(scheduler, session_id);
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
    && (get_current_op_type_t<pos, scope_t>::rpc_sender_recv_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename context_t>
        static void
        push_value(context_t& context, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(context, scope);
        }
    };

    template <typename context_t, auto st, typename scheduler_t>
    struct
    compute_sender_type<context_t, scheduler::rpc::senders::recv::sender<st, scheduler_t>> {
        consteval static uint32_t
        count_value() {
            return 1;
        }

        consteval static auto
        get_value_type_identity() {
            using svc_t = scheduler::rpc::service::service<st>;
            return std::type_identity<typename svc_t::message_type>{};
        }
    };
}

#endif //ENV_SCHEDULER_RPC_SENDERS_RECV_HH
