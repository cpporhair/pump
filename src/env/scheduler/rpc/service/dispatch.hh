
#ifndef ENV_SCHEDULER_RPC_SERVICE_DISPATCH_HH
#define ENV_SCHEDULER_RPC_SERVICE_DISPATCH_HH

#include <variant>
#include <optional>
#include <stdexcept>

#include "pump/core/meta.hh"
#include "pump/sender/just.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/then.hh"
#include "pump/sender/visit.hh"

#include "./service.hh"
#include "../common/struct.hh"

namespace pump::scheduler::rpc::service {

    // get_service_class_by_id（plan.md §9.3）
    // 根据 module_id 在编译期展开的服务列表中查找对应的 service 实例
    template<service_type ...service_ids>
    auto get_service_class_by_id(service_type sid) {
        using res_t = std::variant<service<service_ids>...>;
        std::optional<res_t> result;
        (void)((sid == service_ids && (result.emplace(service<service_ids>{}), true)) || ...);
        if (result) return result.value();
        throw common::dispatch_error("unknown service type");
    }

    // dispatch（plan.md §9.3, F-SVC-4）
    // 编译期分发机制，根据 module_id 路由到对应的 service<sid>::handle()
    template<service_type ...service_ids>
    auto dispatch(uint16_t module_id, auto&& msg) {
        auto svc = get_service_class_by_id<service_ids...>(
            static_cast<service_type>(module_id));
        return std::visit(
            [&msg](auto&& svc_instance) {
                using svc_t = std::decay_t<decltype(svc_instance)>;
                if constexpr (svc_t::is_service) {
                    using msg_t = std::decay_t<decltype(msg)>;
                    if constexpr (has_handle_concept<svc_t, msg_t>)
                        return svc_t::handle(__fwd__(msg));
                    else
                        return pump::sender::just_exception(
                            std::make_exception_ptr(
                                common::dispatch_error("unknown request type")));
                } else {
                    return pump::sender::just_exception(
                        std::make_exception_ptr(
                            common::dispatch_error("unknown service type")));
                }
            },
            svc
        );
    }

    // service_decode（plan.md §11.3）
    // 根据 module_id 路由到对应 service 的 decode
    template<service_type ...service_ids>
    auto service_decode(uint16_t module_id, uint8_t msg_type,
                        const char* payload, size_t payload_len) {
        using result_t = std::variant<typename service<service_ids>::message_type...>;
        std::optional<result_t> result;
        auto sid = static_cast<service_type>(module_id);
        (void)((sid == service_ids && (
            result.emplace(service<service_ids>::decode(msg_type, payload, payload_len)),
            true
        )) || ...);
        if (result) return result.value();
        throw common::dispatch_error("unknown module_id for decode");
    }

}

#endif //ENV_SCHEDULER_RPC_SERVICE_DISPATCH_HH
