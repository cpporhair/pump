#include <cstdio>
#include <csignal>
#include <atomic>

#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/repeat.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/any_exception.hh"

#include "env/scheduler/net/tcp/tcp.hh"
#include "env/scheduler/net/tcp/io_uring/scheduler.hh"
#include "env/scheduler/net/common/session.hh"

#include "./server/factory.hh"
#include "./server/protocol.hh"
#include "./llm/scheduler.hh"

using namespace pump::sender;
namespace tcp = pump::scheduler::tcp;

static std::atomic<bool> running{true};

static void signal_handler(int) { running = false; }

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <model.gguf> [port] [gpu_layers]\n", argv[0]);
        return 1;
    }

    const char* model_path = argv[1];
    uint16_t port          = argc > 2 ? atoi(argv[2]) : 8080;
    int gpu_layers         = argc > 3 ? atoi(argv[3]) : 999;

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
    setvbuf(stdout, nullptr, _IONBF, 0);

    // ---------------------------------------------------------------
    // 1. Init LLM scheduler
    // ---------------------------------------------------------------
    inference::llm_scheduler llm_sched;
    if (!llm_sched.init(model_path, 4096, gpu_layers)) {
        return 1;
    }

    // ---------------------------------------------------------------
    // 2. Init TCP schedulers
    // ---------------------------------------------------------------
    using accept_sched_t  = tcp::io_uring::accept_scheduler<tcp::senders::conn::op>;
    using session_sched_t = tcp::io_uring::session_scheduler<inference::session_factory>;

    auto* accept_sched  = new accept_sched_t();
    auto* session_sched = new session_sched_t();

    if (accept_sched->init("0.0.0.0", port, 256) < 0) {
        fprintf(stderr, "TCP accept init failed\n");
        return 1;
    }
    if (session_sched->init(256) < 0) {
        fprintf(stderr, "TCP session init failed\n");
        return 1;
    }

    fprintf(stderr, "[server] listening on port %d\n", port);

    // ---------------------------------------------------------------
    // 3. Accept loop: for each connection → recv prompts → generate → stream tokens
    // ---------------------------------------------------------------
    just()
        >> forever()
        >> flat_map([accept_sched](...) {
            return tcp::wait_connection(accept_sched);
        })
        >> then([session_sched, &llm_sched](int fd) {
            fprintf(stderr, "[server] new connection fd=%d\n", fd);
            auto* session = inference::session_factory::create(fd, session_sched);

            // Per-session pipeline
            tcp::join(session_sched, session)
                >> flat_map([session, &llm_sched](...) {
                    // Recv loop: each recv is a prompt → schedule generation
                    return just()
                        >> forever()
                        >> flat_map([session, &llm_sched](...) {
                            return tcp::recv(session)
                                >> then([session, &llm_sched](
                                        tcp::common::net_frame&& frame) {
                                    auto prompt =
                                        inference::protocol::parse_prompt(frame);
                                    fprintf(stderr, "[server] prompt(%zu): %.60s%s\n",
                                            prompt.size(), prompt.c_str(),
                                            prompt.size() > 60 ? "..." : "");

                                    // Create generate request with token callback
                                    auto* req = new inference::generate_req{
                                        .prompt = std::move(prompt),
                                        .on_token =
                                            [session](const char* text,
                                                      uint32_t len, bool is_eos) {
                                                auto [buf, buf_len] =
                                                    is_eos
                                                        ? inference::protocol::
                                                              encode_eos()
                                                        : inference::protocol::
                                                              encode_token(text, len);
                                                // Fire-and-forget send
                                                tcp::send(session, buf, buf_len)
                                                    >> submit(pump::core::
                                                           make_root_context());
                                            },
                                    };
                                    llm_sched.schedule(req);
                                });
                        })
                        >> reduce();
                })
                >> any_exception([](std::exception_ptr) {
                    fprintf(stderr, "[server] session closed\n");
                    return just();
                })
                >> flat_map([session_sched, session](...) {
                    return tcp::stop(session_sched, session);
                })
                >> submit(pump::core::make_root_context());
        })
        >> reduce()
        >> submit(pump::core::make_root_context());

    // ---------------------------------------------------------------
    // 4. Main advance loop (single core)
    // ---------------------------------------------------------------
    pump::core::this_core_id = 0;
    while (running) {
        accept_sched->advance();
        session_sched->advance();
        llm_sched.advance();
    }

    fprintf(stderr, "\n[server] shutting down...\n");
    delete accept_sched;
    delete session_sched;
    return 0;
}
