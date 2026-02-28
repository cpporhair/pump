
#ifndef PUMP_ENV_SCHEDULER_RPC_SERVER_DISPATCH_HH
#define PUMP_ENV_SCHEDULER_RPC_SERVER_DISPATCH_HH

#include "pump/sender/just.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/then.hh"
#include "env/scheduler/net/net.hh"
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

namespace pump::scheduler::rpc::server {
    using namespace pump::sender;

    template <typename session_scheduler_t>
    struct
    session_state {
        session_scheduler_t* scheduler;
        net::common::session_id_t session_id;
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
        return get_context<server::serv_runtime_context>()
            >> flat_map([](server::serv_runtime_context& memo) {
                return just()
                    >> visit(get_service_class_by_id<service_ids...>(static_cast<service_id_t>(memo.req.get_service_id())))
                    >> flat_map([&memo]<typename T0>([[maybe_unused]] T0 &&result) mutable {
                        if constexpr (requires { std::decay_t<T0>::is_service; }) {
                            if constexpr (std::decay_t<T0>::is_service) {
                                return std::decay_t<T0>::handle(memo.req, memo.res)
                                    >> then([&memo]() {
                                        memo.res.frame->header.flags = static_cast<uint08_t>(
                                            server::rpc_flags::response);
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
            return net::recv(st.scheduler, st.session_id)
                >> get_context<server::serv_runtime_context>()
                >> then([](server::serv_runtime_context& memo, net::common::net_frame &&frame) {
                    memo.req.frame = reinterpret_cast<server::rpc_frame *>(frame._data);;
                });
        });
    }

    inline auto
    send_res(const auto& st) {
        return get_context<server::serv_runtime_context>()
            >> flat_map([&st](server::serv_runtime_context& memo, auto ...should_no_args) {
                static_assert(sizeof...(should_no_args) == 0);
                return net::send(st.scheduler, st.session_id, memo.res.frame, memo.res.get_len());
            });
    }

    inline auto
    handle_exception(auto& st) {
        return pump::sender::any_exception([&st](std::exception_ptr e) mutable {
            st.close();
            return just_exception(e);
        });
    }

    template<typename session_scheduler_t, uint16_enum_concept auto ...service_ids>
    requires(sizeof...(service_ids) > 0)
    auto
    serv_proc() {
        return get_context<session_state<session_scheduler_t> >()
            >> then([](session_state<session_scheduler_t> &sd) {
                return just()
                    >> for_each(coro::make_view_able(check_rpc_state(sd)))
                    >> with_context(server::serv_runtime_context())([&sd]() {
                        return recv_req(sd)
                            >> dispatch<service_ids...>()
                            >> send_res(sd);
                    })
                    >> handle_exception(sd)
                    >> reduce();

            })
            >> flat();
    }

    template<typename session_scheduler_t, uint16_enum_concept auto ...service_ids>
    auto
    serv(session_scheduler_t* sche, net::common::session_id_t id) {
        return net::join(sche, id)
            >> push_context(session_state<session_scheduler_t>{sche, id})
            >> serv_proc<session_scheduler_t, service_ids...>()
            >> pop_context();
    }
}

#endif //PUMP_ENV_SCHEDULER_RPC_SERVER_DISPATCH_HH