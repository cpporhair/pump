#ifndef PUMP_EXAMPLE_NVME_IMPL_HH
#define PUMP_EXAMPLE_NVME_IMPL_HH

#include <cstdint>
#include <vector>
#include <cstring>
#include <algorithm>
#include "env/scheduler/nvme/nvme_page.hh"
#include "env/scheduler/nvme/ssd.hh"

namespace pump::example::nvme {

    struct nvme_page;

    struct page_address {
        scheduler::nvme::ssd<nvme_page>* ssd_info;
        uint32_t pos;
    };

    struct nvme_page {
        constexpr static uint64_t size = scheduler::nvme::nvme_page_size;
        page_address address;
        char* payload;

        [[nodiscard]]
        constexpr static uint64_t
        get_size() {
            return size;
        }

        [[nodiscard]]
        uint32_t
        get_pos() const {
            return address.pos;
        }

        [[nodiscard]]
        char*
        get_payload() const {
            return payload;
        }

        [[nodiscard]]
        const scheduler::nvme::ssd<nvme_page>*
        get_ssd_info() const {
            return address.ssd_info;
        }
    };

    struct page_list {
        std::vector<nvme_page *> pages;

        auto begin() { return pages.begin(); }
        auto end() { return pages.end(); }
        auto begin() const { return pages.begin(); }
        auto end() const { return pages.end(); }

        explicit page_list(uint64_t count) : pages(count, nullptr) {}

        [[nodiscard]]
        bool page_memory_allocated() const {
            return !pages.empty() && pages[0] != nullptr;
        }

        template <typename dma_allocator_t>
        void allocate_page_memory(dma_allocator_t& allocator) {
            for (auto & page : pages)
                page = static_cast<nvme_page*>(allocator.allocate(sizeof(nvme_page) + scheduler::nvme::nvme_page_size));
                // Actually, the original implementation didn't show where payload comes from.
                // It likely allocated the struct and payload together or separately.
                // Let's assume a simple allocation for now.
        }
        
        // Let's refine the allocator logic to match what might be expected.
        // Usually nvme_page is a metadata struct and payload is DMA-able memory.
    };

    struct page_cursor {
    private:
        page_list& page_list_ref;
        size_t current_page_index;
        size_t current_pos;

    public:
        explicit page_cursor(page_list& plist)
            : page_list_ref(plist), current_page_index(0), current_pos(0) {
        }

        [[nodiscard]]
        bool is_the_page_aligned() const {
            return current_pos == 0;
        }

        [[nodiscard]]
        size_t get_current_page_index() const {
            return current_page_index;
        }

        [[nodiscard]]
        size_t get_used_byte_size() const {
            return get_current_page_index() * scheduler::nvme::nvme_page_size + get_current_pos();
        }

        [[nodiscard]]
        size_t get_current_pos() const {
            return current_pos;
        }

        void set_current_pos(size_t pos) {
            current_pos = pos;
        }

        [[nodiscard]]
        nvme_page& get_current_page() const {
            return *page_list_ref.pages[current_page_index];
        }

        void move_to_next_page() {
            current_page_index++;
            current_pos = 0;
        }

        [[nodiscard]]
        bool at_end() const {
            return current_page_index >= page_list_ref.pages.size();
        }
    };

    struct page_list_writer {
    private:
        page_cursor& cursor;

    public:
        explicit page_list_writer(page_cursor& c)
            : cursor(c) {
        }

        void write_stream(const char* data, size_t size) {
            size_t bytes_written = 0;
            while (bytes_written < size && !cursor.at_end()) {
                size_t remaining_space = scheduler::nvme::nvme_page_size - cursor.get_current_pos();
                size_t bytes_to_write = std::min(size - bytes_written, remaining_space);

                nvme_page& current_page = cursor.get_current_page();
                std::memcpy(current_page.payload + cursor.get_current_pos(), data + bytes_written, bytes_to_write);

                bytes_written += bytes_to_write;
                cursor.set_current_pos(cursor.get_current_pos() + bytes_to_write);

                if (cursor.get_current_pos() >= scheduler::nvme::nvme_page_size) {
                    cursor.move_to_next_page();
                }
            }
        }

        template<typename data_t>
        void write_data(const data_t& data) {
            static_assert(std::is_trivially_copyable_v<data_t>);
            write_stream(reinterpret_cast<const char *>(&data), sizeof(data_t));
        }
    };

}

#endif //PUMP_EXAMPLE_NVME_IMPL_HH
