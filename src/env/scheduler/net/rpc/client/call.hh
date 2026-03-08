
#ifndef PUMP_ENV_SCHEDULER_RPC_CLIENT_CALL_HH
#define PUMP_ENV_SCHEDULER_RPC_CLIENT_CALL_HH

#include "trigger.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/pop_context.hh"
#include "../common/struct.hh"
#include "../common/service.hh"
#include "env/scheduler/net/common/send_sender.hh"
#include "env/scheduler/net/common/recv_sender.hh"


namespace pump::scheduler::rpc::client {
    namespace detail {
        template <typename session_t>
        auto
        make_call_context(session_t* session) {
            return call_runtime_context<session_t>(session, request_id().value);
        }

        template <bool mine>
        struct
        recv_res {
            pump::scheduler::net::net_frame frame;
        };
    }

    template <uint16_enum_concept auto service_id, typename session_t, typename ...args_t>
    auto
    send_req(args_t&& ...args) {
        using ctx_t = call_runtime_context<session_t>;
        return sender::get_context<ctx_t>()
            >> sender::flat_map([...args = __fwd__(args)](ctx_t &ctx) mutable {
                service<service_id>::req_to_pkt(ctx.req, __fwd__(args)...);
                ctx.req.frame->header.service_id = static_cast<uint16_t>(service_id);
                ctx.req.frame->header.request_id = ctx.request_id;
                ctx.req.frame->header.flags = static_cast<uint08_t>(rpc_flags::request);
                auto len = ctx.req.get_len();
                auto* f = ctx.req.frame;
                ctx.req.frame = nullptr;
                return pump::scheduler::net::send(ctx.session, f, len);
            });
    }

    inline uint64_t
    get_request_id_from(pump::scheduler::net::net_frame& frame) {
        return reinterpret_cast<rpc_frame *>(frame._data)->header.request_id;
    }

    template <uint16_enum_concept auto service_id, typename session_t>
    auto
    wait_res() {
        using ctx_t = call_runtime_context<session_t>;
        return sender::get_context<ctx_t>()
            >> sender::flat_map([](ctx_t &ctx, bool ok) {
                if (!ok)
                    throw std::runtime_error("rpc send failed");
                return pump::scheduler::net::recv(ctx.session)
                    >> sender::then([&ctx](pump::scheduler::net::net_frame &&frame) {
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
                            return trigger.wait_response(ctx.request_id, reinterpret_cast<uint64_t>(ctx.session));
                        }
                    })
                    >> sender::then([&ctx](pump::scheduler::net::net_frame &&frame) {
                        ctx.res.frame = reinterpret_cast<rpc_frame *>(frame.release());
                        if (ctx.res.frame->header.flags == static_cast<uint08_t>(rpc_flags::error)) {
                            auto code = *reinterpret_cast<rpc_error_code*>(ctx.res.get_payload());
                            throw rpc_error(code);
                        }
                        return service<service_id>::pkt_to_res(ctx.res);
                    })
                    >> sender::any_exception([&ctx](std::exception_ptr e) {
                        static thread_local detail::trigger trigger(2048);
                        trigger.fail_session(reinterpret_cast<uint64_t>(ctx.session), e);
                        using res_t = decltype(service<service_id>::pkt_to_res(ctx.res));
                        return sender::just() >> sender::then([e]() -> res_t {
                            std::rethrow_exception(e);
                        });
                    });
            });
    }

    template <uint16_enum_concept auto service_id, typename session_t, typename ...args_t>
    auto
    call(session_t* session, args_t&& ...args) {
        return sender::with_context(detail::make_call_context(session))([...args = __fwd__(args)]() mutable {
            return send_req<service_id, session_t, args_t...>(__fwd__(args)...)
                >> wait_res<service_id, session_t>();
        });
    }
}

#endif //PUMP_ENV_SCHEDULER_RPC_CLIENT_CALL_HH
