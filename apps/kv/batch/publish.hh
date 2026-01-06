//

//

#ifndef APPS_KV_BATCH_PUBLISH_HH
#define APPS_KV_BATCH_PUBLISH_HH

#include "../runtime/scheduler_objects.hh"

#include "./scheduler.hh"
#include "./chose_scheduler.hh"

namespace apps::kv::batch {
    inline auto
    publish(data::batch* b) {
        return chose_scheduler()->publish(b);
    }
}

#endif //APPS_KV_BATCH_PUBLISH_HH
