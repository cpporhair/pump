//

//

#ifndef APPS_KV_PUT_HH
#define APPS_KV_PUT_HH

#include "pump/sender/get_context.hh"
#include "pump/sender/then.hh"

#include "../data/batch.hh"
#include "../data/kv.hh"

namespace apps::kv {

    inline auto
    put() {
        return pump::sender::get_context<data::batch*>()
            >> pump::sender::then([](data::batch* b, data::key_value&& kv) mutable {
                b->put(__fwd__(kv));
            });
    }

    inline
    auto
    put(data::key_value&& kv) {
        return pump::sender::forward_value(__fwd__(kv)) >> put();
    }

    inline auto
    del() {
        return pump::sender::get_context<data::batch*>()
            >> pump::sender::then([](data::batch* b, const char* key) mutable {
                b->put(data::make_tombstone(data::make_slice(key)));
            });
    }
}

#endif //APPS_KV_PUT_HH
