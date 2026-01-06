//

//

#ifndef APPS_KV_START_BATCH_HH
#define APPS_KV_START_BATCH_HH

#include "pump/sender/flat.hh"
#include "pump/sender/push_context.hh"
#include "pump/sender/pop_context.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/visit.hh"
#include "pump/sender/then.hh"
#include "pump/sender/any_exception.hh"

#include "../batch/start.hh"
#include "./finish_batch.hh"
namespace apps::kv {

    template <uint32_t compile_id>
    inline auto
    start_batch_with_compile_id() {
        return pump::sender::ignore_all_exception()
            >> pump::sender::push_context_with_id<compile_id>(new data::batch())
            >> pump::sender::get_context<data::batch*>()
            >> pump::sender::then([](data::batch* batch, auto&& ...args){
                return batch::start(batch)
                    >> pump::sender::forward_value(__fwd__(args)...);
            })
            >> pump::sender::flat();
    }

    template <uint32_t compile_id>
    inline
    auto
    as_batch_with_compile_id(auto&& b) requires std::is_same_v<std::monostate, typename __typ__(b)::bind_back_flag>{
        return pump::sender::then([b = __fwd__(b)](auto&& ...args) mutable {
                return pump::sender::just()
                    >> start_batch_with_compile_id<compile_id>()
                    >> pump::sender::forward_value(__fwd__(args)...)
                    >> __fwd__(b)
                    >> pump::sender::ignore_all_exception()
                    >> finish_batch();
            })
            >> pump::sender::flat();
    }

    template<uint32_t compile_id>
    inline
    auto
    as_batch_with_compile_id(auto &&b) {
        return pump::sender::then([b = __fwd__(b)](auto &&...args) mutable {
                return pump::sender::just()
                    >> start_batch_with_compile_id<compile_id>()
                    >> pump::sender::forward_value(__fwd__(args)...)
                    >> pump::sender::then(__fwd__(b))
                    >> pump::sender::flat()
                    >> pump::sender::ignore_all_exception()
                    >> finish_batch();
            })
            >> pump::sender::flat();
    }

    template<uint32_t compile_id>
    inline
    auto
    as_batch_with_compile_id() {
        return [](auto &&func) mutable {
            return pump::sender::then([f = __fwd__(func)](auto &&...args) mutable {
                    return pump::sender::just()
                        >> start_batch_with_compile_id<compile_id>()
                        >> pump::sender::forward_value(__fwd__(args)...)
                        >> pump::sender::ignore_inner_exception(f())
                        >> finish_batch();
                })
                >> pump::sender::flat();
        };
    }


#define start_batch start_batch_with_compile_id<__COUNTER__>
#define as_batch as_batch_with_compile_id<__COUNTER__>
}

#endif //APPS_KV_START_BATCH_HH
