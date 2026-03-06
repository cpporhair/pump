
#include <cstdio>
#include <cstring>

#include "udp_echo.hh"
#include "kcp_echo.hh"
#include "tcp_echo.hh"

static bool has_flag(int argc, char** argv, const char* flag) {
    for (int i = 2; i < argc; ++i)
        if (std::strcmp(argv[i], flag) == 0) return true;
    return false;
}

static void usage() {
    printf("Usage: echo <mode> [--epoll]\n\n");
    printf("Modes:\n");
    printf("  udp    UDP echo (self-contained server+client)\n");
    printf("  kcp    KCP echo (self-contained server+client)\n");
    printf("  tcp    TCP echo (self-contained server+client)\n\n");
    printf("Options:\n");
    printf("  --epoll    Use epoll backend (default: io_uring)\n");
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    if (argc < 2) { usage(); return 1; }

    const char* mode = argv[1];
    bool epoll = has_flag(argc, argv, "--epoll");

    if (std::strcmp(mode, "udp") == 0) {
        run_udp_echo(epoll);
    } else if (std::strcmp(mode, "kcp") == 0) {
        run_kcp_echo(epoll);
    } else if (std::strcmp(mode, "tcp") == 0) {
        run_tcp_echo(epoll);
    } else {
        printf("Unknown mode: %s\n\n", mode);
        usage();
        return 1;
    }

    return 0;
}
