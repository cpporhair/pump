//

//

#ifndef APPS_KV_DATA_KV_HH
#define APPS_KV_DATA_KV_HH

#include "pump/core/meta.hh"
#include "./data_page.hh"

namespace apps::kv::data {

    struct
    __ncp__(key_value) {
        data_file* file;

        key_value(data_file *f)
            : file(f) {
            if (!f)
                return;
            file->add_ref();
        }

        key_value(key_value &&rhs)
            : file(__fwd__(rhs.file)) {
            rhs.file = nullptr;
        }

        key_value(const key_value &rhs)
            : file(rhs.file){
            if (!file)
                return;
            file->add_ref();
        }

        ~key_value() {
            if (!file)
                return;
            file->del_ref();
            file->free_payload();
        }
    };

    inline auto
    make_kv(slice* key, slice* val) {
        auto* f = make_data_file(key->len, val->len);
        f->write_key(key);
        f->write_val(val);
        return key_value(f);
    }

    inline auto
    make_tombstone(slice* key) {
        auto* f = make_data_file(key->len, sizeof(uint64_t));
        f->write_key(key);
        f->write_tombstone();
        return key_value(f);
    }
}

#endif //APPS_KV_DATA_KV_HH
