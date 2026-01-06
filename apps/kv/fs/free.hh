//

//

#ifndef APPS_KV_FS_FREE_HH
#define APPS_KV_FS_FREE_HH

#include "./scheduler.hh"
#include "./chose_scheduler.hh"

namespace apps::kv::fs {
    inline auto
    free_span(data::write_span& span) {
        return chose_scheduler(span.ssd_index)->free(span);
    }

}

#endif //APPS_KV_FS_FREE_HH
