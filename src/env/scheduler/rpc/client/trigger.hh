
#ifndef PUMP_ENV_SCHEDULER_RPC_CLIENT_TRIGGER_HH
#define PUMP_ENV_SCHEDULER_RPC_CLIENT_TRIGGER_HH

#include "env/scheduler/rpc/common/rpc_state.hh"
#include "pump/core/meta.hh"
#include "pump/core/op_pusher.hh"
#include "pump/core/op_tuple_builder.hh"
#include "pump/core/compute_sender_type.hh"

namespace pump::scheduler::rpc::client::detail {
    template<typename storage_t>
    struct
    op {
        constexpr static bool storage_at_op = true;
        storage_t *store;
        uint64_t request_id = 0;
        uint64_t session_raw = 0;

        template<uint32_t pos, typename context_t, typename scope_t>
        auto
        start(context_t& context, scope_t& scope) {
            store->wait_activate(
                request_id,
                session_raw,
                [context = context, scope = scope](completion_result &&result) mutable {
                    if (auto* frame = std::get_if<net::common::net_frame>(&result)) {
                        core::op_pusher<pos + 1, scope_t>::push_value(context, scope, __mov__(*frame));
                    } else {
                        core::op_pusher<pos + 1, scope_t>::push_exception(context, scope, std::get<std::exception_ptr>(result));
                    }
                }
            );
        }
    };

    template <typename storage_t>
    struct
    storage_at_sender {
        using storage_type = storage_t;
        storage_type* store;
        uint64_t request_id = 0;
        uint64_t session_raw = 0;

        auto
        make_op() {
            return op<storage_t>{store, request_id, session_raw};
        }

        template<typename context_t>
        auto
        connect() {
            return core::builder::op_list_builder<0>().push_back(make_op());
        }
    };

    struct
    trigger {
        pending_requests_map map;

    private:
        friend struct op<trigger>;

        template<typename func_t>
        auto
        wait_activate(uint64_t request_id, uint64_t session_raw, func_t&& f) {
            if (auto res = map.on_callback(request_id, session_raw, __fwd__(f)))
                res.value().cb(completion_result(__mov__(res.value().frame)));
        }

    public:

        explicit trigger(size_t map_capacity) : map(map_capacity){}

        auto
        wait_response(uint64_t request_id, uint64_t session_raw) {
            return storage_at_sender<trigger>(this, request_id, session_raw);
        }

        void
        fail_session(uint64_t session_raw, std::exception_ptr ex) {
            map.fail_session(session_raw, ex);
        }

        auto
        on_response(uint64_t request_id, net::common::net_frame&& frame) {
            if (auto res = map.on_frame(request_id, __fwd__(frame)))
                res.value().cb(completion_result(__mov__(res.value().frame)));
        }
    };
}

namespace pump::core {
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
    && (get_current_op_type_t<pos, scope_t>::storage_at_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename context_t>
        static void
        push_value(context_t& context, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(context, scope);
        }
    };

    template <typename context_t, typename scheduler_t>
    struct
    compute_sender_type<context_t, scheduler::rpc::client::detail::storage_at_sender<scheduler_t>> {
        consteval static uint32_t
        count_value() {
            return 1;
        }

        consteval static auto
        get_value_type_identity() {
            return std::type_identity<pump::scheduler::net::common::net_frame>{};
        }
    };
}


#endif //PUMP_ENV_SCHEDULER_RPC_CLIENT_TRIGGER_HH
