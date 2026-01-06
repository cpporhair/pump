#ifndef AISAQ_INDEX_NODE_DATA_HH
#define AISAQ_INDEX_NODE_DATA_HH
#pragma once

#include <cstdint>
#include <cstddef>

#include "../common/common.hh"

namespace aisaq::index {

    struct __attribute__((packed))
    neighbor_entry {
        uint32_t sector_id;
        uint8_t pq_code[common::PQ_LEN];
        uint8_t sector_count;
    };

    struct __attribute__((packed))
    node_header {
        uint32_t logical_id;
        uint32_t num_neighbors;
    };

    
    struct
    node_accessor {
    private:
        uint32_t size_;
        uint8_t *base_ptr_;

    public:
        explicit node_accessor(void *raw_memory, uint32_t size)
            : size_(size)
              , base_ptr_(reinterpret_cast<uint8_t *>(raw_memory)) {
        }

        [[nodiscard]]
        inline uint32_t
        get_logical_id() const {
            return reinterpret_cast<const node_header *>(base_ptr_)->logical_id;
        }

        [[nodiscard]]
        inline uint32_t
        get_num_neighbors() const {
            return reinterpret_cast<const node_header *>(base_ptr_)->num_neighbors;
        }

        [[nodiscard]]
        inline const float *
        get_vector() const {
            return reinterpret_cast<const float *>(base_ptr_ + sizeof(node_header));
        }

        [[nodiscard]]
        inline const neighbor_entry &
        get_neighbor(uint32_t index) const {
            const uint8_t *neighbor_start = base_ptr_
                + sizeof(node_header)
                + (common::DIM * sizeof(float));

            const auto *entry_ptr = reinterpret_cast<const neighbor_entry *>(neighbor_start);
            return entry_ptr[index];
        }

        [[nodiscard]]
        inline const neighbor_entry *
        get_neighbor_array_start() const {
            return reinterpret_cast<const neighbor_entry *>(
                base_ptr_ + sizeof(node_header) + (common::DIM * sizeof(float))
            );
        }

        [[nodiscard]]
        inline uint32_t
        get_size() const {
            return size_;
        }
    };
}

#endif //AISAQ_INDEX_NODE_DATA_HH