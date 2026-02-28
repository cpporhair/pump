#ifndef ENV_SCHEDULER_RPC_CHANNEL_DISPATCHER_HH
#define ENV_SCHEDULER_RPC_CHANNEL_DISPATCHER_HH

#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>
#include <type_traits>

#include "../common/codec_concept.hh"
#include "../common/error.hh"

#include "pump/sender/then.hh"
#include "pump/sender/just.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/visit.hh"

namespace pump::rpc {

    // Handler context passed to service handlers (Strategy A: unified type)
    // Owns its payload data (copied from ring buffer) for async handler safety
    struct handler_context {
        uint32_t request_id;
        uint16_t method_id;

        // Owned payload data (shared_ptr so moves don't invalidate pointers)
        std::shared_ptr<std::vector<uint8_t>> _data;

        handler_context(uint32_t rid, uint16_t mid, std::vector<uint8_t> data)
            : request_id(rid)
            , method_id(mid)
            , _data(std::make_shared<std::vector<uint8_t>>(std::move(data))) {
        }

        // Construct a payload_view on demand
        // The view is valid as long as handler_context (or a copy) is alive
        payload_view payload() const {
            thread_local iovec iov;
            iov = {_data->data(), _data->size()};
            return {&iov, 1, _data->size()};
        }
    };

    // Service base template: unregistered by default
    template<auto sid>
    struct service {
        static constexpr bool is_service = false;
    };

    // Concept: check if service T has handle(handler_context&&)
    template<typename T>
    concept has_handle = requires(handler_context&& ctx) {
        T::handle(std::move(ctx));
    };

    // Runtime service_id -> compile-time variant<service<s1>, service<s2>, ...>
    template<auto ...service_ids>
    auto
    get_service_by_id(auto sid) {
        using res_t = std::variant<service<service_ids>...>;
        std::optional<res_t> result;
        (void)((sid == service_ids
                && (result.emplace(service<service_ids>{}), true)) || ...);
        if (result)
            return result.value();
        throw method_not_found_error(static_cast<uint16_t>(sid));
    }

    // Dispatch operator: flat_map that receives handler_context
    // and routes to appropriate service via visit
    template<auto ...service_ids>
    auto
    dispatch() {
        return pump::sender::flat_map([](auto&& ctx_in) {
            handler_context ctx = std::move(ctx_in);
            auto method_id = ctx.method_id;
            return pump::sender::just()
                >> pump::sender::then([method_id]() {
                    return get_service_by_id<service_ids...>(method_id);
                })
                >> pump::sender::visit()
                >> pump::sender::flat_map([ctx = std::move(ctx)](auto&& svc) mutable {
                    using svc_t = std::decay_t<decltype(svc)>;
                    if constexpr (svc_t::is_service) {
                        if constexpr (has_handle<svc_t>)
                            return svc_t::handle(std::move(ctx));
                        else
                            return pump::sender::just_exception(
                                method_not_found_error(ctx.method_id));
                    } else {
                        return pump::sender::just_exception(
                            method_not_found_error(ctx.method_id));
                    }
                });
        });
    }

}

#endif //ENV_SCHEDULER_RPC_CHANNEL_DISPATCHER_HH
