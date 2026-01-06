//

//

#ifndef APPS_KV_INDEX_UPDATE_HH
#define APPS_KV_INDEX_UPDATE_HH

#include "./scheduler.hh"
#include "./chose_scheduler.hh"

namespace apps::kv::index {

    inline auto
    update(data::data_file* f) {
        return chose_scheduler(f)->update(f);
    }

}

#endif //APPS_KV_INDEX_UPDATE_HH
