#include <cstdio>
#include <cstring>

#include "./tcp_rpc.hh"
#include "./kcp_rpc.hh"

static void usage() {
    printf("Usage: rpc [mode] [options]\n\n");
    printf("Modes:\n");
    printf("  tcp      TCP RPC (default)\n");
    printf("  kcp      KCP RPC\n\n");
    printf("Options:\n");
    printf("  --epoll  Use epoll backend (default: io_uring)\n");
}

int
main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    const char* mode = argc > 1 ? argv[1] : "tcp";
    bool epoll = false;
    for (int i = 2; i < argc; ++i)
        if (std::strcmp(argv[i], "--epoll") == 0) epoll = true;

    if (std::strcmp(mode, "tcp") == 0) {
        run_tcp_rpc(epoll);
    } else if (std::strcmp(mode, "kcp") == 0) {
        run_kcp_rpc(epoll);
    } else {
        printf("Unknown mode: %s\n\n", mode);
        usage();
        return 1;
    }

    return 0;
}
