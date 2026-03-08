
#include <cstdio>
#include <cstring>
#include <csignal>

#include <rte_eal.h>
#include <arpa/inet.h>

#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/repeat.hh"
#include "pump/sender/submit.hh"

#include "env/scheduler/dgram/dpdk.hh"
#include "env/scheduler/udp/udp.hh"
#include "env/scheduler/udp/dpdk/scheduler.hh"
#include "env/scheduler/kcp/kcp.hh"
#include "env/scheduler/kcp/dpdk/scheduler.hh"

using namespace pump::sender;
using namespace pump::scheduler;

using udp_sched_t = udp::dpdk::scheduler<udp::senders::recv::op, udp::senders::send::op>;
using kcp_sched_t = kcp::dpdk::scheduler<
    kcp::senders::recv::op, kcp::senders::send::op,
    kcp::senders::accept::op, kcp::senders::connect::op>;

static volatile bool g_running = true;

static void signal_handler(int) { g_running = false; }

// ─── UDP server: recv → echo back ───

static void run_udp_server(dgram::dpdk::dpdk_config cfg, uint16_t port) {
    cfg.local_port = port;
    auto* sche = new udp_sched_t();
    if (sche->init(cfg) < 0) {
        fprintf(stderr, "UDP server init failed\n");
        return;
    }
    printf("UDP server listening on port %u\n", port);

    just()
        >> forever()
        >> flat_map([sche](...) { return udp::recv(sche); })
        >> flat_map([sche](udp::common::datagram&& dg, udp::common::endpoint ep) {
            printf("server: recv %u bytes from port %u\n", dg.size(), ep.port());
            auto len = dg.size();
            auto* data = dg.release();
            return udp::send(sche, ep, data, len);
        })
        >> then([](bool ok) { if (!ok) printf("server: send failed\n"); })
        >> reduce()
        >> submit(pump::core::make_root_context());

    while (g_running) sche->advance();

    sche->shutdown();
    sche->advance();
    delete sche;
}

// ─── UDP client: send N ints → recv echoes ───

static void run_udp_client(dgram::dpdk::dpdk_config cfg,
                           const char* server_ip, uint16_t server_port, int count) {
    cfg.local_port = 0;
    auto* sche = new udp_sched_t();
    if (sche->init(cfg) < 0) {
        fprintf(stderr, "UDP client init failed\n");
        return;
    }

    udp::common::endpoint server_ep{server_ip, server_port};
    printf("UDP client → %s:%u, sending %d messages\n", server_ip, server_port, count);

    just()
        >> loop(count)
        >> flat_map([sche, server_ep](size_t i) {
            auto* buf = new char[sizeof(int)];
            *reinterpret_cast<int*>(buf) = static_cast<int>(i);
            return udp::send(sche, server_ep, buf, sizeof(int))
                >> flat_map([sche](...) { return udp::recv(sche); })
                >> then([i](udp::common::datagram&& dg, udp::common::endpoint) {
                    int v = *reinterpret_cast<const int*>(dg.data());
                    printf("client: sent %zu, got back %d\n", i, v);
                });
        })
        >> reduce()
        >> then([](auto&&...) { printf("client: all done\n"); g_running = false; })
        >> submit(pump::core::make_root_context());

    while (g_running) sche->advance();

    sche->shutdown();
    sche->advance();
    delete sche;
}

// ─── KCP server: accept → echo loop ───

static void run_kcp_server(dgram::dpdk::dpdk_config cfg, uint16_t port) {
    cfg.local_port = port;
    auto* sche = new kcp_sched_t();
    if (sche->init(cfg) < 0) {
        fprintf(stderr, "KCP server init failed\n");
        return;
    }
    printf("KCP server listening on port %u\n", port);

    just()
        >> forever()
        >> flat_map([sche](auto&&...) { return kcp::accept(sche); })
        >> then([sche](kcp::common::conv_id_t conv) {
            printf("server: new connection conv=%u\n", conv.value);
            just()
                >> forever()
                >> flat_map([sche, conv](auto&&...) { return kcp::recv(sche, conv); })
                >> flat_map([sche, conv](pump::common::net_frame&& frame) {
                    printf("server: echo %u bytes\n", frame.size());
                    auto len = frame.size();
                    auto* data = frame.release();
                    return kcp::send(sche, conv, data, len);
                })
                >> then([](bool ok) { if (!ok) printf("server: send failed\n"); })
                >> reduce()
                >> submit(pump::core::make_root_context());
        })
        >> reduce()
        >> submit(pump::core::make_root_context());

    while (g_running) {
        auto now = kcp::clock_ms();
        sche->advance(now);
    }

    delete sche;
}

// ─── KCP client: connect → send N ints → recv echoes ───

