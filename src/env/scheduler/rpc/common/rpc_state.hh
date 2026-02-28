
#ifndef PUMP_ENV_SCHEDULER_RPC_COMMON_RPC_STATE_HH
#define PUMP_ENV_SCHEDULER_RPC_COMMON_RPC_STATE_HH

#include <bits/move_only_function.h>
#include "env/scheduler/net/common/struct.hh"


namespace pump::scheduler::rpc::server {
    using completion_callback = std::move_only_function<void(net::common::net_frame&&)>;

    struct
    pending_requests_map {
        enum class slot_state : uint08_t { empty,wait_frame,wait_callback,done };
        struct slot {
            slot_state state = slot_state::empty;
            net::common::net_frame frame;
            completion_callback cb;
        };

        std::vector<slot> slots;

        explicit pending_requests_map(size_t capacity) : slots(capacity) {}

        pending_requests_map(pending_requests_map&&) noexcept = default;
        pending_requests_map& operator=(pending_requests_map&&) noexcept = default;

        std::optional<slot>
        on_callback(uint64_t rid, completion_callback&& cb) {
            auto idx = rid % slots.size();
            switch (slots[idx].state) {
                case slot_state::empty: {
                    slots[idx] = {slot_state::wait_frame, {}, std::move(cb)};
                    return std::nullopt;
                }
                case slot_state::wait_frame: {
                    throw std::runtime_error("invalid state : wait_frame");
                }
                case slot_state::wait_callback: {
                    auto old = slot{slot_state::empty, __mov__(slots[idx].frame), __fwd__(cb)};
                    slots[idx] = {};
                    return __mov__(old);
                }
                default:
                    throw std::runtime_error("invalid state : done");
            }
        }

        std::optional<slot>
        on_frame(uint64_t rid, net::common::net_frame&& frame) {
            auto idx = rid % slots.size();
            switch (slots[idx].state) {
                case slot_state::empty: {
                    slots[idx] = {slot_state::wait_callback, __fwd__(frame), {}};
                    return std::nullopt;
                }
                case slot_state::wait_callback: {
                    throw std::runtime_error("invalid state : wait_frame");
                }
                case slot_state::wait_frame: {
                    auto old = slot{slot_state::empty, __fwd__(frame), __mov__(slots[idx].cb)};
                    slots[idx] = {};
                    return __mov__(old);
                }
                default:
                    throw std::runtime_error("invalid state : done");
            }
        }

        void
        fail_all(std::exception_ptr ex) {
            // todo 传播异常未实现
        }
    };

    template <typename session_t>
    struct
    rpc_state {
        uint32_t next_request_id = 0;
        pending_requests_map pending{256};
        session_t session;
    };
}

#endif //PUMP_ENV_SCHEDULER_RPC_COMMON_RPC_STATE_HH