
#ifndef ENV_COMMON_SESSION_HH
#define ENV_COMMON_SESSION_HH

#include <cstdint>
#include <tuple>
#include "pump/core/meta.hh"

namespace pump::scheduler::net {

    template<typename ...layer_t>
    struct session_t {
        std::tuple<layer_t...> _impl;

        template<typename ...args_t>
        requires (sizeof...(args_t) == sizeof...(layer_t))
        explicit session_t(args_t&&... layers)
            : _impl(std::forward<args_t>(layers)...) {}

        // invoke: first-match dispatch
        template<uint32_t index_v, typename tag_t, typename ...args_t>
        auto _invoke(const tag_t& tag, args_t&&... args) {
            static_assert(index_v < sizeof...(layer_t), "unhandled tag in session_t::invoke");
            if constexpr (requires { std::get<index_v>(_impl).invoke(tag, *this, __fwd__(args)...); })
                return std::get<index_v>(_impl).invoke(tag, *this, __fwd__(args)...);
            else
                return _invoke<index_v + 1>(tag, __fwd__(args)...);
        }

        template<typename tag_t, typename ...args_t>
        auto invoke(const tag_t& tag, args_t&&... args) {
            return _invoke<0>(tag, __fwd__(args)...);
        }

        // broadcast: all-match dispatch (lifecycle events, args must be copyable)
        template<uint32_t index_v = 0, typename tag_t, typename ...args_t>
        void broadcast(const tag_t& tag, const args_t&... args) {
            if constexpr (index_v < sizeof...(layer_t)) {
                if constexpr (requires { std::get<index_v>(_impl).invoke(tag, *this, args...); })
                    std::get<index_v>(_impl).invoke(tag, *this, args...);
                broadcast<index_v + 1>(tag, args...);
            }
        }
    };
}

#endif //ENV_COMMON_SESSION_HH
