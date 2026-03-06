
#include <cstdio>
#include <cstdlib>

#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/for_each.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/repeat.hh"
#include "pump/sender/submit.hh"

#include "env/scheduler/udp/udp.hh"
#include "env/scheduler/udp/epoll/scheduler.hh"

using namespace pump::sender;
using namespace pump::scheduler;

using udp_scheduler_t = udp::epoll::scheduler<
    udp::senders::recv::op,
    udp::senders::send::op
>;

static void run_server(udp_scheduler_t* sche) {
    just()
        >> forever()
        >> flat_map([sche](...) {
            return udp::recv(sche);
        })
        >> flat_map([sche](udp::common::datagram&& dg, udp::common::endpoint ep) {
            auto len = dg.size();
            auto* data = dg.release();
            printf("server: recv %u bytes from port %u\n", len, ep.port());
            return udp::send(sche, ep, data, len);
        })
        >> then([](bool ok) {
            if (!ok) printf("server: send failed\n");
        })
        >> reduce()
        >> submit(pump::core::make_root_context());
}

static void run_client(udp_scheduler_t* sche, uint16_t server_port) {
    udp::common::endpoint server{"127.0.0.1", server_port};

    just()
        >> loop(5)
        >> flat_map([sche, server](size_t i) {
            auto* buf = new char[sizeof(int)];
            *reinterpret_cast<int*>(buf) = i;
            return udp::send(sche, server, buf, sizeof(int))
                >> flat_map([sche](...) {
                    return udp::recv(sche);
                })
                >> then([i](udp::common::datagram&& dg, udp::common::endpoint from) {
                    int v = *reinterpret_cast<const int*>(dg.data());
                    printf("client: sent %zu, got back %d from port %u\n", i, v, from.port());
                });
        })
        >> reduce()
        >> then([](auto&&...) {
            printf("client: all done\n");
        })
        >> submit(pump::core::make_root_context());
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    constexpr uint16_t port = 19201;

    auto* server_sche = new udp_scheduler_t();
    if (server_sche->init("0.0.0.0", port) < 0) {
        fprintf(stderr, "server init failed\n");
        return 1;
    }

    auto* client_sche = new udp_scheduler_t();
    if (client_sche->init("0.0.0.0", 0) < 0) {
        fprintf(stderr, "client init failed\n");
        return 1;
    }

    run_server(server_sche);
    run_client(client_sche, port);

    for (int tick = 0; tick < 100000; ++tick) {
        server_sche->advance();
        client_sche->advance();
    }

    printf("done\n");
    delete server_sche;
    delete client_sche;
    return 0;
}
