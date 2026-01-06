//

//

#ifndef APPS_KV_STOP_DB_HH
#define APPS_KV_STOP_DB_HH

#include <filesystem>
#include "pump/sender/then.hh"

namespace apps::kv {
    inline auto
    stop_db() {
        return pump::sender::ignore_args()
            >> as_task(runtime::main_core)
            >> pump::sender::then([](){
                runtime::running = false;
            });
    }
}

#endif //APPS_KV_STOP_DB_HH
