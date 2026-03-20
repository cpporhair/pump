//
// Created by null on 25-4-14.
//

#ifndef ENV_SCHEDULER_NVME_GET_PAGE_HH
#define ENV_SCHEDULER_NVME_GET_PAGE_HH

#include <bits/move_only_function.h>

#include "pump/core/op_pusher.hh"
#include "pump/core/op_tuple_builder.hh"
#include "pump/core/compute_sender_type.hh"
#include "nvme_page.hh"


namespace pump::scheduler::nvme::get {

    template <page_concept page_t>
    struct
    res {
        page_t* page;
        uint08_t status;

        [[nodiscard]]
        bool
        is_success() const {
            return status == 0;
        }
    };

    template <page_concept page_t>
    struct
    req {
        uint32_t io_flags;
        page_t* page;
        std::move_only_function<void(res<page_t>&& r)> cb;

        template<typename func_t>
        req(uint32_t flags, page_t *p, func_t &&f)
            : io_flags(flags), page(p), cb(__fwd__(f)) {
        }
    };

    template<typename scheduler_t, page_concept page_t>
    struct
    op {
        constexpr static bool nvme_get_data_op = true;
        scheduler_t *scheduler;
        page_t *page;

        op(scheduler_t *s, page_t *p)
            : scheduler(s)
              , page(p) {
        }

        op(op &&rhs) noexcept
            : scheduler(rhs.scheduler)
              , page(rhs.page) {
        }

        template<uint32_t pos, typename context_t, typename scope_t>
        auto
        start(context_t &context, scope_t &scope) {
            return scheduler->schedule(
                new req<page_t>{
                    0,
                    page,
                    [context = context, scope = scope](res<page_t>&& r) mutable {
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


    template <typename scheduler_t, page_concept page_t>
    struct
    sender {
        scheduler_t* scheduler;
        page_t* page;

        sender(scheduler_t* s, page_t* p)
            : scheduler(s)
            , page(p){
        }

        sender(sender&& rhs) noexcept
            : scheduler(rhs.scheduler)
              , page(rhs.page){
        }

        auto
        make_op(){
            return op<scheduler_t, page_t> (scheduler, page);
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
    && (get_current_op_type_t<pos, scope_t>::nvme_get_data_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename context_t>
        static
        void
        push_value(context_t &context, scope_t &scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(context, scope);
        }
    };


    template <typename context_t, typename scheduler_t, scheduler::nvme::page_concept page_t>
    struct
    compute_sender_type<context_t, scheduler::nvme::get::sender<scheduler_t, page_t>> {
        consteval static uint32_t
        count_value() {
            return 2;
        }

        consteval static auto
        get_value_type_identity() {
            return std::type_identity<std::tuple<page_t*, bool>>{};
        }
    };
}

#endif //ENV_SCHEDULER_NVME_GET_PAGE_HH
