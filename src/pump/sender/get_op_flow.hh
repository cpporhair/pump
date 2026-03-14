#ifndef PUMP_SENDER_GET_OP_FLOW_HH
#define PUMP_SENDER_GET_OP_FLOW_HH

#include "../core/bind_back.hh"
#include "../core/scope.hh"
#include "../core/op_pusher.hh"
#include "../core/compute_sender_type.hh"

namespace pump::sender {
    namespace _get_op_flow {
        struct
        op {
            constexpr static bool get_op_flow_op = true;
            void reset() {}
        };

        template <typename prev_t>
        struct
        sender {
            using prev_type = prev_t;
            prev_t prev;

            explicit
            sender(prev_t&& s)
                :prev(__fwd__(s)){
            }

            sender(sender&& o)noexcept
                :prev(__fwd__(o.prev)){
            }

            inline
            auto
            make_op() {
                return op{};
            }

            template<typename context_t>
            auto
            connect(){
                return prev.template connect<context_t>().push_back(make_op());
            }
        };

        struct
        fn {
            template <typename sender_t>
            constexpr
            decltype(auto)
            operator ()(sender_t&& sender) const {
                return _get_op_flow::sender<sender_t>{ __fwd__(sender) };
            }

            decltype(auto)
            operator ()() const {
                return ::pump::core::bind_back<fn>(fn{});
            }
        };
    }

    constexpr inline _get_op_flow::fn get_op_flow{};
}

namespace pump::core {

    template<typename context_t, typename sender_t>
    struct
    compute_sender_type<context_t, ::pump::sender::_get_op_flow::sender<sender_t>> {

        consteval static auto
        get_sender_value_class() {
            return compute_sender_type<context_t, sender_t>{};
        }

        consteval static uint32_t
        count_prev_value() {
            return decltype(get_sender_value_class())::count_value();
        }

        consteval static uint32_t
        count_value() {
            if constexpr (count_prev_value() == 0)
                return 1;
            else
                return 2;
        }

        consteval static auto
        get_value_type_identity() {
            static_assert(false, "未实现");
        }
    };
}

#endif //PUMP_SENDER_GET_OP_FLOW_HH
