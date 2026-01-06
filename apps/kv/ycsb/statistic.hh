//

//

#ifndef APPS_KV_YCSB_STATISTIC_HH
#define APPS_KV_YCSB_STATISTIC_HH

#include <utility>

#include "pump/sender/any_exception.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/then.hh"
#include "pump/sender/just.hh"
#include "pump/sender/submit.hh"
#include "pump/core/meta.hh"

#include "../senders/as_task.hh"

namespace apps::kv::ycsb {
    struct
    statistic_data {
        std::atomic<uint64_t> start_tick;
        std::atomic<uint64_t> stop_tick;
        std::atomic<uint64_t> per_sec;
        std::atomic<uint64_t> put_count;
        std::atomic<uint64_t> pub_count;
        std::atomic<uint64_t> get_count;
        std::atomic<uint64_t> gen_count;
        std::atomic<uint64_t> alc_count;

        double
        duration() {
            return double(stop_tick - start_tick) / spdk_get_ticks_hz();
        }

        double
        qps() {
            return double (put_count + get_count) / duration();
        }

        bool
        sec(){
            if (1 <= (spdk_get_ticks() - per_sec) / spdk_get_ticks_hz()) {
                per_sec = spdk_get_ticks();
                return true;
            }
            return false;
        }

    };

    statistic_data g_statistic_data;

    struct
    statistic_helper {
        statistic_data* data;
        inline
        void
        insert_done(uint32_t count = 1)const{
            data->put_count.fetch_add(count);
        }

        inline
        void
        publish_done(uint32_t count = 1)const{
            data->pub_count.fetch_add(count);
        }

        inline
        void
        just_done(uint32_t count = 1)const{
            data->gen_count.fetch_add(count);
        }

        inline
        void
        read_done(uint32_t count = 1)const{
            data->get_count.fetch_add(count);
        }

        inline
        void
        start_at_now(){
            data->start_tick = spdk_get_ticks();
        }

        inline
        void
        end_at_now(){
            data->stop_tick = spdk_get_ticks();
        }
    };

    struct
    logger {
        auto
        log_line(auto&& ...args){
            ((std::cout << k << ":" << args ), ...) << '\n';
        }

        uint64_t k;
    };

    inline
    auto
    maybe_log_exception(){
        return pump::sender::any_exception([](std::exception_ptr e) {
            return pump::sender::get_context<logger>()
                >> pump::sender::then([e](logger& ctx) {
                    try{
                        std::rethrow_exception(e);
                    }
                    catch (const std::exception& exp){
                        ctx.log_line(exp.what());
                    }
                });
        });
    }

    inline
    auto
    statistic_just(uint32_t count){
        return pump::sender::get_context<statistic_helper>()
            >> pump::sender::then([count](statistic_helper& ctx, auto&& arg){
                ctx.just_done(count);
                return __fwd__(arg);
            });
    }

    inline
    auto
    statistic_alc(uint32_t count){
        return pump::sender::get_context<statistic_helper>()
            >> pump::sender::then([count](statistic_helper& ctx, auto&& arg){
                ctx.data->alc_count.fetch_add(count);
                return arg;
            });
    }

    inline
    auto
    statistic_put(uint32_t count = 1){
        return pump::sender::get_context<statistic_helper>()
            >> pump::sender::then([count](statistic_helper& ctx, auto&& ...arg){
                ctx.insert_done(count);
            });
    }

    inline
    auto
    statistic_get(uint32_t count = 1){
        return pump::sender::get_context<statistic_helper>()
            >> pump::sender::then([count](statistic_helper& ctx, auto&& ...arg){
                ctx.read_done(count);
            });
    }

    inline auto
    start_statistic() {
        return pump::sender::get_context<statistic_helper>()
            >> pump::sender::then([](statistic_helper& ctx, auto&& ...arg){
                std::cout << "start ycsb proc :" << std::chrono::utc_clock::now() << std::endl;
                ctx.start_at_now();
                __forward_values__(arg);
            });
    }

