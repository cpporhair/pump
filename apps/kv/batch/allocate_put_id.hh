//

//

#ifndef APPS_KV_BATCH_ALLOCATE_PUT_ID_HH
#define APPS_KV_BATCH_ALLOCATE_PUT_ID_HH

#include "../runtime/scheduler_objects.hh"
#include "./scheduler.hh"
#include "./chose_scheduler.hh"

namespace apps::kv::batch {
    inline auto
    allocate_put_id(data::batch* b) {
        return chose_scheduler()->allocate_put_snapshot(b);
    }
}

#endif //APPS_KV_BATCH_ALLOCATE_PUT_ID_HH
