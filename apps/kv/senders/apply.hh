//

//

#ifndef APPS_KV_APPLY_HH
#define APPS_KV_APPLY_HH

#include "pump/sender/concurrent.hh"
#include "pump/sender/visit.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/then.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/for_each.hh"
#include "pump/sender/reduce.hh"

#include "../batch/allocate_put_id.hh"
#include "../fs/allocate.hh"
#include "../fs/free.hh"
#include "../index/update.hh"
#include "../index/cache.hh"
#include "../nvme/sender.hh"
#include "../data/exceptions.hh"
#include "../senders/as_task.hh"


namespace apps::kv {

    inline auto
    merge_batch_and_allocate_page() {
        return pump::sender::get_context<data::batch*>()
            >> pump::sender::then([](data::batch* b, ...){
                return fs::allocate_data_page(b);
            })
            >> pump::sender::flat();
    }

    inline auto
    update_index(data::batch* b){
        return pump::sender::for_each(b->cache)
            >> pump::sender::concurrent()
            >> pump::sender::then([](data::key_value& kv){ return index::update(kv.file); })
            >> pump::sender::flat()
            >> pump::sender::reduce()
            >> pump::sender::then([](bool b){ if (!b) throw data::update_index_failed(); });
    }

    inline auto
    free_page_when_error(data::write_span_list& list){
        return pump::sender::catch_exception<data::write_data_failed>([&list](data::write_data_failed&& e) mutable {
            return pump::sender::just()
                >> pump::sender::generate(list.spans)
                >> pump::sender::concurrent()
                >> pump::sender::then([](data::write_span& s) { return fs::free_span(s); })
                >> pump::sender::flat()
                >> pump::sender::reduce();
        });
    }

    inline auto
    write_data(data::write_span_list& list){
        return pump::sender::for_each(list.spans)
            >> pump::sender::concurrent()
            >> nvme::then_put_span()
            //>> pump::sender::flat()
            >> pump::sender::all([](data::write_span& span){ return span.all_wrote(); })
            >> pump::sender::then([](bool b){ if (!b) throw data::write_data_failed(); });
    }

    inline auto
    write_meta(fs::_allocate::leader_res* res){
        return pump::sender::then([res]() { return fs::allocate_meta_page(res); })
            >> pump::sender::flat()
            >> pump::sender::then([](auto &&res) {
                if constexpr (std::is_same_v<fs::_metadata::leader_res *, __typ__(res)>) {
                    return pump::sender::just() >> write_data(res->span_list) >> free_page_when_error(res->span_list);
                }
                else if constexpr (std::is_same_v<fs::_metadata::follower_res, __typ__(res)>) {
                    return pump::sender::just(__fwd__(res));
                }
                else if constexpr (std::is_same_v<fs::_metadata::failed_res, __typ__(res)>) {
                    return pump::sender::just(std::make_exception_ptr(new data::allocate_page_failed()));
                }
                else {
                    static_assert(std::is_same_v<fs::_metadata::failed_res, __typ__(res)>);
                    throw data::absolutely_not_code_block();
                }
            })
            >> pump::sender::flat();
    }

    inline auto
    handle_exception(fs::_allocate::follower_res& res){
        return pump::sender::catch_exception<data::write_data_failed>([](data::write_data_failed&& e){
            return pump::sender::just();
        });
    }

    inline auto
    check_batch(data::batch* b) {
        return pump::sender::then([b](...){ return !b->any_error(); });
    }

    inline auto
    cache_data_if_succeed(data::batch* b){
        return check_batch(b)
            >> pump::sender::visit()
            >> pump::sender::then([b](auto&& v){
                if constexpr (std::is_same_v<__typ__(v), std::true_type>)
                    return pump::sender::just()
                        >> pump::sender::generate(b->cache)
                        >> pump::sender::concurrent()
                        >> pump::sender::then([](data::key_value& kv){ return index::cache(kv.file); })
                        >> pump::sender::flat()
                        >> pump::sender::reduce();
                else
                    return pump::sender::just();
            })
            >> pump::sender::flat();
    }

    inline auto
    notify_follower(fs::_allocate::leader_res* res){
        return pump::sender::then([res](...){
            for(decltype(auto) e : res->followers_req) {
                e->cb(fs::_allocate::follower_res{});
                delete e;
            }
            delete res;
        });
    }

    inline auto
    request_put_serial_number(data::batch* b) {
        return pump::sender::then([b](){
                return batch::allocate_put_id(b);
            })
            >> pump::sender::flat();
    }

    inline auto
    apply() {
        return pump::sender::get_context<data::batch*>()
            >> pump::sender::then([](data::batch* b){
                return pump::sender::just()
                    >> request_put_serial_number(b)
                    >> update_index(b)
                    >> merge_batch_and_allocate_page()
                    >> pump::sender::then([b](auto &&res) {
                        if constexpr (std::is_same_v<fs::_allocate::leader_res *, __typ__(res)>) {
                            return pump::sender::just()
                                >> write_data(res->span_list)
                                >> write_meta(res)
                                >> free_page_when_error(res->span_list)
                                >> notify_follower(res);
                        }
                        else if constexpr (std::is_same_v<fs::_allocate::follower_res, __typ__(res)>) {
                            return pump::sender::just(__fwd__(res));
                        }
                        else if constexpr (std::is_same_v<fs::_allocate::failed_res, __typ__(res)>){
                            return pump::sender::just(std::make_exception_ptr(new data::allocate_page_failed()));
                        }
                        else {
                            static_assert(false);
                        }
                    })
                    >> pump::sender::flat()
                    >> pump::sender::ignore_all_exception()
                    >> cache_data_if_succeed(b);
            })
            >> pump::sender::flat();
    }

}

#endif //APPS_KV_APPLY_HH
