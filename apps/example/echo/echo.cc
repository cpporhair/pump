
#include <print>

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

auto
read_packet_coro(scheduler::net::common::packet_buffer* buf) -> coro::return_yields<scheduler::net::common::pkt_iovec> {
    bool run = true;
    do {
        if (auto pio = scheduler::net::common::detail::get_recv_pkt(buf);pio.cnt > 0) {
            co_yield __mov__(pio);
            buf->forward_head(pio.cnt);
        }
        else {
            run = false;
        }
    }
    while (run);
    co_return scheduler::net::common::pkt_iovec{};
}

struct
session_data {
    uint64_t id;
    session_scheduler_t* scheduler;
    std::atomic<bool> closed;

    session_data(session_data &&rhs) noexcept
        : id(__fwd__(rhs.id))
          , scheduler(__fwd__(rhs.scheduler))
          , closed(rhs.closed.load()) {
    }

    session_data(uint64_t id, session_scheduler_t* scheduler)
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
    return new runtime_schedulers();
}

auto
session_proc(const runtime_schedulers *rs, const uint64_t sid) {
    return scheduler::net::join(rs->get_by_core<session_scheduler_t>(sid % 2), sid)
        >> with_context(session_data(sid, rs->get_by_core<session_scheduler_t>(sid % 2)))([]() {
            return get_context<session_data>()
                >> then([](const session_data &sd) {
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
                >> flat_map([](const session_data &sd, scheduler::net::common::pkt_iovec &&pkt) {
                    return scheduler::net::send(sd.scheduler, sd.id, pkt.vec, pkt.cnt);
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
                >> concurrent()
                >> get_context<runtime_schedulers *>()
                >> flat_map([](runtime_schedulers *r, uint64_t s) {
                    return session_proc(r, s);
                })
                >> sequential()
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