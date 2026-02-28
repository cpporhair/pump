
#ifndef PUMP_ENV_SCHEDULER_RPC_COMMON_STRUCT_HH
#define PUMP_ENV_SCHEDULER_RPC_COMMON_STRUCT_HH

#include <cstdint>

#include "env/scheduler/net/net.hh"
#include "env/scheduler/net/net.hh"
#include "env/scheduler/net/common/detail.hh"
#include "env/scheduler/net/common/struct.hh"
#include "pump/core/meta.hh"

#include "./rpc_state.hh"

namespace pump::scheduler::rpc::server {
    struct
    __attribute__((packed))
    rpc_header {
        uint32_t total_len;
        uint64_t request_id;
        uint16_t service_id;
        uint08_t flags;
    };

    enum class
    rpc_flags : uint08_t {
        request  = 0x00,
        response = 0x01,
        push     = 0x02,
    };

    struct
    __attribute__((packed))
    rpc_frame {
        rpc_header  header;
        uint08_t    payload[];
    };

    struct
    rpc_frame_helper {
        rpc_frame* frame;

        rpc_frame_helper() : frame(nullptr) {
        }

        explicit
        rpc_frame_helper(rpc_frame* f) : frame(f) {
        }

        rpc_frame_helper(rpc_frame_helper&& rhs) noexcept : frame(rhs.frame) {
            rhs.frame = nullptr;
        }

        rpc_frame_helper(rpc_frame_helper&) = delete;

        ~rpc_frame_helper() {
            delete[] reinterpret_cast<char*>(frame);
        }

        [[nodiscard]] auto
        get_len() const {
            return frame->header.total_len;
        }

        [[nodiscard]] auto
        get_service_id() const {
            return frame->header.service_id;
        }

        [[nodiscard]] auto
        get_payload() const {
            return frame->payload;
        }

        void
        realloc_frame(uint32_t new_size) {
            auto total = new_size + sizeof(rpc_header);
            if (frame == nullptr) {
                frame = reinterpret_cast<rpc_frame *>(new char[total]);
            }
            else if (frame->header.total_len < total) {
                auto* new_frame = reinterpret_cast<rpc_frame *>(new char[total]);
                std::memcpy(new_frame, frame, frame->header.total_len);
                delete[] reinterpret_cast<char*>(frame);
                frame = new_frame;
            }
            frame->header.total_len = total;
        }
    };

    struct
    serv_runtime_context {

        rpc_frame_helper req{nullptr};
        rpc_frame_helper res{nullptr};

        serv_runtime_context() = default;

        serv_runtime_context(serv_runtime_context&) = delete;

        serv_runtime_context(serv_runtime_context &&rhs) noexcept
            : req(__fwd__(rhs.req))
            , res(__fwd__(rhs.res)) {
        }
    };

    struct
    request_id {
        static inline std::atomic<uint16_t> thread_index_allocator = 1;
        static inline thread_local const uint16_t current_thread_index = ++thread_index_allocator;
        static inline thread_local uint64_t request_id_counter = 0;

        uint64_t value;

        request_id()
            : value((static_cast<uint64_t>(current_thread_index) << 48) | request_id_counter++) {
        }
    };

    template <typename session_scheduler_t>
    struct
    call_runtime_context {
        uint64_t request_id{};
        session_scheduler_t* scheduler = nullptr;
        net::common::session_id_t sid;
        rpc_frame_helper req{nullptr};
        rpc_frame_helper res{nullptr};

        call_runtime_context(
            session_scheduler_t *sche,
            net::common::session_id_t ssid,
            uint64_t rid
        ) : request_id(rid), scheduler(sche), sid(ssid){
        }

        call_runtime_context(call_runtime_context&) = delete;

        call_runtime_context(call_runtime_context &&rhs) noexcept
            : req(__fwd__(rhs.req))
            , res(__fwd__(rhs.res)) {
            std::swap(request_id, rhs.request_id);
            std::swap(scheduler, rhs.scheduler);
            std::swap(sid, rhs.sid);
        }
    };
}

#endif //PUMP_ENV_SCHEDULER_RPC_COMMON_STRUCT_HH