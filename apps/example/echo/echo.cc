
#include <print>
#include <cassert>
#include <thread>
#include <cstring>

#include "pump/sender/flat.hh"
#include "pump/sender/repeat.hh"
#include "pump/sender/sequential.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/visit.hh"
#include "pump/sender/just.hh"
#include "pump/sender/when_all.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/on.hh"
#include "env/scheduler/task/tasks_scheduler.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/pop_context.hh"
#include "env/runtime/share_nothing.hh"
#include "env/runtime/runner.hh"
#include "env/scheduler/net/net.hh"
#include "env/scheduler/net/io_uring/scheduler.hh"
#include "env/scheduler/net/epoll/scheduler.hh"

#include "pump/core/lock_free_queue.hh"

using namespace pump;
using namespace pump::sender;

using task_scheduler_t = scheduler::task::scheduler;


template <typename session_scheduler_t>
struct
session_data {
    scheduler::net::common::session_id_t id;
    session_scheduler_t* scheduler;
    std::atomic<bool> closed;

    session_data(session_data &&rhs) noexcept
        : id(rhs.id)
          , scheduler(rhs.scheduler)
          , closed(rhs.closed.load()) {
    }

    session_data(scheduler::net::common::session_id_t id, session_scheduler_t* scheduler)
        : id(id)
        , scheduler(scheduler)
        , closed(false) {
    }

};

template <typename session_scheduler_t>
auto
check_session(const session_data<session_scheduler_t>& sd) -> coro::return_yields<bool> {
    while (!sd.closed.load())
        co_yield true;
    co_return false;
}

template <typename accept_scheduler_t, typename session_scheduler_t>
using runtime_schedulers = env::runtime::runtime_schedulers<
    task_scheduler_t,
    accept_scheduler_t,
    session_scheduler_t
>;

template <typename accept_scheduler_t, typename session_scheduler_t>
auto
session_proc(const runtime_schedulers<accept_scheduler_t, session_scheduler_t> *rs, const scheduler::net::common::session_id_t sid) {
    auto session_count = rs->template get_schedulers<session_scheduler_t>().size();
    auto core_idx = sid.raw() % session_count;
    auto* session_sched = rs->template get_schedulers<session_scheduler_t>()[core_idx];
    return scheduler::net::join(session_sched, sid)
        >> with_context(session_data<session_scheduler_t>(sid, session_sched))([]() {
            return get_context<session_data<session_scheduler_t>>()
                >> then([](const session_data<session_scheduler_t> &sd) {
                    return coro::make_view_able(check_session(sd));
                })
                >> as_stream()
                >> get_context<session_data<session_scheduler_t>>()
                >> flat_map([](const session_data<session_scheduler_t> &sd, ...) {
                    return scheduler::net::recv(sd.scheduler, sd.id);
                })
                >> get_context<session_data<session_scheduler_t>>()
                >> flat_map([](const session_data<session_scheduler_t> &sd, scheduler::net::common::net_frame &&frame) {
                    auto len = frame.size();
                    return scheduler::net::send(sd.scheduler, sd.id, frame.release(), len);
                    // send_req 持有 net_frame，析构自动释放 data
                })
                >> any_exception([](std::exception_ptr e) {
                    return just()
                        >> get_context<session_data<session_scheduler_t>>()
                        >> then([](session_data<session_scheduler_t> &sd) {
                            sd.closed.store(true);
                        });
                })
                >> count();
        })
        >> flat_map([session_sched, sid](...) {
            return scheduler::net::stop(session_sched, sid);
        });
}

