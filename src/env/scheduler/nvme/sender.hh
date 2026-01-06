#ifndef ENV_SCHEDULER_NVME_SENDER_HH
#define ENV_SCHEDULER_NVME_SENDER_HH

#include "./scheduler.hh"
#include "pump/core/random.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/reduce.hh"

namespace pump::scheduler::nvme {
    template <page_concept page_t, typename scheduler_t>
    inline
    auto
    get_page(page_t *p, scheduler_t *s) {
        return s->get(p) >> sender::then([](get::res<page_t> &&res) { return res.is_success(); });
    }

    template <typename pages_t, typename scheduler_t>
    inline
    auto
    get_pages(pages_t&& pl, scheduler_t *s) {
        return sender::as_stream(__fwd__(pl))
            >> sender::concurrent()
            >> sender::flat_map([s](auto *p) { return get_page(p, s); })
            >> sender::all();
    }

    template <page_concept page_t, typename scheduler_t>
    inline
    auto
    put_page(page_t *p, scheduler_t *s) {
        return s->put(p) >> sender::then([](put::res<page_t> &&res) { return res.is_success(); });
    }

    template <typename pages_t, typename scheduler_t>
    inline
    auto
    put_pages(pages_t&& pl, scheduler_t *s) {
        return sender::as_stream(__fwd__(pl))
            >> sender::concurrent()
            >> sender::flat_map([s](auto *p) { return put_page(p, s); })
            >> sender::all();
    }
}

#endif //ENV_SCHEDULER_NVME_SENDER_HH