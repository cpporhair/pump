#ifndef ENV_SCHEDULER_NVME_SSD_HH
#define ENV_SCHEDULER_NVME_SSD_HH

#include <vector>
#include <algorithm>
#include <cstring>
#include "spdk/nvme.h"
#include "nvme_page.hh"

namespace pump::scheduler::nvme {
    struct
    ssd_config {
        char* sn;
        uint32_t qpair_count;
        uint32_t qpair_depth;
        std::vector<uint32_t> rw_cores;
    };

    template <page_concept page_t>
    struct scheduler;

    template <typename page_t>
    struct
    ssd {
        const char*         sn{};
        spdk_nvme_ctrlr*    ctrlr{};
        spdk_nvme_ns*       ns{};
        uint32_t            sector_size{};
        uint64_t            max_sector{};
        uint32_t            cfg_qpair_count{};
        uint32_t            cfg_qpair_depth{};
    };

    template <page_concept page_t>
    struct
    qpair {
        spdk_nvme_qpair* impl;
        uint32_t core;
        uint32_t depth;
        uint32_t used;
        const ssd<page_t>* owner;

        [[nodiscard]]
        bool
        busy() const {
            return used > depth;
        }

        [[nodiscard]]
        bool
        empty() const {
            return used == 0;
        }
    };

    template <page_concept page_t>
    static
    bool
    probe_cb(void *cb_ctx, const spdk_nvme_transport_id *trid, spdk_nvme_ctrlr_opts *opts) {
        if(trid->trtype != SPDK_NVME_TRANSPORT_PCIE)
            return false;
        auto* ssds = static_cast<std::vector<ssd<page_t>*>*>(cb_ctx);
        return std::ranges::any_of(*ssds, [&](auto *ctx) {
            if (strcmp(ctx->sn, trid->traddr) == 0) {
                opts->num_io_queues = ctx->cfg_qpair_count;
                opts->io_queue_size = UINT16_MAX;
                return true;
            }
            return false;
        });
    }

    template <page_concept page_t>
    static
    auto
    attach_cb(void *cb_ctx, const spdk_nvme_transport_id *trid, spdk_nvme_ctrlr *ctrlr, const spdk_nvme_ctrlr_opts *opts ){
        if (spdk_nvme_ctrlr_reset(ctrlr) < 0) {
            throw std::logic_error("spdk_nvme_ctrlr_reset return failed");
        }
        auto* ssds = static_cast<std::vector<ssd<page_t>*>*>(cb_ctx);
        if (!std::ranges::any_of(*ssds, [&](auto *ctx) {
            if (strcmp(ctx->sn, trid->traddr) == 0) {
                ctx->ctrlr = ctrlr;
                ctx->ns = spdk_nvme_ctrlr_get_ns(ctrlr, spdk_nvme_ctrlr_get_first_active_ns(ctrlr));
                ctx->sector_size = spdk_nvme_ns_get_sector_size(ctx->ns);
                ctx->max_sector = spdk_nvme_ns_get_num_sectors(ctx->ns);
                ctx->cfg_qpair_count = std::min(ctx->cfg_qpair_count , opts->num_io_queues);
                return true;
            }
            return false;
        })) {
            throw std::logic_error("spdk_nvme_ctrlr_reset return failed");
        }
    }

    static
    void
    create_qpairs() {

    }

    template <typename ssd_config_t>
    concept ssd_config_concept = requires(const ssd_config_t& config) {
        { config.sn } -> std::convertible_to<char*>;
        { config.qpair_count } -> std::convertible_to<uint32_t>;
        { config.qpair_depth } -> std::convertible_to<uint32_t>;
    };

    template <ssd_config_concept ssd_config_t, page_concept page_t>
    static
    auto
    init_ssds(const std::vector<ssd_config_t>& configs, std::vector<ssd<page_t>*>& res) {
        for (const auto &config : configs) {
            auto *ctx = new ssd<page_t>;
            ctx->sn = config.sn;
            ctx->cfg_qpair_count = config.qpair_count;
            ctx->cfg_qpair_depth = config.qpair_depth;
            res.push_back(ctx);
        }

        spdk_nvme_transport_id trid{};
        spdk_nvme_trid_populate_transport(&trid, SPDK_NVME_TRANSPORT_PCIE);

        if (0 != spdk_nvme_probe(&trid, &res, probe_cb<page_t>, attach_cb<page_t>, nullptr)) {
            throw std::runtime_error("spdk_nvme_probe failed");
        }
    }
}

#endif //ENV_SCHEDULER_NVME_SSD_HH