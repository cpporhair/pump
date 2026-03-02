
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
            return call_runtime_context<scheduler_t>(sche, ssid, request_id().value);
        }

        template <bool mine>
        struct
        recv_res {
            net::common::net_frame frame;
        };
    }

    template <uint16_enum_concept auto service_id, typename scheduler_t, typename ...args_t>
    auto
    send_req(args_t&& ...args) {
        using ctx_t = call_runtime_context<scheduler_t>;
        return sender::get_context<ctx_t>()
            >> sender::flat_map([...args = __fwd__(args)](ctx_t &ctx) mutable {
                service<service_id>::req_to_pkt(ctx.req, __fwd__(args)...);
                ctx.req.frame->header.service_id = static_cast<uint16_t>(service_id);
                ctx.req.frame->header.request_id = ctx.request_id;
                ctx.req.frame->header.flags = static_cast<uint08_t>(rpc_flags::request);
                auto len = ctx.req.get_len();
                auto* f = ctx.req.frame;
                ctx.req.frame = nullptr;
                return net::send<scheduler_t>(ctx.scheduler, ctx.sid, f, len);
            });
    }

    inline uint64_t
    get_request_id_from(net::common::net_frame& frame) {
        return reinterpret_cast<rpc_frame *>(frame._data)->header.request_id;
    }

    template <uint16_enum_concept auto service_id, typename scheduler_t>
    auto
    wait_res() {
        using ctx_t = call_runtime_context<scheduler_t>;
        return sender::get_context<ctx_t>()
            >> sender::flat_map([](ctx_t &ctx, bool ok) {
                if (!ok)
                    throw std::runtime_error("rpc send failed");
                return net::recv(ctx.scheduler, ctx.sid)
                    >> sender::then([&ctx](net::common::net_frame &&frame) {
                        if (get_request_id_from(frame) == ctx.request_id)
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
                            trigger.on_response(get_request_id_from(res.frame), __mov__(res.frame));
                            return trigger.wait_response(ctx.request_id, ctx.sid._value);
                        }
                    })
                    >> sender::then([&ctx](net::common::net_frame &&frame) {
                        ctx.res.frame = reinterpret_cast<rpc_frame *>(frame.release());
                        if (ctx.res.frame->header.flags == static_cast<uint08_t>(rpc_flags::error)) {
                            auto code = *reinterpret_cast<rpc_error_code*>(ctx.res.get_payload());
                            throw rpc_error(code);
                        }
                        return service<service_id>::pkt_to_res(ctx.res);
                    })
                    >> sender::any_exception([&ctx](std::exception_ptr e) {
                        static thread_local detail::trigger trigger(2048);
                        trigger.fail_session(ctx.sid._value, e);
                        using res_t = decltype(service<service_id>::pkt_to_res(ctx.res));
                        return sender::just() >> sender::then([e]() -> res_t {
                            std::rethrow_exception(e);
                        });
                    });
            });
    }

    template <uint16_enum_concept auto service_id, typename scheduler_t, typename ...args_t>
    auto
    call(scheduler_t* sche, net::common::session_id_t sid, args_t&& ...args) {
        using ctx_t = call_runtime_context<scheduler_t>;
        return sender::with_context(detail::make_call_context(sche,sid))([...args = __fwd__(args)]() mutable {
            return send_req<service_id, scheduler_t, args_t...>(__fwd__(args)...)
                >> wait_res<service_id, scheduler_t>();
        });
    }
}

#endif //PUMP_ENV_SCHEDULER_RPC_CLIENT_CALL_HH
