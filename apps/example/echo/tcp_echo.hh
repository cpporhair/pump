
#pragma once

#include <cstring>
#include <chrono>

#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/repeat.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/any_exception.hh"

#include "env/scheduler/tcp/tcp.hh"
#include "env/scheduler/tcp/io_uring/scheduler.hh"
#include "env/scheduler/tcp/io_uring/connect_scheduler.hh"
#include "env/scheduler/tcp/epoll/scheduler.hh"
#include "env/scheduler/tcp/epoll/connect_scheduler.hh"

using namespace pump::sender;
namespace tcp = pump::scheduler::tcp;

template <typename accept_sched_t, typename connect_sched_t, typename session_sched_t>
static void run_tcp_echo_impl() {
    constexpr uint16_t port = 19100;

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

    // Init session scheduler (epoll doesn't need init)
    if constexpr (requires { session_sched->init(256u); }) {
        if (session_sched->init(256) < 0) {
            fprintf(stderr, "TCP session init failed\n"); return;
        }
    }

    // Server: accept → per-session echo loop
    just()
        >> forever()
        >> flat_map([accept_sched](...) {
            return tcp::wait_connection(accept_sched);
        })
        >> then([session_sched](tcp::common::session_id_t sid) {
            printf("server: new connection sid=%lu\n", static_cast<unsigned long>(sid.raw()));
            tcp::join(session_sched, sid)
                >> flat_map([session_sched, sid](...) {
                    return just()
                        >> forever()
                        >> flat_map([session_sched, sid](...) {
                            return tcp::recv(session_sched, sid);
                        })
                        >> flat_map([session_sched, sid](tcp::common::net_frame&& frame) {
                            printf("server: echo %u bytes\n", frame.size());
                            auto len = frame.size();
                            return tcp::send(session_sched, sid, frame.release(), len);
                        })
                        >> reduce();
                })
                >> any_exception([](std::exception_ptr) {
                    printf("server: session closed\n");
                    return just();
                })
                >> submit(pump::core::make_root_context());
        })
        >> reduce()
        >> submit(pump::core::make_root_context());

    // Client: connect → send → recv → stop
    bool client_done = false;
    just()
        >> flat_map([connect_sched, port](...) {
            return tcp::connect(connect_sched, "127.0.0.1", port);
        })
        >> flat_map([session_sched](tcp::common::session_id_t sid) {
            printf("client: connected sid=%lu\n", static_cast<unsigned long>(sid.raw()));
            return tcp::join(session_sched, sid)
                >> flat_map([session_sched, sid](...) {
                    const char* msg = "hello from echo client";
                    auto len = static_cast<uint32_t>(std::strlen(msg));
                    auto* buf = new char[len];
                    std::memcpy(buf, msg, len);
                    return tcp::send(session_sched, sid, buf, len);
                })
                >> flat_map([session_sched, sid](...) {
                    return tcp::recv(session_sched, sid);
                })
                >> then([](tcp::common::net_frame&& frame) {
                    printf("client: got echo %u bytes\n", frame.size());
                })
                >> flat_map([session_sched, sid](...) {
                    return tcp::stop(session_sched, sid);
                });
        })
        >> then([&client_done](auto&&...) { client_done = true; printf("client: all done\n"); })
        >> any_exception([&client_done](std::exception_ptr e) {
            client_done = true;
            try { std::rethrow_exception(e); }
            catch (const std::exception& ex) { fprintf(stderr, "client error: %s\n", ex.what()); }
            return just();
        })
        >> submit(pump::core::make_root_context());

    auto start = std::chrono::steady_clock::now();
    while (!client_done && std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
        accept_sched->advance();
        connect_sched->advance();
        session_sched->advance();
    }

    printf("done\n");
    // Skip cleanup — destructors may block on pending pipelines
    // In a real application, use scheduler shutdown() before delete
}

static void run_tcp_echo(bool epoll) {
    printf("TCP echo (%s)\n", epoll ? "epoll" : "io_uring");

    if (epoll) {
        using accept_t = tcp::epoll::accept_scheduler<tcp::senders::conn::op>;
        using connect_t = tcp::epoll::connect_scheduler<tcp::senders::conn::op>;
        using session_t = tcp::epoll::session_scheduler<
            tcp::senders::join::op, tcp::senders::recv::op,
            tcp::senders::send::op, tcp::senders::stop::op>;
        run_tcp_echo_impl<accept_t, connect_t, session_t>();
    } else {
        using accept_t = tcp::io_uring::accept_scheduler<tcp::senders::conn::op>;
        using connect_t = tcp::io_uring::connect_scheduler<tcp::senders::conn::op>;
        using session_t = tcp::io_uring::session_scheduler<
            tcp::senders::join::op, tcp::senders::recv::op,
            tcp::senders::send::op, tcp::senders::stop::op>;
        run_tcp_echo_impl<accept_t, connect_t, session_t>();
    }
}
