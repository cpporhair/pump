#ifndef APPS_EXAMPLE_RPC_SERVER_HH
#define APPS_EXAMPLE_RPC_SERVER_HH

#include <print>
#include <cstdio>
#include <cassert>
#include "env/scheduler/rpc/rpc.hh"

#include "../../kv/runtime/scheduler_objects.hh"
#include "env/runtime/runner.hh"
#include "env/runtime/share_nothing.hh"
#include "env/scheduler/tcp/io_uring/scheduler.hh"
#include "env/scheduler/task/sender.hh"
#include "pump/sender/repeat.hh"
#include "pump/sender/submit.hh"

#include "./service.hh"

namespace apps::rpc::server {
    template<typename accept_scheduler_t, typename session_scheduler_t>
    using runtime_schedulers = pump::env::runtime::runtime_schedulers<
        pump::scheduler::task::scheduler,
        accept_scheduler_t,
        session_scheduler_t
    >;

    template<typename accept_scheduler_t, typename session_scheduler_t>
    auto
    create_runtime_schedulers(uint32_t used_core) {
        using rs_t = runtime_schedulers<accept_scheduler_t, session_scheduler_t>;

        const char *address = "0.0.0.0";
        uint16_t port = 8080;
        unsigned queue_depth = 256;
        uint32_t num_cores = std::thread::hardware_concurrency();

        auto *rs = new rs_t();

        auto *accept_sched = new accept_scheduler_t();
        if constexpr (requires { accept_sched->init(address, port, queue_depth); }) {
            if (accept_sched->init(address, port, queue_depth) < 0) {
                std::println(stderr, "Failed to init accept_scheduler");
                std::exit(1);
            }
        } else {
            if (accept_sched->init(address, port) < 0) {
                std::println(stderr, "Failed to init accept_scheduler");
                std::exit(1);
            }
        }

        std::vector<session_scheduler_t *> session_scheds(num_cores);

        session_scheds[used_core] = new session_scheduler_t();
        if constexpr (requires { session_scheds[used_core]->init(queue_depth); }) {
            if (session_scheds[used_core]->init(queue_depth) < 0) {
                std::println(stderr, "Failed to init session_scheduler for core {}", used_core);
                std::exit(1);
            }
        }

        auto *task_sched = new pump::scheduler::task::scheduler(used_core);
        rs->add_core_schedulers(
            used_core,
            task_sched,
            accept_sched,
            session_scheds[used_core]
        );

        assert(!rs->schedulers_by_core.empty() && "schedulers_by_core must not be empty");
        assert(!rs->template get_schedulers<accept_scheduler_t>().empty() && "accept_schedulers must not be empty");

        return rs;
    }

    template<typename accept_scheduler_t, typename session_scheduler_t>
    void
    start(uint32_t core) {
        using rs_t = runtime_schedulers<accept_scheduler_t, session_scheduler_t>;
        pump::sender::just()
            >> pump::sender::get_context<rs_t *>()
            >> pump::sender::then([](rs_t *rs) {
                return pump::sender::just()
                    >> pump::sender::forever()
                    >> pump::sender::flat_map([rs](...) {
                        return pump::scheduler::tcp::wait_connection(
                            rs->template get_schedulers<accept_scheduler_t>()[0]);
                    })
                    >> pump::sender::then([rs](pump::scheduler::tcp::common::session_id_t s) {
                        auto *session_sched = rs->template get_schedulers<session_scheduler_t>()[0];
                        return pump::scheduler::rpc::serv<service::type::sub, service::type::add>(session_sched, s)
                            >> pump::sender::submit(pump::core::make_root_context());
                    })
                    >> pump::sender::count()
                    >> pump::sender::submit(pump::core::make_root_context(rs));
            })
            >> pump::sender::get_context<rs_t *>()
            >> pump::sender::then([](rs_t *rs) {
                pump::env::runtime::start(rs);
            })
            >> pump::sender::submit(
                pump::core::make_root_context(
                    create_runtime_schedulers<accept_scheduler_t, session_scheduler_t>(core)
                )
            );
    }
}

#endif //APPS_EXAMPLE_RPC_SERVER_HH