/*
 *  Echo Server — Scheduler-to-CPU-Core Topology
 *
 *  Each core runs one thread; that thread drives all schedulers assigned to it.
 *  accept_scheduler only lives on core 0; session/task schedulers exist on every core.
 *
 *  ┌─────────────────────────────────────────────────────────────────────────┐
 *  │                         runtime_schedulers                             │
 *  ├──────────────────────────┬──────────────────────┬───────────────── ─ ─ ┤
 *  │        Core 0            │       Core 1         │     Core N-1         │
 *  │  ┌──────────────────┐    │  ┌────────────────┐  │  ┌────────────────┐  │
 *  │  │ task_scheduler    │    │  │ task_scheduler  │  │  │ task_scheduler  │  │
 *  │  └──────────────────┘    │  └────────────────┘  │  └────────────────┘  │
 *  │  ┌──────────────────┐    │  ┌────────────────┐  │  ┌────────────────┐  │
 *  │  │ accept_scheduler  │    │  │    nullptr      │  │  │    nullptr      │  │
 *  │  │  (bind/listen)    │    │  │                 │  │  │                 │  │
 *  │  │  io_uring/epoll   │    │  └────────────────┘  │  └────────────────┘  │
 *  │  └──────────────────┘    │  ┌────────────────┐  │  ┌────────────────┐  │
 *  │  ┌──────────────────┐    │  │session_scheduler│  │  │session_scheduler│  │
 *  │  │session_scheduler  │    │  │ io_uring/epoll  │  │  │ io_uring/epoll  │  │
 *  │  │ io_uring/epoll    │    │  └────────────────┘  │  └────────────────┘  │
 *  │  └──────────────────┘    │                       │                      │
 *  ├──────────────────────────┴──────────────────────┴───────────────── ─ ─ ┤
 *  │  N = std::thread::hardware_concurrency()                               │
 *  │  Session assignment: sid.raw() % N → core_idx → session_scheduler[idx] │
 *  │  Backend: --io_uring (default) or --epoll                              │
 *  └─────────────────────────────────────────────────────────────────────────┘
 */

template <typename accept_scheduler_t, typename session_scheduler_t>
auto
create_runtime_schedulers() {
    using rs_t = runtime_schedulers<accept_scheduler_t, session_scheduler_t>;
    auto* rs = new rs_t();

    const char* address = "0.0.0.0";
    uint16_t port = 8080;
    unsigned queue_depth = 256;
    uint32_t num_cores = std::thread::hardware_concurrency();

    auto* accept_sched = new accept_scheduler_t();
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

    std::vector<session_scheduler_t*> session_scheds(num_cores);
    for (uint32_t i = 0; i < num_cores; ++i) {
        session_scheds[i] = new session_scheduler_t();
        if constexpr (requires { session_scheds[i]->init(queue_depth); }) {
            if (session_scheds[i]->init(queue_depth) < 0) {
                std::println(stderr, "Failed to init session_scheduler for core {}", i);
                std::exit(1);
            }
        }
    }

    for (uint32_t i = 0; i < num_cores; ++i) {
        auto* task_sched = new task_scheduler_t(i);
        rs->add_core_schedulers(
            i,
            task_sched,
            i == 0 ? accept_sched : nullptr,
            session_scheds[i]
        );
    }

    assert(!rs->schedulers_by_core.empty() && "schedulers_by_core must not be empty");
    assert(!rs->template get_schedulers<accept_scheduler_t>().empty() && "accept_schedulers must not be empty");

    return rs;
}

template <typename accept_scheduler_t, typename session_scheduler_t>
void
run_echo() {
    using rs_t = runtime_schedulers<accept_scheduler_t, session_scheduler_t>;
    just()
        >> get_context<rs_t *>()
        >> then([](rs_t *rs) {
            return just()
                >> forever()
                >> flat_map([rs](...) {
                    return scheduler::net::wait_connection(rs->template get_schedulers<accept_scheduler_t>()[0]);
                })
                >> then([rs](scheduler::net::common::session_id_t s) {
                    session_proc<accept_scheduler_t, session_scheduler_t>(rs, s)
                        >> submit(core::make_root_context());
                })
                >> count()
                >> submit(core::make_root_context(rs));
        })
        >> get_context<rs_t *>()
        >> then([](rs_t *rs) {
            env::runtime::start(rs);
        })
        >> submit(core::make_root_context(create_runtime_schedulers<accept_scheduler_t, session_scheduler_t>()));
}

int
main(int argc, char **argv) {
    bool use_epoll = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--epoll") == 0) {
            use_epoll = true;
        }
    }

    if (use_epoll) {
        std::println("Using epoll backend");
        using accept_sched_t = scheduler::net::epoll::accept_scheduler<pump::scheduler::net::senders::conn::op>;
        using session_sched_t = scheduler::net::epoll::session_scheduler<
            scheduler::net::senders::join::op,
            scheduler::net::senders::recv::op,
            scheduler::net::senders::send::op,
            scheduler::net::senders::stop::op
        >;
        run_echo<accept_sched_t, session_sched_t>();
    } else {
        std::println("Using io_uring backend");
        using accept_sched_t = scheduler::net::io_uring::accept_scheduler<pump::scheduler::net::senders::conn::op>;
        using session_sched_t = scheduler::net::io_uring::session_scheduler<
            scheduler::net::senders::join::op,
            scheduler::net::senders::recv::op,
            scheduler::net::senders::send::op,
            scheduler::net::senders::stop::op
        >;
        run_echo<accept_sched_t, session_sched_t>();
    }

    return 0;
}
