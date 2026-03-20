//
// Created by null on 25-4-11.
//

#ifndef ENV_SCHEDULER_NVME_NVME_PAGE_HH
#define ENV_SCHEDULER_NVME_NVME_PAGE_HH

#include <cstdint>
#include <concepts>

namespace pump::scheduler::nvme {
    constexpr static uint64_t nvme_page_size = 4096;

    template <typename page_t> struct ssd;

    template <typename T>
    concept page_concept = requires(T p) {
        { p.get_size() } -> std::convertible_to<uint64_t>;
        { p.get_pos() } -> std::convertible_to<uint64_t>;
        { p.get_payload() } -> std::convertible_to<void*>;
        { p.get_ssd_info() } -> std::convertible_to<const ssd<T>*>;
    };

}

#endif //ENV_SCHEDULER_NVME_NVME_PAGE_HH
