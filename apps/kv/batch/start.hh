//

//

#ifndef APPS_KV_BATCH_START_HH
#define APPS_KV_BATCH_START_HH

#include "../runtime/scheduler_objects.hh"
#include "./chose_scheduler.hh"

namespace apps::kv::batch {
    inline auto
    start(data::batch* b) {
        return chose_scheduler()->start_batch(b);
    }
}

#endif //APPS_KV_BATCH_START_HH
