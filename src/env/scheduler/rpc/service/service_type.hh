
#ifndef ENV_SCHEDULER_RPC_SERVICE_SERVICE_TYPE_HH
#define ENV_SCHEDULER_RPC_SERVICE_SERVICE_TYPE_HH

#include <cstdint>

namespace pump::scheduler::rpc::service {

    // service_type 枚举（plan.md §9.1）
    // 应用层新增服务时只需在此文件中添加枚举值，无需修改 RPC 层其他代码
    enum class service_type : uint16_t {
        service_001,
        service_002,
        service_003,
        max_service
    };

}

#endif //ENV_SCHEDULER_RPC_SERVICE_SERVICE_TYPE_HH
