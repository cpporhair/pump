
#include <cstdio>

#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/repeat.hh"
#include "pump/sender/submit.hh"

#include "env/scheduler/kcp/kcp.hh"
#include "env/scheduler/kcp/epoll/scheduler.hh"

using namespace pump::sender;
using namespace pump::scheduler;

using kcp_scheduler_t = kcp::epoll::scheduler<
    kcp::senders::recv::op,
    kcp::senders::send::op,
    kcp::senders::accept::op,
    kcp::senders::connect::op
>;

static void run_server(kcp_scheduler_t* sche) {
    just()
        >> forever()
        >> flat_map([sche](auto&&...) {
            return kcp::accept(sche);
        })
        >> then([sche](kcp::common::conv_id_t conv) {
            printf("server: new connection conv=%u\n", conv.value);

            // Per-connection echo loop
            just()
                >> forever()
                >> flat_map([sche, conv](auto&&...) {
                    return kcp::recv(sche, conv);
                })
                >> flat_map([sche, conv](pump::common::net_frame&& frame) {
                    printf("server: echo %u bytes\n", frame.size());
                    auto len = frame.size();
                    auto* data = frame.release();
                    return kcp::send(sche, conv, data, len);
                })
                >> then([](bool ok) {
                    if (!ok) printf("server: send failed\n");
                })
                >> reduce()
                >> submit(pump::core::make_root_context());
        })
        >> reduce()
        >> submit(pump::core::make_root_context());
}

static void run_client(kcp_scheduler_t* sche, const char* server_ip, uint16_t server_port) {
    just()
        >> flat_map([sche, server_ip, server_port](auto&&...) {
            return kcp::connect(sche, server_ip, server_port);
        })
        >> flat_map([sche](kcp::common::conv_id_t conv) {
            printf("client: connected conv=%u\n", conv.value);

            return just()
                >> loop(5)
                >> flat_map([sche, conv](size_t i) {
                    auto* buf = new char[sizeof(int)];
                    *reinterpret_cast<int*>(buf) = static_cast<int>(i);
                    return kcp::send(sche, conv, buf, sizeof(int))
                        >> flat_map([sche, conv](auto&&...) {
                            return kcp::recv(sche, conv);
                        })
                        >> then([i](pump::common::net_frame&& frame) {
                            int v = *reinterpret_cast<const int*>(frame.data());
                            printf("client: sent %zu, got back %d\n", i, v);
                        });
                })
                >> reduce();
        })
        >> then([](auto&&...) {
            printf("client: all done\n");
        })
        >> submit(pump::core::make_root_context());
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    constexpr uint16_t port = 19300;

    auto* server_sche = new kcp_scheduler_t();
    if (server_sche->init("0.0.0.0", port) < 0) {
        fprintf(stderr, "server init failed\n");
        return 1;
    }

    auto* client_sche = new kcp_scheduler_t();
    if (client_sche->init("0.0.0.0", 0) < 0) {
        fprintf(stderr, "client init failed\n");
        return 1;
    }

    run_server(server_sche);
    run_client(client_sche, "127.0.0.1", port);

    auto start = kcp::clock_ms();
    while (kcp::clock_ms() - start < 3000) {  // run for 3 seconds
        auto now = kcp::clock_ms();
        server_sche->advance(now);
        client_sche->advance(now);
    }

    printf("done\n");
    delete server_sche;
    delete client_sche;
    return 0;
}
