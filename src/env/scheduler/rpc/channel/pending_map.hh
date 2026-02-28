#ifndef ENV_SCHEDULER_RPC_CHANNEL_PENDING_MAP_HH
#define ENV_SCHEDULER_RPC_CHANNEL_PENDING_MAP_HH

#include <cstdint>
#include <array>
#include <functional>
#include <optional>
#include <variant>

#include "../common/codec_concept.hh"
#include "../common/error.hh"

namespace pump::rpc {

    struct pending_slot {
        enum class state : uint8_t {
            empty = 0,
            active,
        };

        state       status = state::empty;
        uint32_t    request_id = 0;
        uint64_t    deadline_ms = 0;   // 0 = no timeout
        std::move_only_function<void(std::variant<payload_view, std::exception_ptr>)> cb;
    };

    template<uint32_t MaxPending = 1024>
    struct pending_map {
        std::array<pending_slot, MaxPending> slots{};
        uint32_t active_count = 0;

        std::optional<uint32_t>
        insert(uint32_t request_id, uint64_t deadline_ms, auto&& cb) {
            if (active_count >= MaxPending) return std::nullopt;
            uint32_t idx = request_id % MaxPending;
            for (uint32_t i = 0; i < MaxPending; ++i) {
                uint32_t probe = (idx + i) % MaxPending;
                if (slots[probe].status == pending_slot::state::empty) {
                    slots[probe].status      = pending_slot::state::active;
                    slots[probe].request_id  = request_id;
                    slots[probe].deadline_ms = deadline_ms;
                    slots[probe].cb          = std::forward<decltype(cb)>(cb);
                    ++active_count;
                    return probe;
                }
            }
            return std::nullopt;
        }

        auto
        remove(uint32_t request_id)
            -> std::optional<std::move_only_function<void(std::variant<payload_view, std::exception_ptr>)>>
        {
            uint32_t idx = request_id % MaxPending;
            for (uint32_t i = 0; i < MaxPending; ++i) {
                uint32_t probe = (idx + i) % MaxPending;
                auto& slot = slots[probe];
                if (slot.status == pending_slot::state::empty)
                    return std::nullopt;
                if (slot.status == pending_slot::state::active &&
                    slot.request_id == request_id) {
                    auto cb = std::move(slot.cb);
                    slot.status = pending_slot::state::empty;
                    --active_count;
                    return cb;
                }
            }
            return std::nullopt;
        }

        void
        check_timeouts(uint64_t now_ms) {
            for (uint32_t i = 0; i < MaxPending; ++i) {
                auto& slot = slots[i];
                if (slot.status == pending_slot::state::active &&
                    slot.deadline_ms > 0 && now_ms >= slot.deadline_ms) {
                    auto cb = std::move(slot.cb);
                    auto rid = slot.request_id;
                    slot.status = pending_slot::state::empty;
                    --active_count;
                    cb(std::make_exception_ptr(rpc_timeout_error(rid)));
                }
            }
        }

        void
        fail_all(std::exception_ptr e) {
            for (uint32_t i = 0; i < MaxPending; ++i) {
                auto& slot = slots[i];
                if (slot.status == pending_slot::state::active) {
                    auto cb = std::move(slot.cb);
                    slot.status = pending_slot::state::empty;
                    cb(e);
                }
            }
            active_count = 0;
        }
    };

}

#endif //ENV_SCHEDULER_RPC_CHANNEL_PENDING_MAP_HH
