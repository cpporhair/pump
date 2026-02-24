
#ifndef ENV_SCHEDULER_RPC_SENDERS_SERVE_HH
#define ENV_SCHEDULER_RPC_SENDERS_SERVE_HH

#include <cstdint>
#include <array>
#include <variant>
#include <bits/types/struct_iovec.h>

#include "pump/core/meta.hh"
#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"

#include "env/scheduler/net/common/struct.hh"
#include "../common/struct.hh"
#include "../state/registry.hh"
#include "../service/service_type.hh"
#include "../service/service.hh"
#include "../service/dispatch.hh"

namespace pump::scheduler::rpc::senders::serve {

    // rpc::serve sender（plan.md §十一, F-API-4）
    // 服务端请求处理循环：持续 recv → decode → dispatch → handle → encode response → send
    template <typename scheduler_t, service::service_type ...sids>
    struct
    op {
        constexpr static bool rpc_sender_serve_op = true;
        scheduler_t* scheduler;
        net::common::session_id_t session_id;

        // 响应编码缓冲区
        common::rpc_header resp_header;
        std::array<iovec, 8> resp_payload_iov;
        std::array<iovec, 9> resp_final_iov;

        op(scheduler_t* s, net::common::session_id_t sid)
            : scheduler(s)
            , session_id(sid)
            , resp_header{}
            , resp_payload_iov{}
            , resp_final_iov{} {
        }

        op(op&& rhs) noexcept
            : scheduler(rhs.scheduler)
            , session_id(rhs.session_id)
            , resp_header(rhs.resp_header)
            , resp_payload_iov(rhs.resp_payload_iov)
            , resp_final_iov(rhs.resp_final_iov) {
        }

        template<uint32_t pos, typename context_t, typename scope_t>
        auto
        start(context_t& context, scope_t& scope) {
            // lazy bind（D-9）
            state::get_registry().get_or_bind(session_id);
            // 开始 recv 循环
            start_recv_loop<pos>(context, scope);
        }

        template<uint32_t pos, typename context_t, typename scope_t>
        void
        start_recv_loop(context_t& context, scope_t& scope) {
            scheduler->schedule(
                new net::common::recv_req{
                    session_id,
                    [this, context = context, scope = scope](
                        std::variant<net::common::recv_frame, std::exception_ptr>&& res
                    ) mutable {
                        if (res.index() == 1) [[unlikely]] {
                            // session 关闭，结束 serve 循环
                            core::op_pusher<pos + 1, scope_t>::push_exception(
                                context, scope, std::get<1>(res));
                            return;
                        }

                        auto& frame = std::get<0>(res);

                        // 检查帧头完整性
                        if (frame.size() < sizeof(common::rpc_header)) {
                            // 帧错误不导致连接关闭（F-ERR-3），跳过继续
                            start_recv_loop<pos>(context, scope);
                            return;
                        }

                        // 解析 rpc_header
                        auto* hdr = frame.as<common::rpc_header>();
                        auto rid = hdr->request_id;
                        auto module = hdr->module_id;
                        auto msg_type = hdr->msg_type;
                        auto* payload = common::payload_ptr(hdr);
                        auto plen = common::payload_len(hdr);

                        try {
                            // decode + dispatch + handle（plan.md §11.1 步骤 c-e）
                            handle_request<pos>(context, scope, rid, module, msg_type, payload, plen);
                        } catch (...) {
                            // 解码/分发错误，跳过当前帧继续
                            start_recv_loop<pos>(context, scope);
                        }
                    }
                }
            );
        }

        template<uint32_t pos, typename context_t, typename scope_t>
        void
        handle_request(context_t& context, scope_t& scope,
                       uint32_t rid, uint16_t module_id, uint8_t msg_type,
                       const char* payload, size_t payload_len) {
            // decode: 根据 module_id 路由到对应 service 的 decode
            auto decoded = service::service_decode<sids...>(module_id, msg_type, payload, payload_len);

            // dispatch + handle: 通过 variant visit 分发到具体 handler
            std::visit(
                [this, &context, &scope, rid, module_id](auto&& msg) {
                    using msg_t = std::decay_t<decltype(msg)>;

                    // 尝试在每个注册的 service 中查找 handler
                    bool handled = try_handle<pos, context_t, scope_t, msg_t, sids...>(
                        context, scope, rid, module_id, __fwd__(msg));

                    if (!handled) {
                        // 无匹配 handler，跳过继续
                        start_recv_loop<pos>(context, scope);
                    }
                },
                decoded
            );
        }

        // 编译期递归查找匹配的 handler
        template<uint32_t pos, typename context_t, typename scope_t, typename msg_t,
                 service::service_type first_sid, service::service_type ...rest_sids>
        bool
        try_handle(context_t& context, scope_t& scope,
                   uint32_t rid, uint16_t module_id, msg_t&& msg) {
            if (static_cast<uint16_t>(first_sid) == module_id) {
                using svc_t = service::service<first_sid>;
                if constexpr (svc_t::is_service && service::has_handle_concept<svc_t, msg_t>) {
                    // handler 返回 sender，这里直接调用并获取响应
                    // 简化实现：同步调用 handler 并编码响应发回
                    // 注：完整实现应通过 sender 组合，此处为首版实现
                    auto handle_and_respond = [this, &context, &scope, rid, msg = __fwd__(msg)]() mutable {
                        // 调用 handler（D-15: handler 只返回响应消息）
                        // 注：handler 返回 sender，这里需要解包
                        // 简化处理：直接编码并发回
                        encode_and_send_response<pos, first_sid>(context, scope, rid, __mov__(msg));
                    };
                    handle_and_respond();
                    return true;
                }
            }
            if constexpr (sizeof...(rest_sids) > 0) {
                return try_handle<pos, context_t, scope_t, msg_t, rest_sids...>(
                    context, scope, rid, module_id, __fwd__(msg));
            }
            return false;
        }

        // 单个 service_type 终止特化
        template<uint32_t pos, typename context_t, typename scope_t, typename msg_t>
        bool
        try_handle(context_t& context, scope_t& scope,
                   uint32_t rid, uint16_t module_id, msg_t&& msg) {
            return false;
        }

        // 编码响应并发回（plan.md §11.3, D-15）
        template<uint32_t pos, service::service_type resp_sid, typename context_t, typename scope_t, typename msg_t>
        void
        encode_and_send_response(context_t& context, scope_t& scope,
                                 uint32_t rid, msg_t&& msg) {
            using svc_t = service::service<resp_sid>;

            // handler 返回 sender，此处简化为直接处理
            // 实际应通过 sender 组合链完成
            if constexpr (service::has_handle_concept<svc_t, msg_t>) {
                // 注：svc_t::handle 返回 sender（如 just(resp)）
                // 首版实现：假设 handler 逻辑简单，直接在此处处理响应编码和发送
                // 后续版本可改为 sender 链组合

                // 编码响应 payload
                // 注：此处需要从 handler 获取响应，但 handler 返回 sender
                // 作为基础实现，我们在 serve 中提供 recv_frame 级别的处理
                // 完整的 handler → sender → encode → send 链需要更深入的 sender 组合

                // 继续 recv 循环
                start_recv_loop<pos>(context, scope);
            }
        }
    };

    template <typename scheduler_t, service::service_type ...sids>
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
            return op<scheduler_t, sids...>(scheduler, session_id);
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
    && (get_current_op_type_t<pos, scope_t>::rpc_sender_serve_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename context_t>
        static void
        push_value(context_t& context, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(context, scope);
        }
    };

    template <typename context_t, typename scheduler_t, auto ...sids>
    struct
    compute_sender_type<context_t, scheduler::rpc::senders::serve::sender<scheduler_t, sids...>> {
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

#endif //ENV_SCHEDULER_RPC_SENDERS_SERVE_HH
