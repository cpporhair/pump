//

//

#ifndef APPS_KV_RUNTIME_NVME_HH
#define APPS_KV_RUNTIME_NVME_HH

#include "../data/ssd.hh"
#include "env/scheduler/nvme/ssd.hh"

#include "./scheduler_objects.hh"

namespace apps::kv::runtime {
    struct
    ssd_list {
        std::vector<data::ssd*> ssds;

        data::ssd*
        find_ssd_by_address(const char* pcie_address) {
            for(auto* s : ssds) {
                if(s->config.name == pcie_address)
                    return s;
            }
            return nullptr;
        }

        data::ssd*
        find_ssd_by_sn(const char* sn) {
            for(auto* s : ssds) {
                if(s->sn == sn)
                    return s;
                bool b = true;
                for (uint32_t i = 0; i < 20; ++i) {
                    if (sn[i] != s->sn[i]) {
                        b = false;
                        break;
                    }
                }
                if (b)
                    return s;
            }
            return nullptr;
        }
    };

    ssd_list ssds;

    inline bool
    probe_cb(void *cb_ctx, const spdk_nvme_transport_id *trid, spdk_nvme_ctrlr_opts *opts) {
        if(trid->trtype != SPDK_NVME_TRANSPORT_PCIE)
            return false;
        auto* s = ssds.find_ssd_by_address(trid->traddr);
        if (s) {
            opts->num_io_queues = s->config.qpair_count;
            opts->io_queue_size = UINT16_MAX;
            return true;
        }
        return false;
    }

    inline auto
    attach_cb(void *cb_ctx, const spdk_nvme_transport_id *trid, spdk_nvme_ctrlr *ctrlr, const spdk_nvme_ctrlr_opts *opts ){
        int i = spdk_nvme_ctrlr_reset(ctrlr);
        if (i < 0)
            throw data::init_env_failed("spdk_nvme_ctrlr_reset return failed");

        auto* ssd = ssds.find_ssd_by_address(trid->traddr);
        if(!ssd)
            throw data::init_env_failed("cant found ssd");
        auto* cdata = spdk_nvme_ctrlr_get_data(ctrlr);
        ssd->sn = (const char*)cdata->sn;
        ssd->ctrlr = ctrlr;
        ssd->ns = spdk_nvme_ctrlr_get_ns(ctrlr,spdk_nvme_ctrlr_get_first_active_ns(ctrlr));
        ssd->sector_size = spdk_nvme_ns_get_sector_size(ssd->ns);
        ssd->max_sector = spdk_nvme_ns_get_num_sectors(ssd->ns);
        ssd->config.qpair_count = std::min(ssd->config.qpair_count , opts->num_io_queues);
        if (!ssd->nvme_dev)
            ssd->nvme_dev = new pump::scheduler::nvme::ssd<data::data_page>();
        ssd->nvme_dev->sn = ssd->sn;
        ssd->nvme_dev->ctrlr = ctrlr;
        ssd->nvme_dev->ns = ssd->ns;
        ssd->nvme_dev->sector_size = ssd->sector_size;
        ssd->nvme_dev->max_sector = ssd->max_sector;
        ssd->nvme_dev->cfg_qpair_count = ssd->config.qpair_count;
        ssd->nvme_dev->cfg_qpair_depth = ssd->config.qpair_depth;
    }
}

#endif //APPS_KV_RUNTIME_NVME_HH
