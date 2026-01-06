//

//

#ifndef APPS_KV_INDEX_CACHE_HH
#define APPS_KV_INDEX_CACHE_HH

#include "./scheduler.hh"
#include "./chose_scheduler.hh"

namespace apps::kv::index {
    inline auto
    cache(data::data_file* f) {
        return chose_scheduler(f)->cache_file(f);
    }
}

#endif //APPS_KV_INDEX_CACHE_HH
