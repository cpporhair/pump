//
//

#ifndef APPS_KV_NVME_SENDER_HH
#define APPS_KV_NVME_SENDER_HH

#include "spdk/env.h"

#include "pump/sender/flat.hh"
#include "pump/sender/just.hh"
#include "pump/sender/then.hh"

#include "env/scheduler/nvme/get_page.hh"
#include "env/scheduler/nvme/put_page.hh"
#include "env/scheduler/nvme/scheduler.hh"
#include "env/scheduler/nvme/sender.hh"

#include "../data/data_page.hh"
#include "../data/write_span.hh"
#include "../runtime/scheduler_objects.hh"

namespace apps::kv::nvme {
    inline auto
    get_page(data::data_page* p) {
        if (!p->payload)
            p->payload = data::make_data_page_payload();
        auto *sche = runtime::random_nvme_scheduler(p->ssd_index);
        return sche->get(p)
            >> pump::sender::then([](pump::scheduler::nvme::get::res<data::data_page>&& r){
                if (!r.status)
                    r.page->set_error();
                return r.page;
            });
    }

    inline auto
    put_span(data::write_span& span) {
        return pump::scheduler::nvme::put_pages(span.pages, runtime::random_nvme_scheduler(span.ssd_index))
            >> pump::sender::then([&span](bool ok) -> data::write_span& {
                if (ok) span.set_wrote();
                return span;
            });
    }

    inline auto
    then_put_span() {
        return pump::sender::flat_map([](data::write_span& span) {
            return pump::sender::just() >> put_span(span);
        });
    }
}

#endif //APPS_KV_NVME_SENDER_HH