    inline auto
    stop_statistic() {
        return pump::sender::get_context<statistic_helper>()
            >> pump::sender::then([](statistic_helper& ctx, auto&& ...arg){
                ctx.end_at_now();
                std::cout << " stop ycsb proc :" << std::chrono::utc_clock::now() << std::endl;
                __forward_values__(arg);
            });
    }

    inline
    auto
    statistic_publish(uint32_t count = 1){
        return pump::sender::get_context<statistic_helper>()
            >> pump::sender::then([count](statistic_helper& ctx, auto&& ...arg){
                ctx.publish_done(count);
            });
    }

    inline
    auto
    output_statistics_per_sec() {
        return pump::sender::get_context<statistic_helper>()
            >> pump::sender::then([](statistic_helper& ctx, auto&& ...args){
                if (ctx.data->sec())
                    pump::sender::just()
                        >> apps::kv::as_task(apps::kv::runtime::main_core)
                        >> pump::sender::then([&ctx]() {
                            //for (decltype(auto) x: apps::kv::runtime::task_schedulers.list)
                            //    std::cout << " core :" << x->bind_core << " task done : "
                            //              << apps::kv::runtime::g_task_info.done_task_count[x->bind_core] << std::endl;
                            spdk_log_set_level(SPDK_LOG_DEBUG);
                            spdk_log(
                                SPDK_LOG_ERROR,
                                "",
                                __LINE__,
                                "",
                                "last put = %ld, last get = %ld \n",
                                ctx.data->put_count.load(),
                                ctx.data->get_count.load()
                            );
                            /*
                            SPDK_ERRLOG(
                                "last put = %ld, last get = %ld \n",
                                ctx.data->put_count.load(),
                                ctx.data->get_count.load()
                            );
                            */
                            /*
                            std::cout << "page cnt : " << apps::kv::runtime::malloc_count << std::endl;
                            std::cout << "generate : " << ctx.data->gen_count << std::endl;
                            std::cout << "batch    : " << apps::kv::data::batch_count << std::endl;
                            std::cout << "index    : " << apps::kv::runtime::g_task_info.index_count << std::endl;
                            std::cout << "allocate : " << apps::kv::runtime::g_task_info.fs_count << std::endl;
                            std::cout << "nvme     : " << apps::kv::runtime::g_task_info.nv_count << std::endl;
                            std::cout << "curbatch : " << apps::kv::runtime::g_task_info.batch_count << std::endl;
                            std::cout << "wait     : " << apps::kv::runtime::g_task_info.wait_batch << std::endl;
                            std::cout << "publish  : " << apps::kv::runtime::g_task_info.publish << std::endl;
                            std::cout << "last put : " << ctx.data->put_count << std::endl;
                            std::cout << "last get : " << ctx.data->get_count << std::endl;
                            std::cout << "last pub : " << ctx.data->pub_count << std::endl;
                            */
                        })
                        >> pump::sender::submit(pump::core::make_root_context());
                __forward_values__(args);
            });
    }

    inline
    auto
    output_finally_statistics() {
        return pump::sender::get_context<statistic_helper>()
            >> pump::sender::then([](statistic_helper& ctx, auto&& ...arg){
                //for(decltype(auto) x : apps::kv::runtime::task_schedulers.list)
                //    std::cout << " core :" << x->bind_core << " task done : "
                //              << apps::kv::runtime::g_task_info.done_task_count[x->bind_core] << std::endl;
                std::cout << "generate : " << ctx.data->gen_count << std::endl;
                std::cout << "batch    : " << apps::kv::data::batch_count << std::endl;
                std::cout << "index    : " << apps::kv::runtime::g_task_info.index_count << std::endl;
                std::cout << "allocate_data_page : " << apps::kv::runtime::g_task_info.fs_count << std::endl;
                std::cout << "nvme     : " << apps::kv::runtime::g_task_info.nv_count << std::endl;
                std::cout << "last put : " << ctx.data->put_count << std::endl;
                std::cout << "last get : " << ctx.data->get_count << std::endl;
                std::cout << "last pub : " << ctx.data->pub_count << std::endl;
                std::cout << "qps      : " << ctx.data->qps() << std::endl;
            });
    }

}
#endif //APPS_KV_YCSB_STATISTIC_HH
