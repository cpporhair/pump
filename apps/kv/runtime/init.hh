//

//

#ifndef APPS_KV_RUNTIME_INIT_HH
#define APPS_KV_RUNTIME_INIT_HH

#include "./nvme.hh"
#include "./scheduler_objects.hh"
#include "env/scheduler/nvme/dma.hh"
#include "env/scheduler/nvme/scheduler.hh"

namespace apps::kv::runtime {
    inline auto
    init_ssd() {
        for (uint32_t i = 0; i < config.io.ssds.size(); ++i) {
            auto* s = new data::ssd();
            s->config = config.io.ssds[i];
            s->index_on_config = i;
            ssds.ssds.push_back(s);
        }

        spdk_nvme_transport_id trid{};
        spdk_nvme_trid_populate_transport(&trid, SPDK_NVME_TRANSPORT_PCIE);

        if (spdk_nvme_probe(&trid, nullptr, probe_cb, attach_cb, nullptr) < 0)
            throw data::init_env_failed("Unable to initialize SPDK in spdk_nvme_probe");

        for (auto *ssd : ssds.ssds) {
            if (!ssd->nvme_dev)
                continue;
            spdk_nvme_io_qpair_opts opts{};
            spdk_nvme_ctrlr_get_default_io_qpair_opts(ssd->ctrlr, &opts, sizeof(spdk_nvme_io_qpair_opts));
            opts.delay_cmd_submit = false;
            opts.create_only = false;

            std::vector<spdk_nvme_qpair*> qpairs;
            qpairs.reserve(ssd->config.qpair_count);
            for (uint32_t i = 0; i < ssd->config.qpair_count; ++i)
                qpairs.emplace_back(spdk_nvme_ctrlr_alloc_io_qpair(
                    ssd->ctrlr,
                    &opts,
                    sizeof(spdk_nvme_io_qpair_opts)
                ));

            if (ssd->config.used_core.empty())
            {
                for (auto *raw_qp : qpairs)
                    spdk_nvme_ctrlr_free_io_qpair(raw_qp);
                continue;
            }

            size_t core_index = 0;
            for (auto *raw_qp : qpairs) {
                auto core = ssd->config.used_core[core_index % ssd->config.used_core.size()];
                if (!ssd->nvme_qpairs_by_core[core]) {
                    auto *qpr = new pump::scheduler::nvme::qpair<data::data_page>{
                        raw_qp,
                        core,
                        ssd->config.qpair_depth,
                        0,
                        ssd->nvme_dev
                    };
                    ssd->nvme_qpairs_by_core[core] = qpr;
                } else {
                    spdk_nvme_ctrlr_free_io_qpair(raw_qp);
                }
                core_index++;
            }
        }
    }

    inline auto
    init_allocator() {
        uint32_t ssd_index = 0;
        for (auto *ssd : ssds.ssds) {
            ssd->allocator = new data::slot_manager(
                ssd->sn,
                ssd_index++,
                ssd->nvme_dev,
                ssd->sector_size * ssd->max_sector / runtime::data_page_size
            );
        }
    }

    inline auto
    init_indexes() {
        for (auto c : runtime::config.index.used_core) {
            all_index.by_core[c] = new data::b_tree;
            all_index.list.emplace_back(all_index.by_core[c]);
        }
    }

    inline auto
    init_schedulers() {

        for (auto *ssd : ssds.ssds) {
            nvme_schedulers_per_ssd.push_back(nvme_scheduler_list(ssd->sn));
            for (auto c: ssd->config.used_core) {
                if (!ssd->nvme_qpairs_by_core[c])
                    continue;
                auto *sche = new pump::scheduler::nvme::scheduler<data::data_page>(
                    ssd->nvme_qpairs_by_core[c]
                );
                nvme_schedulers_per_ssd.back().by_core[c] = sche;
                nvme_schedulers_per_ssd.back().list.push_back(sche);
            }
        }


        for (auto c : runtime::config.batch.used_core) {
            batch_schedulers.by_core[c] = new batch::scheduler(c);
            batch_schedulers.list.emplace_back(batch_schedulers.by_core[c]);
        }

        for (auto c : runtime::config.index.used_core) {
            index_schedulers.by_core[c] = new index::scheduler(c, all_index.by_core[c]);
            index_schedulers.list.emplace_back(index_schedulers.by_core[c]);
        }

        for (auto *ssd : ssds.ssds) {
            fs_schedulers.by_core[ssd->config.fs_core] = new fs::scheduler(ssd->config.fs_core, ssd);
            fs_schedulers.list.emplace_back(fs_schedulers.by_core[ssd->config.fs_core]);
        }

        for (auto c = spdk_env_get_first_core(); c != UINT32_MAX; c = spdk_env_get_next_core(c)){
            task_schedulers.by_core[c] = new pump::scheduler::task::scheduler(c);
            task_schedulers.list.emplace_back(task_schedulers.by_core[c]);
        }
    }

    inline auto
    init_core_mask() {
        std::stringstream ss;
        ss << std::hex << runtime::config.compute_core_mask();
        std::cout << ss.str() << std::endl;
        return ss.str();
    }

    inline auto
    init_proc_env() {
        std::string core_mask_string = init_core_mask();

        spdk_env_opts opts;
        spdk_env_opts_init(&opts);
        opts.name = "monism";
        opts.core_mask = core_mask_string.c_str();

        if (spdk_env_init(&opts) < 0)
            throw data::init_env_failed("Unable to initialize SPDK in spdk_env_init");
        pump::scheduler::nvme::global_page_dma.init(runtime::data_page_size, runtime::spdk_dma_pool_size);
    }

    inline auto
    init_config(std::string path) {
        return runtime::read_config_from_file(config, path);
    }

    inline auto
    make_config(std::string path) {
        db_config c;
        runtime::read_config_from_file(c, path);
        return c;
    }
}

#endif //APPS_KV_RUNTIME_INIT_HH
