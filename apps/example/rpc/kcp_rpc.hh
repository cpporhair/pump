
#pragma once

#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/repeat.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/any_exception.hh"

#include "env/scheduler/kcp/kcp.hh"
#include "env/scheduler/kcp/io_uring/scheduler.hh"
#include "env/scheduler/kcp/epoll/scheduler.hh"
#include "env/scheduler/kcp/transport.hh"
#include "env/scheduler/rpc/rpc.hh"

#include "./service.hh"

template <typename kcp_sched_t>
static void run_kcp_rpc_impl() {
    using namespace pump::sender;
    namespace kcp = pump::scheduler::kcp;
    namespace rpc = pump::scheduler::rpc;
    using kcp_transport = rpc::transport::kcp;
    using service_type = apps::rpc::service::type;

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
        >> then([server](kcp::common::conv_id_t conv) {
            printf("server: accepted conv=%u\n", conv.value);
            just() >> rpc::serv<kcp_transport, service_type::sub, service_type::add>(server, conv)
                >> then([conv]() {
                    printf("server: session conv=%u ended\n", conv.value);
                })
                >> any_exception([conv](std::exception_ptr) {
                    printf("server: session conv=%u error\n", conv.value);
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
        >> flat_map([client](kcp::common::conv_id_t conv) {
            printf("client: connected conv=%u\n", conv.value);
            return just()
                >> loop(5)
                >> flat_map([client, conv](size_t i) {
                    return just()
                        >> rpc::call<kcp_transport, service_type::add>(
                            client, conv, static_cast<int>(i), 10)
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
        pump::scheduler::kcp::senders::recv::op,
        pump::scheduler::kcp::senders::send::op,
        pump::scheduler::kcp::senders::accept::op,
        pump::scheduler::kcp::senders::connect::op>;
    using epoll_t = pump::scheduler::kcp::epoll::scheduler<
        pump::scheduler::kcp::senders::recv::op,
        pump::scheduler::kcp::senders::send::op,
        pump::scheduler::kcp::senders::accept::op,
        pump::scheduler::kcp::senders::connect::op>;

    printf("KCP RPC (%s)\n", epoll ? "epoll" : "io_uring");
    if (epoll) run_kcp_rpc_impl<epoll_t>();
    else       run_kcp_rpc_impl<uring_t>();
}
