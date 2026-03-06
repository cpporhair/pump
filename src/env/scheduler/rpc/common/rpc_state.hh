
#ifndef PUMP_ENV_SCHEDULER_RPC_COMMON_RPC_STATE_HH
#define PUMP_ENV_SCHEDULER_RPC_COMMON_RPC_STATE_HH

#include <bits/move_only_function.h>
#include "env/scheduler/tcp/common/struct.hh"


namespace pump::scheduler::rpc {
    using completion_result = std::variant<tcp::common::net_frame, std::exception_ptr>;
    using completion_callback = std::move_only_function<void(completion_result&&)>;

    struct
    pending_requests_map {
        enum class slot_state : uint08_t { empty, wait_frame, wait_callback };
        struct slot {
            slot_state state = slot_state::empty;
            tcp::common::net_frame frame;
            completion_callback cb;
            uint64_t session_raw = 0;
        };

        std::vector<slot> slots;

        explicit pending_requests_map(size_t capacity) : slots(capacity) {}

        pending_requests_map(pending_requests_map&&) noexcept = default;
        pending_requests_map& operator=(pending_requests_map&&) noexcept = default;

        std::optional<slot>
        on_callback(uint64_t rid, uint64_t session_raw, completion_callback&& cb) {
            auto idx = rid % slots.size();
            switch (slots[idx].state) {
                case slot_state::empty: {
                    slots[idx] = {slot_state::wait_frame, {}, std::move(cb), session_raw};
                    return std::nullopt;
                }
                case slot_state::wait_frame: {
                    throw std::runtime_error("duplicate callback for slot in wait_frame state");
                }
                case slot_state::wait_callback: {
                    auto old = slot{slot_state::empty, __mov__(slots[idx].frame), __fwd__(cb), session_raw};
                    slots[idx] = {};
                    return __mov__(old);
                }
            }
            __builtin_unreachable();
        }

        std::optional<slot>
        on_frame(uint64_t rid, tcp::common::net_frame&& frame) {
            auto idx = rid % slots.size();
            switch (slots[idx].state) {
                case slot_state::empty: {
                    slots[idx] = {slot_state::wait_callback, __fwd__(frame), {}};
                    return std::nullopt;
                }
                case slot_state::wait_callback: {
                    throw std::runtime_error("duplicate frame for slot in wait_callback state");
                }
                case slot_state::wait_frame: {
                    auto old = slot{slot_state::empty, __fwd__(frame), __mov__(slots[idx].cb)};
                    slots[idx] = {};
                    return __mov__(old);
                }
            }
            __builtin_unreachable();
        }

        void
        fail_all(std::exception_ptr ex) {
            for (auto& s : slots) {
                if (s.state == slot_state::wait_frame && s.cb) {
                    s.cb(completion_result(ex));
                }
                if (s.state != slot_state::empty) {
                    s = {};
                }
            }
        }

        void
        fail_session(uint64_t session_raw, std::exception_ptr ex) {
            for (auto& s : slots) {
                if (s.session_raw == session_raw && s.state != slot_state::empty) {
                    if (s.state == slot_state::wait_frame && s.cb) {
                        s.cb(completion_result(ex));
                    }
                    s = {};
                }
            }
        }
    };
}

#endif //PUMP_ENV_SCHEDULER_RPC_COMMON_RPC_STATE_HH
