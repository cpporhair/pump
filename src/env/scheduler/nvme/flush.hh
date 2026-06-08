#ifndef ENV_SCHEDULER_NVME_FLUSH_HH
#define ENV_SCHEDULER_NVME_FLUSH_HH

#include <functional>

#include "pump/core/op_pusher.hh"
#include "pump/core/op_tuple_builder.hh"
#include "pump/core/compute_sender_type.hh"

namespace pump::scheduler::nvme::flush {

    struct
    res {
        uint08_t status;

        [[nodiscard]]
        bool
        is_success() const {
            return status == 0;
        }
    };

    struct
    req {
        std::move_only_function<void(res&& r)> cb;

        template<typename func_t>
        explicit
        req(func_t&& f)
            : cb(__fwd__(f)) {
        }
    };

    template<typename scheduler_t>
    struct
    op {
        constexpr static bool nvme_flush_op = true;
        scheduler_t* scheduler;

        explicit
        op(scheduler_t* s)
            : scheduler(s) {
        }

        op(op&& rhs) noexcept
            : scheduler(rhs.scheduler) {
        }

        template<uint32_t pos, typename context_t, typename scope_t>
        auto
        start(context_t& context, scope_t& scope) {
            return scheduler->schedule(
                new req{
                    [context = context, scope = scope](res&& r) mutable {
                        core::op_pusher<pos + 1, scope_t>::push_value(
                            context,
                            scope,
                            __fwd__(r)
                        );
                    }
                }
            );
        }
    };

    template<typename scheduler_t>
    struct
    sender {
        scheduler_t* scheduler;

        explicit
        sender(scheduler_t* s)
            : scheduler(s) {
        }

        sender(sender&& rhs) noexcept
            : scheduler(rhs.scheduler) {
        }

        auto
        make_op() {
            return op<scheduler_t>(scheduler);
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
    && (get_current_op_type_t<pos, scope_t>::nvme_flush_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename context_t>
        static
        void
        push_value(context_t& context, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(context, scope);
        }
    };

    template<typename context_t, typename scheduler_t>
    struct
    compute_sender_type<context_t, scheduler::nvme::flush::sender<scheduler_t>> {
        consteval static uint32_t
        count_value() {
            return 1;
        }

        consteval static auto
        get_value_type_identity() {
            return std::type_identity<scheduler::nvme::flush::res>{};
        }
    };
}

#endif //ENV_SCHEDULER_NVME_FLUSH_HH
