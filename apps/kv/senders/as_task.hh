//

//

#ifndef APPS_KV_AS_TASK_HH
#define APPS_KV_AS_TASK_HH

#include "../runtime/scheduler_objects.hh"
#include "pump/sender/then.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/generate.hh"

namespace apps::kv {
    inline auto
    random_core() {
        return spdk_get_ticks() % runtime::task_schedulers.list.size();
    }

    inline auto
    random_task_scheduler() {
        return runtime::task_schedulers.list[random_core()]->as_task();
    }

    inline auto
    as_task(uint32_t core = UINT32_MAX){
        return pump::sender::then([core](auto&& ...args){
            if (core == UINT32_MAX)
                return random_task_scheduler()
                    >> pump::sender::forward_value(__fwd__(args)...);
            else
                return runtime::task_schedulers.list[core]->as_task()
                    >> pump::sender::forward_value(__fwd__(args)...);
        })
        >> pump::sender::flat();
    }

    inline auto
    any_task_scheduler(uint32_t core = UINT32_MAX){
        return pump::sender::then([core](auto&& ...args){
            if (core == UINT32_MAX)
                return random_task_scheduler()
                    >> pump::sender::forward_value(__fwd__(args)...);
            else
                return runtime::task_schedulers.list[core]->as_task()
                    >> pump::sender::forward_value(__fwd__(args)...);
        })
            >> pump::sender::flat();
    }

    template<typename bind_back_t, std::ranges::viewable_range range_t>
    inline auto
    generate_on(bind_back_t&& t, range_t&& r){
        return pump::sender::generate(__fwd__(r)) >> __fwd__(t);
    }

    template<typename bind_back_t>
    inline auto
    generate_on(bind_back_t&& t){
        return [t = __fwd__(t)](auto&& r) mutable { return pump::sender::generate(__fwd__(r)) >> __fwd__(t); };
    }
}

#endif //APPS_KV_AS_TASK_HH
