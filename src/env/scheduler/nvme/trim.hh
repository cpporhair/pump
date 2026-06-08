#ifndef ENV_SCHEDULER_NVME_TRIM_HH
#define ENV_SCHEDULER_NVME_TRIM_HH

#include <functional>

#include "spdk/nvme.h"

#include "pump/core/op_pusher.hh"
#include "pump/core/op_tuple_builder.hh"
#include "pump/core/compute_sender_type.hh"

namespace pump::scheduler::nvme::trim {

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
        spdk_nvme_dsm_range range{};
        std::move_only_function<void(res&& r)> cb;

        template<typename func_t>
        req(uint64_t ns_lba, uint32_t ns_lba_count, func_t&& f)
            : cb(__fwd__(f)) {
            range.starting_lba = ns_lba;
            range.length = ns_lba_count;
        }
    };

    template<typename scheduler_t>
    struct
    op {
        constexpr static bool nvme_trim_op = true;
        scheduler_t* scheduler;
        uint64_t ns_lba;
        uint32_t ns_lba_count;

        op(scheduler_t* s, uint64_t lba, uint32_t count)
            : scheduler(s)
            , ns_lba(lba)
            , ns_lba_count(count) {
        }

        op(op&& rhs) noexcept
            : scheduler(rhs.scheduler)
            , ns_lba(rhs.ns_lba)
            , ns_lba_count(rhs.ns_lba_count) {
        }

        template<uint32_t pos, typename context_t, typename scope_t>
        auto
        start(context_t& context, scope_t& scope) {
            return scheduler->schedule(
                new req{
                    ns_lba,
                    ns_lba_count,
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
        uint64_t ns_lba;
        uint32_t ns_lba_count;

        sender(scheduler_t* s, uint64_t lba, uint32_t count)
            : scheduler(s)
            , ns_lba(lba)
            , ns_lba_count(count) {
        }

        sender(sender&& rhs) noexcept
            : scheduler(rhs.scheduler)
            , ns_lba(rhs.ns_lba)
            , ns_lba_count(rhs.ns_lba_count) {
        }

        auto
        make_op() {
            return op<scheduler_t>(scheduler, ns_lba, ns_lba_count);
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
    && (get_current_op_type_t<pos, scope_t>::nvme_trim_op)
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
    compute_sender_type<context_t, scheduler::nvme::trim::sender<scheduler_t>> {
        consteval static uint32_t
        count_value() {
            return 1;
        }

        consteval static auto
        get_value_type_identity() {
            return std::type_identity<scheduler::nvme::trim::res>{};
        }
    };
}

#endif //ENV_SCHEDULER_NVME_TRIM_HH
