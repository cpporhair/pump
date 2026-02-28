
#ifndef PUMP_ENV_SCHEDULER_RPC_CLIENT_CALL_HH
#define PUMP_ENV_SCHEDULER_RPC_CLIENT_CALL_HH

#include "trigger.hh"
#include "env/scheduler/net/net.hh"
#include "env/scheduler/net/common/struct.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/pop_context.hh"
#include "../common/struct.hh"
#include "../common/service.hh"


namespace pump::scheduler::rpc::client {
    namespace detail {
        template <typename scheduler_t>
        auto
        make_call_context(
            scheduler_t *sche,
            net::common::session_id_t ssid
        ) {
            return server::call_runtime_context<scheduler_t>(sche, ssid, server::request_id().value);
        }

        template <bool mine>
        struct
        recv_res {
            net::common::net_frame frame;
        };
    }

    template <server::uint16_enum_concept auto service_id, typename scheduler_t, typename ...args_t>
    auto
    send_req(args_t&& ...args) {
        using ctx_t = server::call_runtime_context<scheduler_t>;
        return sender::get_context<ctx_t>()
            >> sender::flat_map([...args = __fwd__(args)](ctx_t &ctx) mutable {
                server::service<service_id>::req_to_pkt(ctx.req, __fwd__(args)...);
                ctx.req.frame->header.service_id = static_cast<uint16_t>(service_id);
                ctx.req.frame->header.request_id = ctx.request_id;
                ctx.req.frame->header.flags = static_cast<uint08_t>(server::rpc_flags::request);
                return net::send<scheduler_t>(ctx.scheduler, ctx.sid, ctx.req.frame, ctx.req.get_len());
            });
    }

    template <server::uint16_enum_concept auto service_id, typename scheduler_t>
    auto
    wait_res() {
        using ctx_t = server::call_runtime_context<scheduler_t>;
        return sender::get_context<ctx_t>()
            >> sender::flat_map([](ctx_t &ctx, bool ok) {
                return net::recv(ctx.scheduler, ctx.sid)
                    >> sender::then([&ctx](net::common::net_frame &&frame) {
                        if (reinterpret_cast<server::rpc_frame *>(frame._data)->header.request_id == ctx.request_id)
                            return std::variant<detail::recv_res<true>, detail::recv_res<false> >(
                                detail::recv_res<true>(__fwd__(frame))
                            );
                        else
                            return std::variant<detail::recv_res<true>, detail::recv_res<false> >(
                                detail::recv_res<false>(__fwd__(frame))
                            );
                    })
                    >> sender::visit()
                    >> sender::flat_map([&ctx](auto &&res) {
                        static thread_local detail::trigger trigger(2048);
                        if constexpr (std::is_same_v<__typ__(res), detail::recv_res<true> >) {
                            return sender::just() >> sender::forward_value(__mov__(res.frame));
                        } else {
                            auto *rpc_frame = reinterpret_cast<server::rpc_frame *>(res.frame._data);
                            trigger.on_response(rpc_frame->header.request_id, __mov__(res.frame));
                            return trigger.wait_response(ctx.request_id);
                        }
                    })
                    >> sender::then([&ctx](net::common::net_frame &&frame) {
                        ctx.res.frame = reinterpret_cast<server::rpc_frame *>(frame._data);
                        return server::service<service_id>::pkt_to_res(ctx.res);
                    });
            });
    }

    template <server::uint16_enum_concept auto service_id, typename scheduler_t, typename ...args_t>
    auto
    call(scheduler_t* sche, net::common::session_id_t sid, args_t&& ...args) {
        using ctx_t = server::call_runtime_context<scheduler_t>;
        return sender::with_context(detail::make_call_context(sche,sid))([...args = __fwd__(args)]() mutable {
            return send_req<service_id, scheduler_t, args_t...>(__fwd__(args)...)
                >> wait_res<service_id, scheduler_t>();
        });
    }
}

#endif //PUMP_ENV_SCHEDULER_RPC_CLIENT_CALL_HH