
#pragma once

#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/repeat.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/any_exception.hh"

#include "env/scheduler/net/tcp/tcp.hh"
#include "env/scheduler/net/common/session.hh"
#include "env/scheduler/net/tcp/io_uring/scheduler.hh"
#include "env/scheduler/net/tcp/io_uring/connect_scheduler.hh"
#include "env/scheduler/net/tcp/epoll/scheduler.hh"
#include "env/scheduler/net/tcp/epoll/connect_scheduler.hh"
#include "env/scheduler/net/rpc/rpc.hh"
#include "env/scheduler/net/rpc/common/rpc_layer.hh"

#include "./service.hh"

struct tcp_rpc_factory {
    template<typename sched_t>
    using session_type = pump::scheduler::net::session_t<
        pump::scheduler::tcp::common::tcp_bind<sched_t>,
        pump::scheduler::tcp::common::tcp_ring_buffer<>,
        pump::scheduler::rpc::rpc_session_layer,
        pump::scheduler::net::frame_receiver
    >;

    template<typename sched_t>
    static auto* create(int fd, sched_t* sche) {
        return new session_type<sched_t>(
            pump::scheduler::tcp::common::tcp_bind<sched_t>(fd, sche),
            pump::scheduler::tcp::common::tcp_ring_buffer<>(),
            pump::scheduler::rpc::rpc_session_layer(),
            pump::scheduler::net::frame_receiver()
        );
    }
};

template <typename accept_sched_t, typename connect_sched_t, typename session_sched_t>
static void run_tcp_rpc_impl() {
    using namespace pump::sender;
    namespace tcp = pump::scheduler::tcp;
    namespace rpc = pump::scheduler::rpc;
    using service_type = apps::rpc::service::type;
    using session_t = typename session_sched_t::address_type;

    constexpr uint16_t port = 8080;

    auto* accept_sched = new accept_sched_t();
    auto* connect_sched = new connect_sched_t();
    auto* session_sched = new session_sched_t();

    // Init accept scheduler
    if constexpr (requires { accept_sched->init("0.0.0.0", port, 256u); }) {
        if (accept_sched->init("0.0.0.0", port, 256) < 0) {
            fprintf(stderr, "TCP accept init failed\n"); return;
        }
    } else {
        if (accept_sched->init("0.0.0.0", port) < 0) {
            fprintf(stderr, "TCP accept init failed\n"); return;
        }
    }

    // Init connect scheduler
    auto cfg = tcp::common::scheduler_config{};
    if (connect_sched->init(cfg) < 0) {
        fprintf(stderr, "TCP connect init failed\n"); return;
    }

    // Init session scheduler
    if constexpr (requires { session_sched->init(256u); }) {
        if (session_sched->init(256) < 0) {
            fprintf(stderr, "TCP session init failed\n"); return;
        }
    }

    // Server: accept → join → serve RPC (add + sub) → stop
    just()
        >> forever()
        >> flat_map([accept_sched](...) {
            return tcp::wait_connection(accept_sched);
        })
        >> then([session_sched](int fd) {
            printf("server: new connection fd=%d\n", fd);
            auto* session = tcp_rpc_factory::create(fd, session_sched);
            tcp::join(session_sched, session)
                >> rpc::serv<service_type::sub, service_type::add>(session)
                >> then([](auto&&...) { printf("server: session ended\n"); })
                >> any_exception([](std::exception_ptr) {
                    printf("server: session error\n");
                    return just();
                })
                >> flat_map([session_sched, session]() {
                    return tcp::stop(session_sched, session);
                })
                >> submit(pump::core::make_root_context());
        })
        >> reduce()
        >> submit(pump::core::make_root_context());

    // Client: connect → make RPC calls → stop
    just()
        >> flat_map([connect_sched, port](...) {
            return tcp::connect(connect_sched, "127.0.0.1", port);
        })
        >> flat_map([session_sched](int fd) {
            printf("client: connected fd=%d\n", fd);
            auto* session = tcp_rpc_factory::create(fd, session_sched);
            return tcp::join(session_sched, session)
                >> flat_map([session](...) {
                    return just()
                        >> loop(5)
                        >> flat_map([session](size_t i) {
                            return just()
                                >> rpc::call<service_type::add>(
                                    session, static_cast<int>(i), 10)
                                >> then([i](auto&& res) {
                                    printf("client: %zu + 10 = %d\n", i, res.v);
                                });
                        })
                        >> reduce();
                })
                >> flat_map([session_sched, session](...) {
                    return tcp::stop(session_sched, session);
                });
        })
        >> then([](auto&&...) { printf("client: all done\n"); })
        >> submit(pump::core::make_root_context());

    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        accept_sched->advance();
        connect_sched->advance();
        session_sched->advance();
    }

    printf("done\n");
    delete accept_sched;
    delete connect_sched;
    delete session_sched;
}

static void run_tcp_rpc(bool epoll) {
    namespace tcp = pump::scheduler::tcp;

    if (epoll) {
        using accept_t = tcp::epoll::accept_scheduler<tcp::senders::conn::op>;
        using connect_t = tcp::epoll::connect_scheduler<tcp::senders::conn::op>;
        using session_t = tcp::epoll::session_scheduler<tcp_rpc_factory>;
        printf("TCP RPC (epoll)\n");
        run_tcp_rpc_impl<accept_t, connect_t, session_t>();
    } else {
        using accept_t = tcp::io_uring::accept_scheduler<tcp::senders::conn::op>;
        using connect_t = tcp::io_uring::connect_scheduler<tcp::senders::conn::op>;
        using session_t = tcp::io_uring::session_scheduler<tcp_rpc_factory>;
        printf("TCP RPC (io_uring)\n");
        run_tcp_rpc_impl<accept_t, connect_t, session_t>();
    }
}
