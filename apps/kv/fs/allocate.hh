//

//

#ifndef APPS_KV_FS_ALLOCATE_HH
#define APPS_KV_FS_ALLOCATE_HH

#include "./scheduler.hh"
#include "./chose_scheduler.hh"

namespace apps::kv::fs {
    inline auto
    allocate_data_page(data::batch* b) {
        return chose_scheduler()->allocate_data_page(b);
    }

    inline auto
    allocate_meta_page(_allocate::leader_res* res) {
        return chose_scheduler()->allocate_meta_page(res);
    }
}

#endif //APPS_KV_FS_ALLOCATE_HH
