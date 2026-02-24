
#ifndef ENV_SCHEDULER_RPC_SERVICE_SERVICE_HH
#define ENV_SCHEDULER_RPC_SERVICE_SERVICE_HH

#include <concepts>
#include <utility>

#include "./service_type.hh"

namespace pump::scheduler::rpc::service {

    // service 基模板（plan.md §9.1）
    // 应用层通过模板特化注册具体服务
    template<service_type sid>
    struct service {
        static constexpr bool is_service = false;
    };

    // handle 能力检测（plan.md §9.1, F-SVC-3）
    template<typename T, typename Req>
    concept has_handle_concept = requires(Req&& r) {
        T::handle(std::forward<Req>(r));
    };

}

#endif //ENV_SCHEDULER_RPC_SERVICE_SERVICE_HH
