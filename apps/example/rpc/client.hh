
#ifndef APPS_EXAMPLE_RPC_CLIENT_HH
#define APPS_EXAMPLE_RPC_CLIENT_HH

#include <oneapi/tbb/task_arena.h>

#include "env/runtime/runner.hh"
#include "env/runtime/share_nothing.hh"
#include "env/scheduler/net/io_uring/scheduler.hh"
#include "env/scheduler/task/sender.hh"
#include "pump/sender/repeat.hh"
#include "pump/sender/submit.hh"

#include "./service.hh"
#include "env/scheduler/net/net.hh"
#include "env/scheduler/rpc/rpc.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/then.hh"

namespace apps::rpc::client {
    using task_scheduler_t = pump::scheduler::task::scheduler;
    using session_id_t = pump::scheduler::net::common::session_id_t;

    template <typename connect_scheduler_t, typename session_scheduler_t>
    using runtime_schedulers = pump::env::runtime::runtime_schedulers<
        task_scheduler_t,
        connect_scheduler_t,
        session_scheduler_t
    >;

    template <typename session_scheduler_t>
    struct
    connection_pool {
        std::vector<std::pair<session_id_t, session_scheduler_t*> > sessions{};
    };

    template<typename connect_scheduler_t, typename session_scheduler_t>
    auto
    create_runtime_schedulers(uint32_t core) {
        using rs_t = runtime_schedulers<connect_scheduler_t, session_scheduler_t>;
        auto *rs = new rs_t();

        unsigned queue_depth = 256;
        uint32_t num_cores = std::thread::hardware_concurrency();

        auto *connect_sched = new connect_scheduler_t();
        auto cfg = pump::scheduler::net::common::scheduler_config{};
        if (connect_sched->init(cfg) < 0) {
            std::println(stderr, "Failed to init connect_scheduler");
            std::exit(1);
        }

        std::vector<session_scheduler_t *> session_scheds(num_cores);

        session_scheds[core] = new session_scheduler_t();
        if constexpr (requires { session_scheds[core]->init(queue_depth); }) {
            if (session_scheds[core]->init(queue_depth) < 0) {
                std::println(stderr, "Failed to init session_scheduler for core {}", core);
                std::exit(1);
            }
        }

        auto *task_sched = new task_scheduler_t(core);
        rs->add_core_schedulers(
            core,
            task_sched,
            connect_sched,
            session_scheds[core]
        );

        assert(!rs->schedulers_by_core.empty() && "schedulers_by_core must not be empty");
        assert(!rs->template get_schedulers<connect_scheduler_t>().empty() && "connect_schedulers must not be empty");

        return rs;
    }

    template<typename conn_scheduler_t, typename sess_scheduler_t>
    inline auto
    connect_to_server(runtime_schedulers<conn_scheduler_t, sess_scheduler_t>* rs) {
        return pump::sender::flat_map([rs](...) {
            auto *conn_sche = rs->template get_schedulers<conn_scheduler_t>()[0];
            return pump::scheduler::net::connect(conn_sche, "127.0.0.1", 8080);
        });
    }

    template<typename conn_scheduler_t, typename sess_scheduler_t>
    inline auto
    bind_session(runtime_schedulers<conn_scheduler_t, sess_scheduler_t>* rs) {
        return pump::sender::flat_map([rs](pump::scheduler::net::common::session_id_t s) {
            auto *sess_sche = rs->template get_schedulers<sess_scheduler_t>()[0];
            return pump::scheduler::net::join(sess_sche, s)
                >> pump::sender::get_context<connection_pool<sess_scheduler_t> >()
                >> pump::sender::then([s,sess_sche](auto &pool) {
                    pool.sessions.emplace_back(std::make_pair(s, sess_sche));
                });
        });
    }

    template<typename conn_scheduler_t, typename sess_scheduler_t>
    inline auto
    make_connection_pool(runtime_schedulers<conn_scheduler_t, sess_scheduler_t>* rs) {
        return pump::sender::repeat(5)
            >> pump::sender::concurrent()
            >> connect_to_server<conn_scheduler_t, sess_scheduler_t>(rs)
            >> bind_session<conn_scheduler_t, sess_scheduler_t>(rs)
            >> pump::sender::any_exception([](...) {
                return std::terminate(), pump::sender::just();
            })
            >> pump::sender::count()
            >> pump::sender::ignore_args();
    }

    template<typename conn_scheduler_t, typename sess_scheduler_t>
    auto
    test_service(runtime_schedulers<conn_scheduler_t, sess_scheduler_t>* rs) {
        return pump::sender::repeat(15)
            >> pump::sender::get_context<connection_pool<sess_scheduler_t> >()
            >> pump::sender::flat_map([rs](auto& pool, auto&& i) {
                auto sess = pool.sessions[i%5].first;
                auto sche = pool.sessions[i%5].second;
                return pump::scheduler::task::delay(rs->template get_schedulers<task_scheduler_t>()[0], 1000)
                    >> pump::scheduler::rpc::call<service::type::add>(sche, sess, __fwd__(i), 1)
                    >> pump::sender::then([rs, &pool](auto&& res) {
                        std::cout << "call add result: " << res.v << std::endl;
                    });
            })
            >> pump::sender::count();
    }

    template<typename conn_scheduler_t, typename sess_scheduler_t>
    void
    start(uint32_t core) {
        using rs_t = runtime_schedulers<conn_scheduler_t, sess_scheduler_t>;
        pump::sender::just()
            >> pump::sender::get_context<rs_t *>()
            >> pump::sender::then([](rs_t *rs) {
                return pump::sender::just()
                    >> make_connection_pool<conn_scheduler_t, sess_scheduler_t>(rs)
                    >> test_service<conn_scheduler_t, sess_scheduler_t>(rs)
                    >> pump::sender::submit(pump::core::make_root_context(rs, connection_pool<sess_scheduler_t>{}));
            })
            >> pump::sender::get_context<rs_t *>()
            >> pump::sender::then([](rs_t *rs) {
                pump::env::runtime::start(rs->schedulers_by_core);
            })
            >> pump::sender::submit(
                pump::core::make_root_context(
                    create_runtime_schedulers<conn_scheduler_t, sess_scheduler_t>(core)
                )
            );
    }
}

#endif //APPS_EXAMPLE_RPC_CLIENT_HH