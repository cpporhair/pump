#include <iostream>
#include <vector>
#include <string>

#include "pump/sender/flat.hh"
#include "pump/sender/repeat.hh"
#include "pump/sender/sequential.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/just.hh"
#include "pump/sender/on.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/get_context.hh"
#include "env/scheduler/task/tasks_scheduler.hh"
#include "env/runtime/share_nothing.hh"
#include "env/runtime/runner.hh"

#include "env/scheduler/nvme/sender.hh"
#include "nvme_impl.hh"

using namespace pump;
using namespace pump::sender;

using task_scheduler_t = scheduler::task::scheduler;
using nvme_scheduler_t = scheduler::nvme::scheduler<example::nvme::nvme_page>;

using runtime_schedulers = env::runtime::runtime_schedulers<
    task_scheduler_t,
    nvme_scheduler_t
>;

auto
create_runtime_schedulers() {
    return new runtime_schedulers();
}

auto
nvme_test_proc(const runtime_schedulers *rs) {
    return just()
        >> then([]() {
            // 准备测试数据
            example::nvme::page_list pl(1);
            // 这里为了演示，我们假设页面内存已经分配并填充了内容
            // 实际上在真实环境中需要 DMA 分配
            return pl;
        })
        >> flat_map([rs](example::nvme::page_list&& pl) {
            auto* scheduler = rs->get_schedulers<nvme_scheduler_t>()[0];
            return scheduler::nvme::put_page(pl.pages[0], scheduler);
        })
        >> then([](auto res) {
            if (res.status == 0) {
                std::cout << "NVMe Put Success" << std::endl;
            } else {
                std::cout << "NVMe Put Failed" << std::endl;
            }
            return res.page;
        })
        >> flat_map([rs](example::nvme::nvme_page* p) {
            auto* scheduler = rs->get_schedulers<nvme_scheduler_t>()[0];
            return scheduler::nvme::get_page(p, scheduler);
        })
        >> then([](auto res) {
            if (res.status == 0) {
                std::cout << "NVMe Get Success" << std::endl;
            }
            return just();
        })
        >> flat()
        >> any_exception([](std::exception_ptr e) {
            std::cerr << "Exception in nvme_test_proc" << std::endl;
            return just();
        });
}

int
main(int argc, char **argv) {
    just()
        >> get_context<runtime_schedulers *>()
        >> then([](runtime_schedulers *rs) {
            // 初始化 SSD (模拟)
            std::vector<scheduler::nvme::ssd_config> configs;
            std::vector<scheduler::nvme::ssd<example::nvme::nvme_page>*> ssds;
            // env::nvme::init_ssds(configs, ssds); // 实际上这里会尝试连接硬件

            return nvme_test_proc(rs)
                >> submit(core::make_root_context(rs));
        })
        >> get_context<runtime_schedulers *>()
        >> then([](runtime_schedulers *rs) {
            // 启动调度器循环
            env::runtime::start(rs->schedulers_by_core);
        })
        >> submit(core::make_root_context(create_runtime_schedulers()));

    return 0;
}