static void run_kcp_client(dgram::dpdk::dpdk_config cfg,
                           const char* server_ip, uint16_t server_port, int count) {
    cfg.local_port = 0;
    auto* sche = new kcp_sched_t();
    if (sche->init(cfg) < 0) {
        fprintf(stderr, "KCP client init failed\n");
        return;
    }
    printf("KCP client → %s:%u, sending %d messages\n", server_ip, server_port, count);

    just()
        >> flat_map([sche, server_ip, server_port](auto&&...) {
            return kcp::connect(sche, server_ip, server_port);
        })
        >> flat_map([sche, count](kcp::common::conv_id_t conv) {
            printf("client: connected conv=%u\n", conv.value);
            return just()
                >> loop(count)
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
        >> then([](auto&&...) { printf("client: all done\n"); g_running = false; })
        >> submit(pump::core::make_root_context());

    while (g_running) {
        auto now = kcp::clock_ms();
        sche->advance(now);
    }

    delete sche;
}

// ─── Main ───

static void usage() {
    printf("Usage: dpdk <mode> <role> [options]\n\n");
    printf("Modes:\n");
    printf("  udp    UDP echo over DPDK\n");
    printf("  kcp    KCP echo over DPDK\n\n");
    printf("Roles:\n");
    printf("  server              Start echo server\n");
    printf("  client <ip> [N]     Send N messages (default 5) to server at <ip>\n\n");
    printf("Options:\n");
    printf("  --port <P>          Listen/connect port (default: 19200 for UDP, 19300 for KCP)\n");
    printf("  --dpdk-port <P>     DPDK ethdev port id (default: 0)\n");
    printf("  --queue <Q>         DPDK queue id (default: 0)\n");
    printf("  --local-ip <IP>     Local IP for DPDK packets (default: 10.0.0.1)\n\n");
    printf("DPDK EAL is initialized with --no-huge --in-memory --no-telemetry --vdev=net_tap0.\n");
    printf("Use unshare -r -n to run without root (creates user+network namespace).\n\n");
    printf("Quick test (no root needed):\n");
    printf("  ./run_test.sh udp\n");
    printf("  ./run_test.sh kcp\n");
}

static const char* get_arg(int argc, char** argv, const char* flag, const char* def) {
    for (int i = 1; i < argc - 1; ++i)
        if (std::strcmp(argv[i], flag) == 0) return argv[i + 1];
    return def;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    signal(SIGINT, signal_handler);

    if (argc < 3) { usage(); return 1; }

    const char* mode = argv[1];
    const char* role = argv[2];

    // DPDK EAL init
    char no_huge[] = "--no-huge";
    char in_memory[] = "--in-memory";
    char no_telemetry[] = "--no-telemetry";
    char vdev[] = "--vdev=net_tap0";
    char* eal_argv[] = { argv[0], no_huge, in_memory, no_telemetry, vdev, nullptr };
    int eal_argc = 5;
    int ret = rte_eal_init(eal_argc, eal_argv);
    if (ret < 0) {
        fprintf(stderr, "rte_eal_init failed\n");
        return 1;
    }

    // Init ethdev port 0 with 1 queue
    uint16_t dpdk_port = static_cast<uint16_t>(atoi(get_arg(argc, argv, "--dpdk-port", "0")));
    if (dgram::dpdk::init_port(dpdk_port, 1) < 0) {
        fprintf(stderr, "init_port(%u) failed\n", dpdk_port);
        return 1;
    }

    dgram::dpdk::dpdk_config cfg{};
    cfg.port_id = dpdk_port;
    cfg.queue_id = static_cast<uint16_t>(atoi(get_arg(argc, argv, "--queue", "0")));
    cfg.local_ip = inet_addr(get_arg(argc, argv, "--local-ip", "10.0.0.1"));

    bool is_udp = std::strcmp(mode, "udp") == 0;
    bool is_kcp = std::strcmp(mode, "kcp") == 0;
    uint16_t default_port = is_udp ? 19200 : 19300;
    uint16_t port = static_cast<uint16_t>(atoi(get_arg(argc, argv, "--port", "0")));
    if (port == 0) port = default_port;

    if (std::strcmp(role, "server") == 0) {
        if (is_udp)      run_udp_server(cfg, port);
        else if (is_kcp) run_kcp_server(cfg, port);
        else { printf("Unknown mode: %s\n", mode); return 1; }
    } else if (std::strcmp(role, "client") == 0) {
        if (argc < 4) {
            printf("client requires server IP\n");
            return 1;
        }
        const char* server_ip = argv[3];
        int count = argc > 4 && argv[4][0] != '-' ? atoi(argv[4]) : 5;
        if (is_udp)      run_udp_client(cfg, server_ip, port, count);
        else if (is_kcp) run_kcp_client(cfg, server_ip, port, count);
        else { printf("Unknown mode: %s\n", mode); return 1; }
    } else {
        printf("Unknown role: %s\n", role);
        usage();
        return 1;
    }

    rte_eal_cleanup();
    return 0;
}
