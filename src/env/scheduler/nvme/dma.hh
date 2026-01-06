//

//

#ifndef ENV_SCHEDULER_NVME_DMA_HH
#define ENV_SCHEDULER_NVME_DMA_HH

#include "spdk/env.h"

namespace pump::scheduler::nvme {

    template <size_t ele_size>
    struct
    req_pool {
        static constexpr size_t size = ele_size;
        spdk_mempool* pool = nullptr;

        explicit
        req_pool(const char * name, uint64_t spdk_dma_pool_size) {
            init(name, spdk_dma_pool_size);
        }

        void
        init(const char * name, uint64_t spdk_dma_pool_size) {
            pool = spdk_mempool_create(name, spdk_dma_pool_size, size, 4096, SPDK_ENV_SOCKET_ID_ANY);
            for (uint32_t i = 0; i < 1024; ++i) {
                spdk_mempool_put(pool, malloc(size));
            }
        }

        [[nodiscard]]
        void*
        malloc_req() const {
            auto* res = spdk_mempool_get(pool);
            if (!res)[[unlikely]]{
                res = malloc(size);
            }

            return res;
        }

        void
        free_req(void* dma) {
            spdk_mempool_put(pool, dma);
        }
    };

    struct
    page_dma {
        spdk_mempool* global_pool;

        void
        init(spdk_mempool* pool, uint32_t data_page_size) {
            for (uint32_t i = 0; i < 1024; ++i) {
                spdk_mempool_put(pool, spdk_dma_malloc(data_page_size, data_page_size, nullptr));
            }
        }

        void
        init(uint32_t data_page_size, uint64_t spdk_dma_pool_size) {
            global_pool = spdk_mempool_create("global", spdk_dma_pool_size, data_page_size, 4096, SPDK_ENV_SOCKET_ID_ANY);
            init(global_pool, data_page_size);
        }

        [[nodiscard]]
        void*
        malloc(uint32_t data_page_size) const {
            auto* res = spdk_mempool_get(global_pool);
            if (!res)[[unlikely]]{
                res = spdk_dma_malloc(data_page_size, data_page_size, nullptr);
            }

            return res;
        }

        void
        free(void* dma) const {
            spdk_mempool_put(global_pool, dma);
        }
    };

    inline page_dma global_page_dma;

    inline
    auto
    malloc_page(uint32_t data_page_size) {
        return global_page_dma.malloc(data_page_size);
    }

    inline
    auto
    free_page(void* dma) {
        if (dma == nullptr)
            return;
        return global_page_dma.free(dma);
    }
}
#endif //ENV_SCHEDULER_NVME_DMA_HH
