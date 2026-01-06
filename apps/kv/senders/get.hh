//

//

#ifndef APPS_KV_GET_HH
#define APPS_KV_GET_HH

#include "pump/sender/concurrent.hh"
#include "pump/sender/on.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/then.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/pop_context.hh"
#include "pump/sender/for_each.hh"
#include "pump/sender/reduce.hh"

#include "../data/exceptions.hh"
#include "../index/get.hh"
#include "../nvme/sender.hh"

namespace apps::kv {
    namespace _get {
        inline auto
        read_pages(data::data_file* f) {
            return pump::sender::for_each(f->pages)
                >> pump::sender::concurrent()
                >> pump::sender::then([](data::data_page* p){ return nvme::get_page(p); })
                >> pump::sender::flat()
                >> pump::sender::reduce();
        }

        inline auto
        on_index_scheduler(const data::slice* key) {
            return pump::sender::then([key](auto&& ...args){ return index::on_scheduler(key) >> pump::sender::forward_value(args...); })
                >> pump::sender::flat();
        }

        inline auto
        notify_waiters(index::_getter::pager_reader_res* res) {
            return pump::sender::then([res](bool b){
                for(auto *v : res->ver->page_waiters) {
                    auto *req = (index::_getter::req *) v;
                    if (b)
                        req->cb(new index::_getter::pager_waiter_res(data::key_value(res->ver->file)));
                    else
                        req->cb(data::read_page_failed());
                    delete req;
                }
            });
        }

        inline auto
        get(data::batch* b, const char* k) {
            return index::get(data::make_slice(k), b->get_snapshot->get_serial_number(), b->last_free_sn)
                >> pump::sender::then([b](auto&& res){;
                    if constexpr (std::is_same_v<__typ__(res), index::_getter::pager_reader_res*>)
                        return pump::sender::just()
                            >> as_task()
                            >> read_pages(res->kv.file)
                            >> on_index_scheduler(res->key)
                            >> notify_waiters(res)
                            >> pump::sender::then([b, res](){
                                runtime::g_task_info.nv_count++;
                                auto kv = __mov__(res->kv);
                                delete res;
                                return kv;
                            });
                    else if constexpr (std::is_same_v<__typ__(res), index::_getter::pager_waiter_res*>)
                        return pump::sender::just()
                            >> pump::sender::then([res](){
                                runtime::g_task_info.nv_count++;
                                auto kv = __mov__(res->kv);
                                delete res;
                                return kv;
                            });
                    else if constexpr (std::is_same_v<__typ__(res), index::_getter::not_fround_res>)
                        return pump::sender::just()
                            >> pump::sender::forward_value(data::key_value{nullptr});
                    else if constexpr (std::is_same_v<__typ__(res), data::read_page_failed>)
                        return pump::sender::just(std::make_exception_ptr(__fwd__(res)));
                    else
                        throw data::absolutely_not_code_block();
                })
                >> pump::sender::flat()
                >> pump::sender::any_exception([](...){
                    return pump::sender::just()
                        >> pump::sender::forward_value(data::key_value{nullptr});
                });
        }
    }

    inline
    auto
    get() {
        return pump::sender::get_context<data::batch*>()
            >> pump::sender::then([](data::batch* b, auto&& k){
                if constexpr (std::is_same_v<std::string, __typ__(k)>) {
                    return pump::sender::just()
                        >> pump::sender::with_context(__fwd__(k))(
                            pump::sender::get_context<std::string, data::batch*>()
                                >> pump::sender::then([](std::string& s, data::batch* b){
                                    return _get::get(b, s.c_str());
                                })
                                >> pump::sender::flat()
                        );
                }
                if constexpr (std::is_same_v<const char *, __typ__(k)>) {
                    return pump::sender::just()
                        >> pump::sender::with_context(std::string(k))(_get::get(b, k));
                }
                else if constexpr (std::is_same_v<data::key_value, __typ__(k)>) {
                    return _get::read_pages(k.file);
                }
                else {
                    static_assert(std::is_same_v<std::string, __typ__(k)>);
                }
            })
            >> pump::sender::flat();
    }

    inline auto
    get(std::string&& k) {
        return pump::sender::forward_value(__fwd__(k)) >> get();
    }
}

#endif //APPS_KV_GET_HH
