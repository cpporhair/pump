
#include <print>
#include <cassert>
#include <thread>

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

using accept_scheduler_t = scheduler::net::io_uring::accept_scheduler<pump::scheduler::net::senders::conn::op>;
using session_scheduler_t = scheduler::net::io_uring::session_scheduler<
        scheduler::net::senders::join::op,
        scheduler::net::senders::recv::op,
        scheduler::net::senders::send::op,
        scheduler::net::senders::stop::op
    >;

struct
pkt_iovec_ex {
    scheduler::net::common::pkt_iovec pkt;
    scheduler::net::common::packet_buffer* buf;
    size_t forward_cnt;
};

auto
read_packet_coro(scheduler::net::common::packet_buffer* buf) -> coro::return_yields<pkt_iovec_ex> {
    bool run = true;
    do {
        if (auto pio = scheduler::net::common::detail::get_recv_pkt(buf); pio.cnt > 0) {
            co_yield pkt_iovec_ex{pio, buf, pio.len()};
            // forward_head 在 co_yield 恢复后调用：无 concurrent 时 send 已完成，数据安全
            buf->forward_head(pio.len());
        }
        else {
            run = false;
        }
    }
    while (run);
    co_return pkt_iovec_ex{};
}

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

auto
check_session(const session_data& sd) -> coro::return_yields<bool> {
    while (!sd.closed.load())
        co_yield true;
    co_return false;
}

using runtime_schedulers = env::runtime::runtime_schedulers<
    task_scheduler_t,
    accept_scheduler_t,
    session_scheduler_t
>;

auto
create_runtime_schedulers() {
    auto* rs = new runtime_schedulers();

    const char* address = "0.0.0.0";
    uint16_t port = 8080;
    unsigned queue_depth = 256;
    uint32_t num_cores = std::thread::hardware_concurrency();

    auto* accept_sched = new accept_scheduler_t();
    if (accept_sched->init(address, port, queue_depth) < 0) {
        std::println(stderr, "Failed to init accept_scheduler");
        std::exit(1);
    }

    std::vector<session_scheduler_t*> session_scheds(num_cores);
    for (uint32_t i = 0; i < num_cores; ++i) {
        session_scheds[i] = new session_scheduler_t();
        if (session_scheds[i]->init(queue_depth) < 0) {
            std::println(stderr, "Failed to init session_scheduler for core {}", i);
            std::exit(1);
        }
    }

    for (uint32_t i = 0; i < num_cores; ++i) {
        auto* task_sched = new task_scheduler_t(i);
        rs->add_core_schedulers(
            task_sched,
            i == 0 ? accept_sched : nullptr,
            session_scheds[i]
        );
    }

    assert(!rs->schedulers_by_core.empty() && "schedulers_by_core must not be empty");
    assert(!rs->get_schedulers<accept_scheduler_t>().empty() && "accept_schedulers must not be empty");

    return rs;
}

auto
session_proc(const runtime_schedulers *rs, const scheduler::net::common::session_id_t sid) {
    auto session_count = rs->get_schedulers<session_scheduler_t>().size();
    auto core_idx = sid.raw() % session_count;
    auto* session_sched = rs->get_schedulers<session_scheduler_t>()[core_idx];
    return scheduler::net::join(session_sched, sid)
        >> with_context(session_data(sid, session_sched))([]() {
            return get_context<session_data>()
                >> then([](const session_data &sd) {
                    // TODO: check_session 的 closed 检查当前是冗余的，异常会先被 any_exception 捕获
                    return coro::make_view_able(check_session(sd));
                })
                >> as_stream()
                >> get_context<session_data>()
                >> flat_map([](const session_data &sd, ...) {
                    return scheduler::net::recv(sd.scheduler, sd.id);
                })
                >> then([](scheduler::net::common::packet_buffer *p) {
                    return coro::make_view_able(read_packet_coro(p));
                })
                >> as_stream()
                >> get_context<session_data>()
                >> flat_map([](const session_data &sd, pkt_iovec_ex &&pkt_ex) {
                    auto* vec_ptr = pkt_ex.pkt.vec;
                    return scheduler::net::send(sd.scheduler, sd.id, pkt_ex.pkt.vec, pkt_ex.pkt.cnt)
                        >> then([vec_ptr](bool) {
                            delete[] vec_ptr;
                        });
                })
                >> count()
                >> any_exception([](std::exception_ptr e) {
                    return just()
                        >> get_context<session_data>()
                        >> then([](session_data &sd) {
                            sd.closed.store(true);
                        });
                })
                >> count();
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
 *  │  │  io_uring: accept │    │  └────────────────┘  │  └────────────────┘  │
 *  │  └──────────────────┘    │  ┌────────────────┐  │  ┌────────────────┐  │
 *  │  ┌──────────────────┐    │  │session_scheduler│  │  │session_scheduler│  │
 *  │  │session_scheduler  │    │  │ io_uring: r/w   │  │  │ io_uring: r/w   │  │
 *  │  │ io_uring: r/w     │    │  └────────────────┘  │  └────────────────┘  │
 *  │  └──────────────────┘    │                       │                      │
 *  ├──────────────────────────┴──────────────────────┴───────────────── ─ ─ ┤
 *  │  N = std::thread::hardware_concurrency()                               │
 *  │  Session assignment: sid.raw() % N → core_idx → session_scheduler[idx] │
 *  └─────────────────────────────────────────────────────────────────────────┘
 */
int
main(int argc, char **argv) {
    just()
        >> get_context<runtime_schedulers *>()
        >> then([](runtime_schedulers *rs) {
            return just()
                >> forever()
                >> flat_map([rs](...) {
                    return scheduler::net::wait_connection(rs->get_schedulers<accept_scheduler_t>()[0]);
                })
                >> then([rs](scheduler::net::common::session_id_t s) {
                    session_proc(rs, s)
                        >> submit(core::make_root_context());
                })
                >> count()
                >> submit(core::make_root_context(rs));
        })
        >> get_context<runtime_schedulers *>()
        >> then([](runtime_schedulers *rs) {
            env::runtime::start(rs->schedulers_by_core);
        })
        >> submit(core::make_root_context(create_runtime_schedulers()));
    return 0;
}
