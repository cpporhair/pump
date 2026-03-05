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
#include "pump/sender/pop_context.hh"

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
        >> with_context(example::nvme::page_list(1))([rs]() {
            return get_context<example::nvme::page_list>()
                >> flat_map([rs](example::nvme::page_list &pl) {
                    auto *scheduler = rs->get_schedulers<nvme_scheduler_t>()[0];
                    return scheduler::nvme::put_page(pl.pages[0], scheduler);
                })
                >> then([](auto res) {
                    if (res) {
                        std::cout << "NVMe Put Success" << std::endl;
                    } else {
                        std::cout << "NVMe Put Failed" << std::endl;
                    }
                })
                >> get_context<example::nvme::page_list>()
                >> flat_map([rs](example::nvme::page_list &pl) {
                    auto *p = pl.pages[0];
                    auto *scheduler = rs->get_schedulers<nvme_scheduler_t>()[0];
                    return scheduler::nvme::get_page(p, scheduler);
                })
                >> then([](auto res) {
                    if (res == 0) {
                        std::cout << "NVMe Get Success" << std::endl;
                    }
                });
        })
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
            env::runtime::start(rs);
        })
        >> submit(core::make_root_context(create_runtime_schedulers()));

    return 0;
}
