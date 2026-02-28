#ifndef ENV_SCHEDULER_RPC_SENDERS_SERVE_HH
#define ENV_SCHEDULER_RPC_SENDERS_SERVE_HH

#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/repeat.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/any_exception.hh"

#include "env/scheduler/net/net.hh"

#include "../channel/channel.hh"
#include "./reply.hh"

namespace pump::rpc {

    // serve<service_ids...>(ch)
    //
    // Starts a receive loop that:
    //   1. Continuously recv from net layer
    //   2. Parse all complete RPC messages from packet_buffer
    //   3. Response/Error -> pending_map callback (inline)
    //   4. Request -> dispatch to service handler -> encode -> send_response
    //   5. Notification -> dispatch to handler (no response)
    //   6. On recv error -> on_connection_lost cleanup
    //
    // The serve pipeline terminates when recv fails (connection closed/error).
    // on_connection_lost is called to clean up all pending requests.
    // Returns a sender that completes (with void) after the connection ends.
    //
    // Usage:
    //   rpc::serve<svc1, svc2>(ch) >> submit(ctx);
    //
    template<auto ...service_ids, typename channel_ptr_t>
    auto
    serve(channel_ptr_t ch) {
        return pump::sender::just()
            >> pump::sender::forever()
            >> pump::sender::flat_map([ch](auto&&...) {
                return scheduler::net::recv(
                    ch->session_sched, ch->session_id);
            })
            >> pump::sender::then(
                [ch](scheduler::net::common::packet_buffer* buf) {
                    detail::process_buffer<service_ids...>(ch, buf);
                })
            >> pump::sender::reduce()
            >> pump::sender::any_exception(
                [ch](std::exception_ptr e) {
                    ch->on_connection_lost(e);
                    return pump::sender::just();
                });
    }

}

#endif //ENV_SCHEDULER_RPC_SENDERS_SERVE_HH
