
#ifndef ENV_SCHEDULER_RPC_SENDERS_CALL_HH
#define ENV_SCHEDULER_RPC_SENDERS_CALL_HH

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

namespace pump::scheduler::rpc::senders::call {

    // rpc::call sender（plan.md §七, F-API-1）
    // 发送请求并异步等待对应的响应
    // 流程：encode → write rpc_header → net::send → register pending → net::recv loop → match response
    template <service::service_type st, typename scheduler_t, typename req_t>
    struct
    op {
        constexpr static bool rpc_sender_call_op = true;
        scheduler_t* scheduler;
        net::common::session_id_t session_id;
        req_t request;

        // 编码缓冲区（D-10）
        common::rpc_header header;
        std::array<iovec, 8> payload_iov;
        std::array<iovec, 9> final_iov;

        op(scheduler_t* s, net::common::session_id_t sid, req_t&& r)
            : scheduler(s)
            , session_id(sid)
            , request(__fwd__(r))
            , header{}
            , payload_iov{}
            , final_iov{} {
        }

        op(op&& rhs) noexcept
            : scheduler(rhs.scheduler)
            , session_id(rhs.session_id)
            , request(__mov__(rhs.request))
            , header(rhs.header)
            , payload_iov(rhs.payload_iov)
            , final_iov(rhs.final_iov) {
        }

        template<uint32_t pos, typename context_t, typename scope_t>
        auto
        start(context_t& context, scope_t& scope) {
            using svc_t = service::service<st>;
            auto& registry = state::get_registry();
            auto& rpc_state = registry.get_or_bind(session_id);

            // 分配 request_id（D-1）
            auto rid = rpc_state.next_request_id++;

            // 编码 payload
            auto cnt = svc_t::encode(request, payload_iov);

            // 计算 payload 总长度
            size_t payload_total = 0;
            for (size_t i = 0; i < cnt; ++i)
                payload_total += payload_iov[i].iov_len;

            // 填充 rpc_header
            header.total_len = static_cast<uint32_t>(sizeof(common::rpc_header) + payload_total);
            header.request_id = rid;
            header.module_id = static_cast<uint16_t>(st);
            header.msg_type = 0;
            header.flags = static_cast<uint8_t>(common::rpc_flags::request);

            // 组装 final_iov
            final_iov[0] = {&header, sizeof(common::rpc_header)};
            for (size_t i = 0; i < cnt; ++i)
                final_iov[i + 1] = payload_iov[i];

            auto total_cnt = cnt + 1;

            // 在 pending_map 中注册回调（步骤 5）
            // 回调收到 recv_frame 后解码响应并 push_value
            bool inserted = rpc_state.pending.insert(rid,
                [context, scope](net::common::recv_frame&& frame) mutable {
                    // 解析 rpc_header
                    if (frame.size() < sizeof(common::rpc_header)) {
                        core::op_pusher<pos + 1, scope_t>::push_exception(
                            context, scope,
                            std::make_exception_ptr(
                                common::frame_error("response frame too small")));
                        return;
                    }

                    auto* hdr = frame.as<common::rpc_header>();
                    auto payload = common::payload_ptr(hdr);
                    auto plen = common::payload_len(hdr);

                    try {
                        using svc_t2 = service::service<st>;
                        auto msg = svc_t2::decode(hdr->msg_type, payload, plen);
                        core::op_pusher<pos + 1, scope_t>::push_value(
                            context, scope, __mov__(msg));
                    } catch (...) {
                        core::op_pusher<pos + 1, scope_t>::push_exception(
                            context, scope, std::current_exception());
                    }
                }
            );

            if (!inserted) {
                core::op_pusher<pos + 1, scope_t>::push_exception(
                    context, scope,
                    std::make_exception_ptr(common::pending_full_error{}));
                return;
            }

            // 发送请求（步骤 4）
            // send 完成后注册 recv 回调进行响应分发（步骤 6-7）
            scheduler->schedule(
                new net::common::send_req{
                    session_id,
                    final_iov.data(),
                    total_cnt,
                    [this, context = context, scope = scope, rid](bool succeed) mutable {
                        if (!succeed) {
                            // 发送失败，移除 pending 并通知
                            auto& reg = state::get_registry();
                            auto& st2 = reg.get_or_bind(session_id);
                            st2.pending.extract(rid);
                            core::op_pusher<pos + 1, scope_t>::push_exception(
                                context, scope,
                                std::make_exception_ptr(
                                    common::rpc_error("rpc call send failed")));
                            return;
                        }
                        // 发送成功，注册 recv 等待响应
                        start_recv<pos>(context, scope, rid);
                    }
                }
            );
        }

        // recv 循环：接收帧 → 通过 pending_map 分发 → 如果自己的响应未到继续 recv
        template<uint32_t pos, typename context_t, typename scope_t>
        void
        start_recv(context_t& context, scope_t& scope, uint32_t my_rid) {
            scheduler->schedule(
                new net::common::recv_req{
                    session_id,
                    [this, context = context, scope = scope, my_rid](
                        std::variant<net::common::recv_frame, std::exception_ptr>&& res
                    ) mutable {
                        if (res.index() == 1) [[unlikely]] {
                            // 连接异常
                            auto& reg = state::get_registry();
                            auto& st2 = reg.get_or_bind(session_id);
                            st2.pending.extract(my_rid);
                            core::op_pusher<pos + 1, scope_t>::push_exception(
                                context, scope, std::get<1>(res));
                            return;
                        }

                        auto& frame = std::get<0>(res);

                        // 从帧头提取 request_id
                        if (frame.size() < sizeof(common::rpc_header)) {
                            // 帧格式错误，继续等待
                            start_recv<pos>(context, scope, my_rid);
                            return;
                        }

                        auto* hdr = frame.as<common::rpc_header>();
                        auto frame_rid = hdr->request_id;

                        // 在 pending_map 中查找并触发回调（plan.md §7.2）
                        auto& reg = state::get_registry();
                        auto& st2 = reg.get_or_bind(session_id);
                        if (auto cb = st2.pending.extract(frame_rid)) {
                            (*cb)(std::move(std::get<0>(res)));
                        }

                        // 如果自己的响应尚未到达，继续 recv
                        if (st2.pending.contains(my_rid)) {
                            start_recv<pos>(context, scope, my_rid);
                        }
                        // 否则 my_rid 的回调已在上面被触发，流程完成
                    }
                }
            );
        }
    };

    template <service::service_type st, typename scheduler_t, typename req_t>
    struct
    sender {
        scheduler_t* scheduler;
        net::common::session_id_t session_id;
        req_t request;

        sender(scheduler_t* s, net::common::session_id_t sid, req_t&& r)
            : scheduler(s)
            , session_id(sid)
            , request(__fwd__(r)) {
        }

        sender(sender&& rhs) noexcept
            : scheduler(rhs.scheduler)
            , session_id(rhs.session_id)
            , request(__mov__(rhs.request)) {
        }

        inline
        auto
        make_op() {
            return op<st, scheduler_t, req_t>(scheduler, session_id, __mov__(request));
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
    && (get_current_op_type_t<pos, scope_t>::rpc_sender_call_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename context_t>
        static void
        push_value(context_t& context, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(context, scope);
        }
    };

    template <typename context_t, auto st, typename scheduler_t, typename req_t>
    struct
    compute_sender_type<context_t, scheduler::rpc::senders::call::sender<st, scheduler_t, req_t>> {
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

#endif //ENV_SCHEDULER_RPC_SENDERS_CALL_HH
