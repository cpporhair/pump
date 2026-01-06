//

//

#ifndef APPS_KV_INDEX_SCAN_HH
#define APPS_KV_INDEX_SCAN_HH

#include "./root.hh"
#include "pump/sender/then.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/for_each.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/reduce.hh"

namespace apps::kv::index {
    inline auto
    scan(data::slice* key, uint64_t read_sn, uint64_t free_sn, uint32_t max) {
        /*
        return pump::sender::just()
            >> pump::sender::for_each(runtime::index_schedulers.list)
            >> pump::sender::concurrent()
            >> pump::sender::then([](index::scheduler *scheduler) {
                return scheduler->get_b_tree();
            })
            >> pump::sender::flat()
            >> pump::sender::get_context<data::batch *>()
            >> pump::sender::then([key, read_sn, free_sn, max](data::batch *batch, data::b_tree *btree) {
                auto it = btree->impl.lower_bound(key);
                uint32_t count = 0;
                while (it!= btree->impl.end() && count < max) {
                    auto* v = it->second->get_version(read_sn);
                    if(!v)
                        continue;
                    batch->current_scan_env.push_result(data::key_value{v->file});
                    count++;
                }
            })
            >> pump::sender::reduce()
            >> pump::sender::get_context<data::batch *>()
            >> pump::sender::then([max](data::batch *batch, auto&& ...){
                batch->current_scan_env.collect(max);
            });
        */
    }
}

#endif //APPS_KV_INDEX_SCAN_HH
