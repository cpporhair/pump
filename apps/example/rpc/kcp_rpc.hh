
#pragma once

#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/repeat.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/any_exception.hh"

#include "env/scheduler/net/kcp/kcp.hh"
#include "env/scheduler/net/common/session.hh"
#include "env/scheduler/net/kcp/io_uring/scheduler.hh"
#include "env/scheduler/net/kcp/epoll/scheduler.hh"
#include "env/scheduler/net/rpc/rpc.hh"
#include "env/scheduler/net/rpc/common/rpc_layer.hh"

#include "./service.hh"

// KCP session factory with rpc_session_layer for RPC use
struct kcp_rpc_factory {
    template<typename sched_t>
    using session_type = pump::scheduler::net::session_t<
        pump::scheduler::kcp::common::kcp_bind<sched_t>,
        pump::scheduler::rpc::rpc_session_layer,
        pump::scheduler::net::frame_receiver
    >;

    template<typename sched_t>
    static auto* create(pump::scheduler::kcp::common::conv_id_t conv, sched_t* sche) {
        return new session_type<sched_t>(
            pump::scheduler::kcp::common::kcp_bind<sched_t>(conv, sche),
            pump::scheduler::rpc::rpc_session_layer(),
            pump::scheduler::net::frame_receiver()
        );
    }
};

template <typename kcp_sched_t>
static void run_kcp_rpc_impl() {
    using namespace pump::sender;
    namespace kcp = pump::scheduler::kcp;
    namespace rpc = pump::scheduler::rpc;
    using service_type = apps::rpc::service::type;
    using session_t = typename kcp_sched_t::address_type;

    constexpr uint16_t port = 19300;

    auto* server = new kcp_sched_t();
    auto* client = new kcp_sched_t();

    if (server->init("0.0.0.0", port) < 0 ||
        client->init("0.0.0.0", 0) < 0) {
        fprintf(stderr, "KCP init failed\n");
        return;
    }

    // Server: accept → serve RPC (add + sub)
    just()
        >> forever()
        >> flat_map([server](auto&&...) { return kcp::accept(server); })
        >> then([](session_t session) {
            printf("server: accepted conv=%u\n",
                session->invoke(kcp::common::get_conv).value);
            just() >> rpc::serv<service_type::sub, service_type::add>(session)
                >> then([session]() {
                    printf("server: session conv=%u ended\n",
                        session->invoke(kcp::common::get_conv).value);
                })
                >> any_exception([session](std::exception_ptr) {
                    printf("server: session conv=%u error\n",
                        session->invoke(kcp::common::get_conv).value);
                    return just();
                })
                >> submit(pump::core::make_root_context());
        })
        >> reduce()
        >> submit(pump::core::make_root_context());

    // Client: connect → make RPC calls
    just()
        >> flat_map([client, port](auto&&...) {
            return kcp::connect(client, "127.0.0.1", port);
        })
        >> flat_map([](session_t session) {
            printf("client: connected conv=%u\n",
                session->invoke(kcp::common::get_conv).value);
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
        >> then([](auto&&...) { printf("client: all done\n"); })
        >> submit(pump::core::make_root_context());

    auto start = kcp::clock_ms();
    while (kcp::clock_ms() - start < 5000) {
        auto now = kcp::clock_ms();
        server->advance(now);
        client->advance(now);
    }

    delete server;
    delete client;
}

static void run_kcp_rpc(bool epoll) {
    using uring_t = pump::scheduler::kcp::io_uring::scheduler<
        kcp_rpc_factory,
        pump::scheduler::kcp::senders::accept::op,
        pump::scheduler::kcp::senders::connect::op>;
    using epoll_t = pump::scheduler::kcp::epoll::scheduler<
        kcp_rpc_factory,
        pump::scheduler::kcp::senders::accept::op,
        pump::scheduler::kcp::senders::connect::op>;

    printf("KCP RPC (%s)\n", epoll ? "epoll" : "io_uring");
    if (epoll) run_kcp_rpc_impl<epoll_t>();
    else       run_kcp_rpc_impl<uring_t>();
}
