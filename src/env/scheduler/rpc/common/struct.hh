
#ifndef ENV_SCHEDULER_RPC_COMMON_STRUCT_HH
#define ENV_SCHEDULER_RPC_COMMON_STRUCT_HH

#include <cstdint>
#include <stdexcept>
#include <string>

namespace pump::scheduler::rpc::common {

    // RPC 统一帧头（plan.md §三）
    // ┌──────────────────────────────────────────┐
    // │ total_len (4B)     ← 帧总长度（含帧头）    │
    // │ request_id (4B)    ← 关联请求和响应（D-1） │
    // │ module_id (2B)     ← 模块标识（D-14）      │
    // │ msg_type (1B)      ← 模块内的消息类型       │
    // │ flags (1B)         ← request/response/push │
    // │ payload ...                               │
    // └──────────────────────────────────────────┘
    struct
    __attribute__((packed))
    rpc_header {
        uint32_t total_len;
        uint32_t request_id;
        uint16_t module_id;
        uint8_t  msg_type;
        uint8_t  flags;
    };

    static_assert(sizeof(rpc_header) == 12, "rpc_header must be 12 bytes");

    // flags 字段值（plan.md §三）
    enum class rpc_flags : uint8_t {
        request  = 0x00,
        response = 0x01,
        push     = 0x02,
    };

    // 帧头辅助函数
    inline uint32_t payload_len(const rpc_header* hdr) {
        return hdr->total_len - sizeof(rpc_header);
    }

    inline const char* payload_ptr(const rpc_header* hdr) {
        return reinterpret_cast<const char*>(hdr) + sizeof(rpc_header);
    }

    // 错误类型层次（plan.md §十）
    struct rpc_error : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    struct frame_error : rpc_error {
        using rpc_error::rpc_error;
    };

    struct codec_error : rpc_error {
        using rpc_error::rpc_error;
    };

    struct protocol_error : rpc_error {
        using rpc_error::rpc_error;
    };

    struct dispatch_error : rpc_error {
        using rpc_error::rpc_error;
    };

    struct pending_full_error : rpc_error {
        pending_full_error()
            : rpc_error("pending requests map is full") {}
    };

}

#endif //ENV_SCHEDULER_RPC_COMMON_STRUCT_HH
