#ifndef ENV_SCHEDULER_RPC_SENDERS_NOTIFY_HH
#define ENV_SCHEDULER_RPC_SENDERS_NOTIFY_HH

#include <cstdint>

#include "pump/sender/just.hh"
#include "pump/sender/then.hh"

#include "../common/header.hh"
#include "../common/codec_concept.hh"
#include "../channel/channel.hh"

namespace pump::rpc {

    // notify: fire-and-forget, no pending_map entry
    // uses simple then() sender, no custom op needed
    template<typename channel_ptr_t>
    auto
    notify(channel_ptr_t ch, uint16_t method_id, encode_result&& payload) {
        return pump::sender::just()
            >> pump::sender::then(
                [ch = std::move(ch), method_id,
                 payload = std::move(payload)]() mutable {
                    ch->send_notification(method_id, std::move(payload));
                });
    }

    // Convenience version: auto-encode with channel's codec
    template<typename Message, typename channel_ptr_t>
    auto
    notify(channel_ptr_t ch, uint16_t method_id, const Message& msg) {
        auto encoded = ch->codec().encode_payload(msg);
        return notify(std::move(ch), method_id, std::move(encoded));
    }

}

#endif //ENV_SCHEDULER_RPC_SENDERS_NOTIFY_HH
