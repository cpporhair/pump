
#pragma once

#include "env/scheduler/net/kcp/kcp.hh"
#include "env/scheduler/net/kcp/io_uring/scheduler.hh"
#include "env/scheduler/net/kcp/epoll/scheduler.hh"
#include "env/scheduler/net/common/session.hh"

struct kcp_echo_factory {
    template<typename sched_t>
    using session_type = pump::scheduler::net::session_t<
        kcp::common::kcp_bind<sched_t>,
        pump::scheduler::net::frame_receiver
    >;

    template<typename sched_t>
    static auto* create(kcp::common::conv_id_t conv, sched_t* sche) {
        return new session_type<sched_t>(
            kcp::common::kcp_bind<sched_t>(conv, sche),
            pump::scheduler::net::frame_receiver()
        );
    }
};

template <typename sched_t>
static void run_kcp_echo_impl() {
    constexpr uint16_t port = 19300;

    auto* server = new sched_t();
    auto* client = new sched_t();

    if (server->init("0.0.0.0", port) < 0 ||
        client->init("0.0.0.0", 0) < 0) {
        fprintf(stderr, "KCP init failed\n");
        return;
    }

    using session_t = typename sched_t::address_type;

    // Server: accept → per-connection echo loop
    just()
        >> forever()
        >> flat_map([server](auto&&...) { return kcp::accept(server); })
        >> then([](session_t session) {
            printf("server: new connection conv=%u\n",
                session->invoke(kcp::common::get_conv).value);
            just()
                >> forever()
                >> flat_map([session](auto&&...) { return kcp::recv(session); })
                >> flat_map([session](pump::scheduler::net::net_frame&& frame) {
                    printf("server: echo %u bytes\n", frame.size());
                    auto len = frame.size();
                    auto* data = frame.release();
                    return kcp::send(session, data, len);
                })
                >> then([](bool ok) { if (!ok) printf("server: send failed\n"); })
                >> reduce()
                >> submit(pump::core::make_root_context());
        })
        >> reduce()
        >> submit(pump::core::make_root_context());

    // Client: connect → send 5 ints → recv echoes
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
                    auto* buf = new char[sizeof(int)];
                    *reinterpret_cast<int*>(buf) = static_cast<int>(i);
                    return kcp::send(session, buf, sizeof(int))
                        >> flat_map([session](auto&&...) {
                            return kcp::recv(session);
                        })
                        >> then([i](pump::scheduler::net::net_frame&& frame) {
                            int v = *reinterpret_cast<const int*>(frame.data());
                            printf("client: sent %zu, got back %d\n", i, v);
                        });
                })
                >> reduce();
        })
        >> then([](auto&&...) { printf("client: all done\n"); })
        >> submit(pump::core::make_root_context());

    auto start = kcp::clock_ms();
    while (kcp::clock_ms() - start < 3000) {
        auto now = kcp::clock_ms();
        server->advance(now);
        client->advance(now);
    }

    printf("done\n");
    delete server;
    delete client;
}

static void run_kcp_echo(bool epoll) {
    using uring_t = kcp::io_uring::scheduler<
        kcp_echo_factory,
        kcp::senders::accept::op, kcp::senders::connect::op>;
    using epoll_t = kcp::epoll::scheduler<
        kcp_echo_factory,
        kcp::senders::accept::op, kcp::senders::connect::op>;

    printf("KCP echo (%s)\n", epoll ? "epoll" : "io_uring");
    if (epoll) run_kcp_echo_impl<epoll_t>();
    else       run_kcp_echo_impl<uring_t>();
}
