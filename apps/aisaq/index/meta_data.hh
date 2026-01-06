#ifndef AISAQ_INDEX_META_DATA_HH
#define AISAQ_INDEX_META_DATA_HH

#pragma once

#include <cstdint>
#include <string>
#include <fstream>
#include <stdexcept>
#include <cstring> // for memset


namespace aisaq::index {
    constexpr uint64_t METADATA_MAGIC = 0x4C562D5141536941; // "AiSAQ-VL"
    constexpr uint32_t CURRENT_VERSION = 1;

    struct __attribute__((packed))
    index_metadata_pod {
        uint64_t magic;
        uint32_t version;

        uint64_t total_nodes;
        uint32_t max_degree;
        uint32_t dim;
        uint32_t pq_len;
        uint32_t sector_size;

        uint32_t entry_point_sector_id;

        uint8_t reserved[64];
    };

    class index_metadata {
    private:
        index_metadata_pod data_;

    public:
        index_metadata() {
            std::memset(&data_, 0, sizeof(index_metadata_pod));
            data_.magic = METADATA_MAGIC;
            data_.version = CURRENT_VERSION;
        }

        void
        save(const std::string &filename) const {
            std::ofstream file(filename, std::ios::binary | std::ios::trunc);
            if (!file.is_open()) {
                throw std::runtime_error("Failed to open metadata file for writing: " + filename);
            }

            file.write(reinterpret_cast<const char *>(&data_), sizeof(index_metadata_pod));

            if (!file.good()) {
                throw std::runtime_error("Write error occurred in metadata file.");
            }
            file.close();
        }

        void
        load(const std::string &filename) {
            std::ifstream file(filename, std::ios::binary);
            if (!file.is_open()) {
                throw std::runtime_error("Failed to open metadata file for reading: " + filename);
            }

            file.read(reinterpret_cast<char *>(&data_), sizeof(index_metadata_pod));

            if (!file.good()) {
                throw std::runtime_error("Read error or incomplete metadata file.");
            }
            file.close();

            validate();
        }

        void
        validate() const {
            if (data_.magic != METADATA_MAGIC) {
                throw std::runtime_error("Invalid metadata: Magic number mismatch.");
            }
            if (data_.version != CURRENT_VERSION) {
                throw std::runtime_error("Invalid metadata: Version mismatch.");
            }
        }

        void
        set_config(uint64_t total_nodes, uint32_t dim, uint32_t start_sector) {
            data_.total_nodes = total_nodes;
            data_.dim = dim;
            data_.entry_point_sector_id = start_sector;
            // 其他默认值...

        }

        uint64_t total_nodes() const { return data_.total_nodes; }
        uint32_t entry_point_sector_id() const { return data_.entry_point_sector_id; }
        uint32_t dim() const { return data_.dim; }
        uint32_t max_degree() const { return data_.max_degree; }
    };
}

#endif //AISAQ_INDEX_META_DATA_HH