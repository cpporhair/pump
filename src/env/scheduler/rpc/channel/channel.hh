#ifndef ENV_SCHEDULER_RPC_CHANNEL_CHANNEL_HH
#define ENV_SCHEDULER_RPC_CHANNEL_CHANNEL_HH

#include <cstdint>
#include <cstring>
#include <memory>

#include "env/scheduler/net/net.hh"

#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/submit.hh"
#include "pump/core/context.hh"

#include "../common/header.hh"
#include "../common/error.hh"
#include "../common/codec_concept.hh"
#include "../common/config.hh"
#include "./pending_map.hh"

namespace pump::rpc {

    enum class channel_status : uint8_t {
        active,
        draining,
        closed,
    };

    struct no_protocol_state {};

    template<
        typename Codec,
        typename SessionScheduler,
        typename TaskScheduler,
        typename ProtocolState = no_protocol_state
    >
    struct channel {
        // identity
        scheduler::net::common::session_id_t session_id;
        SessionScheduler*                    session_sched = nullptr;
        TaskScheduler*                       task_sched    = nullptr;

        // RPC state
        uint32_t        next_request_id = 1;
        channel_status  status = channel_status::active;
        pending_map<>   pending;
        Codec           _codec;

        // protocol layer state (zero-cost when empty)
        [[no_unique_address]]
        ProtocolState   protocol_state;

        // config
        channel_config  config;

        uint32_t
        alloc_request_id() {
            return next_request_id++;
        }

        Codec&
        codec() { return _codec; }

        // --- send helpers ---
        // Uses net::send sender + submit for fire-and-forget sending,
        // since scheduler::schedule() is private to net op types.

        void
        send_message(message_type type, uint32_t request_id,
                     uint16_t method_id, encode_result&& payload)
        {
            // Linearize header + payload into a single heap buffer.
            // This is necessary because net::send is async — the iov_base
            // pointers must remain valid until the send completes, but
            // encode_result's inline_buf lives on the caller's stack.
            size_t total = rpc_header::size + payload.total_len;
            auto* buf = new uint8_t[total];
            write_header(buf, {type, request_id, method_id});
            size_t offset = rpc_header::size;
            for (size_t i = 0; i < payload.cnt; ++i) {
                std::memcpy(buf + offset,
                            payload.vec[i].iov_base,
                            payload.vec[i].iov_len);
                offset += payload.vec[i].iov_len;
            }

            auto* send_vec = new iovec[1];
            send_vec[0] = {buf, total};

            scheduler::net::send(session_sched, session_id, send_vec, 1)
                >> pump::sender::then(
                    [buf, send_vec](bool) mutable {
                        delete[] buf;
                        delete[] send_vec;
                    })
                >> pump::sender::ignore_all_exception()
                >> pump::sender::submit(core::make_root_context());
        }

        void
        send_response(uint32_t request_id, uint16_t method_id,
                      encode_result&& payload)
        {
            send_message(message_type::response, request_id, method_id,
                         std::move(payload));
        }

        void
        send_error(uint32_t request_id, error_code code,
                   const char* msg)
        {
            encode_result payload{};
            size_t len = write_error_payload(payload.inline_buf, code, msg);
            payload.vec[0] = {payload.inline_buf, len};
            payload.cnt = 1;
            payload.total_len = len;
            send_message(message_type::error, request_id, 0, std::move(payload));
        }

        void
        send_notification(uint16_t method_id, encode_result&& payload) {
            send_message(message_type::notification, 0, method_id,
                         std::move(payload));
        }

        // --- connection management ---

        void
        on_connection_lost(std::exception_ptr) {
            if (status == channel_status::closed) return;
            status = channel_status::closed;

            pending.fail_all(std::make_exception_ptr(connection_lost_error()));

            scheduler::net::stop(session_sched, session_id)
                >> pump::sender::ignore_all_exception()
                >> pump::sender::submit(core::make_root_context());
        }

        void
        close() {
            if (status != channel_status::active) return;
            status = channel_status::draining;

            if (pending.active_count == 0) {
                status = channel_status::closed;
                scheduler::net::stop(session_sched, session_id)
                    >> pump::sender::ignore_all_exception()
                    >> pump::sender::submit(core::make_root_context());
            }
        }
    };

    // Factory function
    template<typename Codec,
             typename SessionScheduler,
             typename TaskScheduler,
             typename ProtocolState = no_protocol_state>
    auto
    make_channel(
        scheduler::net::common::session_id_t sid,
        SessionScheduler* session_sched,
        TaskScheduler* task_sched,
        channel_config config = {}
    ) {
        using channel_t = channel<Codec, SessionScheduler, TaskScheduler, ProtocolState>;
        auto ch = std::make_shared<channel_t>();
        ch->session_id    = sid;
        ch->session_sched = session_sched;
        ch->task_sched    = task_sched;
        ch->config        = config;
        return ch;
    }

}

#endif //ENV_SCHEDULER_RPC_CHANNEL_CHANNEL_HH
