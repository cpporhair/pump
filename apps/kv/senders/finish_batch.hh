//

//

#ifndef APPS_KV_FINISH_DB_HH
#define APPS_KV_FINISH_DB_HH

#include "pump/sender/any_exception.hh"
#include "pump/sender/then.hh"
#include "pump/sender/visit.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/pop_context.hh"

#include "../batch/publish.hh"

namespace apps::kv {
    inline auto
    publish() {
        return pump::sender::get_context<data::batch*>()
            >> pump::sender::then([](data::batch* b) {
                assert(b->put_snapshot != nullptr);
                return batch::publish(b);
            })
            >> pump::sender::flat();
    }

    inline auto
    release() {
        return pump::sender::get_context<data::batch*>()
            >> pump::sender::then([](data::batch* b) {
                assert(b->put_snapshot == nullptr);
                b->get_snapshot->release();
            });
    }

    inline auto
    finish_batch() {
        return pump::sender::assert_no_args()
            >> pump::sender::get_context<data::batch*>()
            >> pump::sender::then([](data::batch* b){
                return b->put_snapshot != nullptr;
            })
            >> pump::sender::visit()
            >> pump::sender::then([](auto&& res){
                if constexpr (std::is_same_v<__typ__(res), std::false_type>)
                    return pump::sender::just() >> release();
                else
                    return pump::sender::just() >> publish();
            })
            >> pump::sender::flat()
            >> pump::sender::get_full_context_object()
            >> pump::sender::then([](auto& ctx){
                static_assert(
                    __typ__(ctx)::element_type::template has_type<data::batch*>, "Maybe the current batch object is not at the top of the context");
            })
            >> pump::sender::get_context<data::batch*>()
            >> pump::sender::then([](data::batch* b){
                if(b->put_snapshot != nullptr){
                    b->put_snapshot = nullptr;
                }
                delete b;
            })
            >> pump::sender::pop_context();
    }
}

#endif //APPS_KV_FINISH_DB_HH
