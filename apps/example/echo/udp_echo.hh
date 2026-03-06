
#pragma once

#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/repeat.hh"
#include "pump/sender/submit.hh"

#include "env/scheduler/udp/udp.hh"
#include "env/scheduler/udp/io_uring/scheduler.hh"
#include "env/scheduler/udp/epoll/scheduler.hh"

using namespace pump::sender;
using namespace pump::scheduler;

template <typename sched_t>
static void run_udp_echo_impl() {
    constexpr uint16_t port = 19200;

    auto* server = new sched_t();
    auto* client = new sched_t();

    if (server->init("0.0.0.0", port) < 0 ||
        client->init("0.0.0.0", 0) < 0) {
        fprintf(stderr, "UDP init failed\n");
        return;
    }

    // Server: forever recv → echo back
    just()
        >> forever()
        >> flat_map([server](...) { return udp::recv(server); })
        >> flat_map([server](udp::common::datagram&& dg, udp::common::endpoint ep) {
            printf("server: recv %u bytes\n", dg.size());
            auto len = dg.size();
            auto* data = dg.release();
            return udp::send(server, ep, data, len);
        })
        >> then([](bool ok) { if (!ok) printf("server: send failed\n"); })
        >> reduce()
        >> submit(pump::core::make_root_context());

    // Client: send 5 ints, receive echoes
    udp::common::endpoint server_ep{"127.0.0.1", port};
    just()
        >> loop(5)
        >> flat_map([client, server_ep](size_t i) {
            auto* buf = new char[sizeof(int)];
            *reinterpret_cast<int*>(buf) = static_cast<int>(i);
            return udp::send(client, server_ep, buf, sizeof(int))
                >> flat_map([client](...) { return udp::recv(client); })
                >> then([i](udp::common::datagram&& dg, udp::common::endpoint) {
                    int v = *reinterpret_cast<const int*>(dg.data());
                    printf("client: sent %zu, got back %d\n", i, v);
                });
        })
        >> reduce()
        >> then([](auto&&...) { printf("client: all done\n"); })
        >> submit(pump::core::make_root_context());

    for (int tick = 0; tick < 100000; ++tick) {
        server->advance();
        client->advance();
    }

    printf("done\n");
    delete server;
    delete client;
}

static void run_udp_echo(bool epoll) {
    using uring_t = udp::io_uring::scheduler<udp::senders::recv::op, udp::senders::send::op>;
    using epoll_t = udp::epoll::scheduler<udp::senders::recv::op, udp::senders::send::op>;

    printf("UDP echo (%s)\n", epoll ? "epoll" : "io_uring");
    if (epoll) run_udp_echo_impl<epoll_t>();
    else       run_udp_echo_impl<uring_t>();
}
