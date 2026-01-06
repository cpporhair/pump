#ifndef APPS_KV_TASK_SCHEDULER_OBJECTS_HH
#define APPS_KV_TASK_SCHEDULER_OBJECTS_HH

#include "../batch/scheduler.hh"
#include "../index/scheduler.hh"
#include "../fs/scheduler.hh"

#include "./config.hh"
#include "env/scheduler/task/tasks_scheduler.hh"

namespace apps::kv::runtime {
    struct
    task_scheduler_list {
        pump::scheduler::task::scheduler** by_core;
        std::vector<pump::scheduler::task::scheduler*> list{};
        task_scheduler_list()
            : by_core(new pump::scheduler::task::scheduler* [std::thread::hardware_concurrency()]{nullptr}) {
        }
    };

    inline task_scheduler_list task_schedulers;

    struct
    batch_scheduler_list {
        batch::scheduler** by_core;
        std::vector<batch::scheduler*> list{};
        batch_scheduler_list()
            : by_core(new batch::scheduler* [std::thread::hardware_concurrency()]{nullptr}) {
        }
    };

    inline batch_scheduler_list batch_schedulers;

    struct
    index_scheduler_list {
        index::scheduler** by_core;
        std::vector<index::scheduler*> list{};
        index_scheduler_list()
            : by_core(new index::scheduler* [std::thread::hardware_concurrency()]{nullptr}) {
        }
    };

    inline index_scheduler_list index_schedulers;

    struct
    fs_scheduler_list {
        fs::scheduler** by_core;
        std::vector<fs::scheduler*> list{};
        fs_scheduler_list()
            : by_core(new fs::scheduler* [std::thread::hardware_concurrency()]{nullptr}) {
        }
    };

    inline fs_scheduler_list fs_schedulers;

    struct
    nvme_scheduler_list {
        const char* ssd_sn;
        pump::scheduler::nvme::scheduler<data::data_page>** by_core;
        std::vector<pump::scheduler::nvme::scheduler<data::data_page>*> list{};
        explicit
        nvme_scheduler_list(const char* sn)
            : ssd_sn(sn)
            , by_core(new pump::scheduler::nvme::scheduler<data::data_page>* [std::thread::hardware_concurrency()]{nullptr}) {
        }
    };

    inline std::vector<nvme_scheduler_list> nvme_schedulers_per_ssd;

    inline
    auto
    random_nvme_scheduler(uint32_t ssd_index) {
        return runtime::nvme_schedulers_per_ssd[ssd_index]
            .list[spdk_get_ticks() % runtime::nvme_schedulers_per_ssd[ssd_index].list.size()];
    }
}

#endif //APPS_KV_TASK_SCHEDULER_OBJECTS_HH
