#ifndef PUMP_SENDER_REPEAT_HH
#define PUMP_SENDER_REPEAT_HH

#include <ranges>

#include "../core/helper.hh"

#include "../core/op_tuple_builder.hh"
#include "./for_each.hh"

namespace pump::sender {
    inline auto
    repeat(uint32_t count) {
        return for_each(std::views::iota(uint32_t(0),count));
    }

    inline auto
    forever() {
        return for_each(std::views::iota(uint32_t(0)));
    }
}

#endif //PUMP_SENDER_REPEAT_HH
