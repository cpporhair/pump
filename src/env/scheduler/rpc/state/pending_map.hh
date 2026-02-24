
#ifndef ENV_SCHEDULER_RPC_STATE_PENDING_MAP_HH
#define ENV_SCHEDULER_RPC_STATE_PENDING_MAP_HH

#include <cstdint>
#include <cstddef>
#include <vector>
#include <optional>
#include <functional>
#include <exception>

#include "env/scheduler/net/common/struct.hh"

namespace pump::scheduler::rpc::state {

    // completion_callback: recv 到响应后触发的回调（plan.md §六）
    using completion_callback = std::move_only_function<void(net::common::recv_frame&&)>;

    // pending_requests_map（plan.md §6.1）
    // 使用 rid % capacity 作为槽位索引，O(1) 查找
    // 由于 request_id 单调递增且 capacity 固定，只要 in-flight 请求数不超过 capacity，就不会冲突
    struct
    pending_requests_map {
        struct slot {
            bool occupied = false;
            completion_callback cb;
        };

        std::vector<slot> slots;

        explicit pending_requests_map(size_t capacity) : slots(capacity) {}

        pending_requests_map(pending_requests_map&&) noexcept = default;
        pending_requests_map& operator=(pending_requests_map&&) noexcept = default;

        bool insert(uint32_t rid, completion_callback&& cb) {
            auto idx = rid % slots.size();
            if (slots[idx].occupied) return false;
            slots[idx] = {true, std::move(cb)};
            return true;
        }

        std::optional<completion_callback> extract(uint32_t rid) {
            auto idx = rid % slots.size();
            if (!slots[idx].occupied) return std::nullopt;
            slots[idx].occupied = false;
            return std::move(slots[idx].cb);
        }

        [[nodiscard]]
        bool contains(uint32_t rid) const {
            auto idx = rid % slots.size();
            return slots[idx].occupied;
        }

        void fail_all(std::exception_ptr ex) {
            for (auto& s : slots) {
                if (s.occupied) {
                    s.occupied = false;
                    // 回调内部通过 push_exception 传播异常
                    // 此处暂不调用 cb，由上层清理逻辑决定异常传播方式
                }
            }
        }
    };

}

#endif //ENV_SCHEDULER_RPC_STATE_PENDING_MAP_HH
