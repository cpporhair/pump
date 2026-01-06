//

//

#ifndef APPS_KV_BATCH_CHOSE_SCHEDULER_HH
#define APPS_KV_BATCH_CHOSE_SCHEDULER_HH

#include "../runtime/scheduler_objects.hh"

namespace apps::kv::batch {
    inline
    auto
    chose_scheduler() {
        return runtime::batch_schedulers.list[0];
    }
}

#endif //APPS_KV_BATCH_CHOSE_SCHEDULER_HH
