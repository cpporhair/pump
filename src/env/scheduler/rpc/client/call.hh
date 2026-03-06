
#ifndef PUMP_ENV_SCHEDULER_RPC_CLIENT_CALL_HH
#define PUMP_ENV_SCHEDULER_RPC_CLIENT_CALL_HH

#include "trigger.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/pop_context.hh"
#include "../common/struct.hh"
#include "../common/service.hh"


namespace pump::scheduler::rpc::client {
    namespace detail {
        template <typename transport_t, typename scheduler_t>
        auto
        make_call_context(
            scheduler_t *sche,
            typename transport_t::address_type addr
        ) {
            return call_runtime_context<transport_t, scheduler_t>(sche, addr, request_id().value);
        }

        template <bool mine>
        struct
        recv_res {
            pump::common::net_frame frame;
        };
    }

    template <typename transport_t, uint16_enum_concept auto service_id, typename scheduler_t, typename ...args_t>
    auto
    send_req(args_t&& ...args) {
        using ctx_t = call_runtime_context<transport_t, scheduler_t>;
        return sender::get_context<ctx_t>()
            >> sender::flat_map([...args = __fwd__(args)](ctx_t &ctx) mutable {
                service<service_id>::req_to_pkt(ctx.req, __fwd__(args)...);
                ctx.req.frame->header.service_id = static_cast<uint16_t>(service_id);
                ctx.req.frame->header.request_id = ctx.request_id;
                ctx.req.frame->header.flags = static_cast<uint08_t>(rpc_flags::request);
                auto len = ctx.req.get_len();
                auto* f = ctx.req.frame;
                ctx.req.frame = nullptr;
                return transport_t::send(ctx.scheduler, ctx.address, f, len);
            });
    }

    inline uint64_t
    get_request_id_from(pump::common::net_frame& frame) {
        return reinterpret_cast<rpc_frame *>(frame._data)->header.request_id;
    }

    template <typename transport_t, uint16_enum_concept auto service_id, typename scheduler_t>
    auto
    wait_res() {
        using ctx_t = call_runtime_context<transport_t, scheduler_t>;
        return sender::get_context<ctx_t>()
            >> sender::flat_map([](ctx_t &ctx, bool ok) {
                if (!ok)
                    throw std::runtime_error("rpc send failed");
                return transport_t::recv(ctx.scheduler, ctx.address)
                    >> sender::then([&ctx](pump::common::net_frame &&frame) {
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
                            return trigger.wait_response(ctx.request_id, transport_t::address_raw(ctx.address));
                        }
                    })
                    >> sender::then([&ctx](pump::common::net_frame &&frame) {
                        ctx.res.frame = reinterpret_cast<rpc_frame *>(frame.release());
                        if (ctx.res.frame->header.flags == static_cast<uint08_t>(rpc_flags::error)) {
                            auto code = *reinterpret_cast<rpc_error_code*>(ctx.res.get_payload());
                            throw rpc_error(code);
                        }
                        return service<service_id>::pkt_to_res(ctx.res);
                    })
                    >> sender::any_exception([&ctx](std::exception_ptr e) {
                        static thread_local detail::trigger trigger(2048);
                        trigger.fail_session(transport_t::address_raw(ctx.address), e);
                        using res_t = decltype(service<service_id>::pkt_to_res(ctx.res));
                        return sender::just() >> sender::then([e]() -> res_t {
                            std::rethrow_exception(e);
                        });
                    });
            });
    }

    template <typename transport_t, uint16_enum_concept auto service_id, typename scheduler_t, typename ...args_t>
    auto
    call(scheduler_t* sche, typename transport_t::address_type addr, args_t&& ...args) {
        return sender::with_context(detail::make_call_context<transport_t>(sche, addr))([...args = __fwd__(args)]() mutable {
            return send_req<transport_t, service_id, scheduler_t, args_t...>(__fwd__(args)...)
                >> wait_res<transport_t, service_id, scheduler_t>();
        });
    }
}

#endif //PUMP_ENV_SCHEDULER_RPC_CLIENT_CALL_HH
