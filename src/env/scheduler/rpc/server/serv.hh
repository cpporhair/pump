
#ifndef PUMP_ENV_SCHEDULER_RPC_SERVER_DISPATCH_HH
#define PUMP_ENV_SCHEDULER_RPC_SERVER_DISPATCH_HH

#include "pump/sender/just.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/then.hh"
#include "env/scheduler/tcp/tcp.hh"
#include "env/scheduler/rpc/common/rpc_state.hh"
#include "pump/coro/coro.hh"
#include "pump/sender/for_each.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/pop_context.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/visit.hh"

#include "../common/struct.hh"
#include "../common/service.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/concurrent.hh"

namespace pump::scheduler::rpc::server {
    using namespace pump::sender;

    template <typename session_scheduler_t>
    struct
    session_state {
        session_scheduler_t* scheduler;
        tcp::common::session_id_t session_id;
        bool closed = false;

        [[nodiscard]] bool
        is_closed() const { return closed; }

        void
        close() { closed = true; }
    };

    template<uint16_enum_concept auto ...service_ids>
    auto
    dispatch() {
        static_assert(sizeof...(service_ids) > 0);
        using service_id_t = std::decay_t<decltype(std::get<0>(std::forward_as_tuple(service_ids...)))>;
        return get_context<serv_runtime_context>()
            >> flat_map([](serv_runtime_context& memo) {
                return just()
                    >> visit(get_service_class_by_id<service_ids...>(static_cast<service_id_t>(memo.req.get_service_id())))
                    >> flat_map([&memo]<typename T0>([[maybe_unused]] T0 &&result) mutable {
                        if constexpr (requires { std::decay_t<T0>::is_service; }) {
                            if constexpr (std::decay_t<T0>::is_service) {
                                return std::decay_t<T0>::handle(memo.req, memo.res)
                                    >> then([&memo]() {
                                        memo.res.frame->header.flags = static_cast<uint08_t>(
                                            rpc_flags::response);
                                        memo.res.frame->header.request_id = memo.req.frame->header.request_id;
                                        memo.res.frame->header.service_id = memo.req.frame->header.service_id;
                                    });
                            } else {
                                return just_exception(std::logic_error("unknown service type"));
                            }
                        } else {
                            return just_exception(std::logic_error("unknown service type"));
                        }
                    });
            });
    }

    template <typename session_state_t>
    auto
    check_rpc_state(const session_state_t& rs) -> coro::return_yields<bool> {
        while (!rs.is_closed())
            co_yield true;
        co_return false;
    }

    inline auto
    recv_req(const auto& st) {
        return flat_map([&st](...) {
            return tcp::recv(st.scheduler, st.session_id)
                >> get_context<serv_runtime_context>()
                >> then([](serv_runtime_context& memo, tcp::common::net_frame &&frame) {
                    memo.req.frame = reinterpret_cast<rpc_frame *>(frame.release());
                });
        });
    }

    inline auto
    send_res(const auto& st) {
        return get_context<serv_runtime_context>()
            >> flat_map([&st](serv_runtime_context& memo, auto ...should_no_args) {
                static_assert(sizeof...(should_no_args) == 0);
                auto len = memo.res.get_len();
                auto* f = memo.res.frame;
                memo.res.frame = nullptr;
                return tcp::send(st.scheduler, st.session_id, f, len);
            });
    }

    inline void
    build_error_response(serv_runtime_context& memo, std::exception_ptr e) {
        rpc_error_code code = rpc_error_code::handler_exception;
        try { std::rethrow_exception(e); }
        catch (std::logic_error&) { code = rpc_error_code::unknown_service; }
        catch (...) {}

        memo.res.realloc_frame(sizeof(rpc_error_code));
        *reinterpret_cast<rpc_error_code*>(memo.res.get_payload()) = code;
        memo.res.frame->header.flags = static_cast<uint08_t>(rpc_flags::error);
        memo.res.frame->header.request_id = memo.req.frame->header.request_id;
        memo.res.frame->header.service_id = memo.req.frame->header.service_id;
    }

    inline auto
    handle_exception(auto& st) {
        return pump::sender::any_exception([&st](std::exception_ptr e) mutable {
            st.close();
            return just_exception(e);
        });
    }

    namespace detail {
        struct forward_cpo {
            template <typename sender_t>
            constexpr decltype(auto) operator()(sender_t&& s) const {
                return __fwd__(s);
            }
        };
    }

    template <uint16_t N>
    auto
    apply_concurrency() {
        if constexpr (N > 0)
            return concurrent(N);
        else
            return ::pump::core::bind_back<detail::forward_cpo>(detail::forward_cpo{});
    }

    template<uint16_t concurrency = 0, typename session_scheduler_t, uint16_enum_concept auto ...service_ids>
    requires(sizeof...(service_ids) > 0)
    auto
    serv_proc() {
        return get_context<session_state<session_scheduler_t> >()
            >> then([](session_state<session_scheduler_t> &sd) {
                return just()
                    >> for_each(coro::make_view_able(check_rpc_state(sd)))
                    >> apply_concurrency<concurrency>()
                    >> with_context(serv_runtime_context())([&sd]() {
                        return recv_req(sd)
                            >> flat_map([&sd](...) {
                                return just()
                                    >> dispatch<service_ids...>()
                                    >> pump::sender::any_exception([](std::exception_ptr e) {
                                        return just()
                                            >> get_context<serv_runtime_context>()
                                            >> then([e](serv_runtime_context& memo) {
                                                build_error_response(memo, e);
                                            });
                                    })
                                    >> send_res(sd);
                            });
                    })
                    >> handle_exception(sd)
                    >> reduce();
            })
            >> flat();
    }

    template<uint16_t concurrency = 0, typename session_scheduler_t, uint16_enum_concept auto ...service_ids>
    auto
    serv(session_scheduler_t* sche, tcp::common::session_id_t id) {
        return tcp::join(sche, id)
            >> push_context(session_state<session_scheduler_t>{sche, id})
            >> serv_proc<concurrency, session_scheduler_t, service_ids...>()
            >> pop_context()
            >> ignore_args()
            >> flat_map([sche, id]() {
                return tcp::stop(sche, id);
            })
            >> pump::sender::any_exception([sche, id](std::exception_ptr e) {
                return tcp::stop(sche, id)
                    >> ignore_all_exception()
                    >> then_exception(e);
            });
    }
}

#endif //PUMP_ENV_SCHEDULER_RPC_SERVER_DISPATCH_HH