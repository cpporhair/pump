
#ifndef ENV_SCHEDULER_RPC_SENDERS_SEND_HH
#define ENV_SCHEDULER_RPC_SENDERS_SEND_HH

#include <cstdint>
#include <cstring>
#include <array>
#include <bits/types/struct_iovec.h>

#include "pump/core/meta.hh"
#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"

#include "env/scheduler/net/common/struct.hh"
#include "../common/struct.hh"
#include "../service/service_type.hh"
#include "../service/service.hh"

namespace pump::scheduler::rpc::senders::send {

    // rpc::send sender（plan.md §五, F-API-2）
    // 单向发送：编码消息 + 写入 rpc_header + net::send
    template <service::service_type st, typename scheduler_t, typename msg_t>
    struct
    op {
        constexpr static bool rpc_sender_send_op = true;
        scheduler_t* scheduler;
        net::common::session_id_t session_id;
        msg_t msg;

        // 编码缓冲区（D-10）
        common::rpc_header header;
        std::array<iovec, 8> payload_iov;
        // 最终 iovec：header + payload 段
        std::array<iovec, 9> final_iov;

        op(scheduler_t* s, net::common::session_id_t sid, msg_t&& m)
            : scheduler(s)
            , session_id(sid)
            , msg(__fwd__(m))
            , header{}
            , payload_iov{}
            , final_iov{} {
        }

        op(op&& rhs) noexcept
            : scheduler(rhs.scheduler)
            , session_id(rhs.session_id)
            , msg(__mov__(rhs.msg))
            , header(rhs.header)
            , payload_iov(rhs.payload_iov)
            , final_iov(rhs.final_iov) {
        }

        template<uint32_t pos, typename context_t, typename scope_t>
        auto
        start(context_t& context, scope_t& scope) {
            using svc_t = service::service<st>;

            // 编码 payload
            auto cnt = svc_t::encode(msg, payload_iov);

            // 计算 payload 总长度
            size_t payload_total = 0;
            for (size_t i = 0; i < cnt; ++i)
                payload_total += payload_iov[i].iov_len;

            // 填充 rpc_header
            header.total_len = static_cast<uint32_t>(sizeof(common::rpc_header) + payload_total);
            header.request_id = 0;  // send 不需要 request_id
            header.module_id = static_cast<uint16_t>(st);
            header.msg_type = 0;    // 由编码器决定，或由上层设置
            header.flags = static_cast<uint8_t>(common::rpc_flags::push);

            // 组装 final_iov: header + payload 段
            final_iov[0] = {&header, sizeof(common::rpc_header)};
            for (size_t i = 0; i < cnt; ++i)
                final_iov[i + 1] = payload_iov[i];

            auto total_cnt = cnt + 1;

            return scheduler->schedule(
                new net::common::send_req{
                    session_id,
                    final_iov.data(),
                    total_cnt,
                    [context = context, scope = scope](bool succeed) mutable {
                        if (succeed) {
                            core::op_pusher<pos + 1, scope_t>::push_value(context, scope, succeed);
                        } else {
                            core::op_pusher<pos + 1, scope_t>::push_exception(
                                context, scope,
                                std::make_exception_ptr(common::rpc_error("rpc send failed")));
                        }
                    }
                }
            );
        }
    };

    template <service::service_type st, typename scheduler_t, typename msg_t>
    struct
    sender {
        scheduler_t* scheduler;
        net::common::session_id_t session_id;
        msg_t msg;

        sender(scheduler_t* s, net::common::session_id_t sid, msg_t&& m)
            : scheduler(s)
            , session_id(sid)
            , msg(__fwd__(m)) {
        }

        sender(sender&& rhs) noexcept
            : scheduler(rhs.scheduler)
            , session_id(rhs.session_id)
            , msg(__mov__(rhs.msg)) {
        }

        inline
        auto
        make_op() {
            return op<st, scheduler_t, msg_t>(scheduler, session_id, __mov__(msg));
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
    && (get_current_op_type_t<pos, scope_t>::rpc_sender_send_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename context_t>
        static void
        push_value(context_t& context, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(context, scope);
        }
    };

    template <typename context_t, auto st, typename scheduler_t, typename msg_t>
    struct
    compute_sender_type<context_t, scheduler::rpc::senders::send::sender<st, scheduler_t, msg_t>> {
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

#endif //ENV_SCHEDULER_RPC_SENDERS_SEND_HH
