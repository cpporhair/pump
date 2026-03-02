#ifndef PUMP_ENV_SCHEDULER_RPC_SERVER_SERVICE_HH
#define PUMP_ENV_SCHEDULER_RPC_SERVER_SERVICE_HH

#include <concepts>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <variant>

namespace pump::scheduler::rpc {
    template<typename T>
    concept uint16_enum_concept = std::is_enum_v<T> && std::same_as<std::underlying_type_t<T>, uint16_t>;

    template<uint16_enum_concept auto sid>
    struct service {};

    template<uint16_enum_concept auto ...service_ids>
    auto
    get_service_class_by_id(uint16_enum_concept auto sid) {
        using res_t = std::variant<std::monostate, service<service_ids>...>;
        std::optional<res_t> result;
        (void)((sid == service_ids && (result.emplace(service<service_ids>{}), true)) || ...);
        if (result)
            return result.value();
        else
            return res_t(std::monostate{});
    }
}

#endif //PUMP_ENV_SCHEDULER_RPC_SERVER_SERVICE_HH
