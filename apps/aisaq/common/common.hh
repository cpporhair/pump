
#ifndef AISAQ_COMMON_COMMON_HH
#define AISAQ_COMMON_COMMON_HH
#include <cstdint>

namespace aisaq::common {
    constexpr uint32_t DIM = 128;
    constexpr uint32_t PQ_LEN = 16;
    constexpr uint32_t SECTOR_SIZE = 4096;

    struct page;

    struct
    page_address {
        uint32_t pos;
        pump::scheduler::nvme::ssd<page>* ssd_info;
    };

    struct
    slice {
        uint32_t size;
        void* data;
    };


    struct
    page {
        page_address address;
        slice data;

        page(page_address a, slice p)
            : address(a), data(p) {
        }

        page(uint32_t pos, pump::scheduler::nvme::ssd<page> *ssd_info, uint32_t size, void *data)
            : address{pos, ssd_info}, data{size, data} {
        }

        [[nodiscard]]
        uint64_t get_size() const { return data.size; }

        [[nodiscard]]
        uint64_t get_pos() const { return address.pos; }


        [[nodiscard]]
        void *get_payload() const { return data.data; }

        [[nodiscard]]
        const pump::scheduler::nvme::ssd<page> *get_ssd_info() const { return address.ssd_info; }
    };
}

#endif //AISAQ_COMMON_COMMON_HH