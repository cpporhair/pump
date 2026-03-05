
#ifndef ENV_SHARE_NOTHING_HH
#define ENV_SHARE_NOTHING_HH

#include <thread>
#include <vector>
#include <bits/move_only_function.h>

#include "./runner.hh"
#include "pump/core/lock_free_queue.hh"

namespace pump::env::runtime {
    template <typename ...scheduler_t>
    using global_runtime_t = runtime_schedulers_impl<std::tuple<scheduler_t...>>;

    template <typename runtime_t, typename scheduler_t>
    auto
    advance_one(runtime_t* runtime, scheduler_t* sche) {
        if constexpr (requires { sche->advance(*runtime); })
            return sche->advance(*runtime);
        else
            return sche->advance();
    }

    template <typename ...scheduler_t>
    auto
    run(global_runtime_t<scheduler_t...> *runtime, uint32_t core) {
        pump::core::this_core_id = core;
        const static auto st = std::chrono::milliseconds(1);
        runtime->is_running_by_core[core].store(true);
        while (runtime->is_running_by_core[core].load()) [[likely]] {
            std::apply(
                [runtime](auto *... sche) {
                    if (!(... | (sche ? advance_one(runtime, sche) : false))) [[unlikely]]
                        std::this_thread::sleep_for(st);
                },
                runtime->schedulers_by_core[core]
            );
        }
    }

    template <typename ...scheduler_t>
    auto
    start(global_runtime_t<scheduler_t...>* rs) {
        const uint32_t cur_core = sched_getcpu();
        const uint32_t max_core = rs->schedulers_by_core.size();
        for (uint32_t i = 0; i < max_core; ++i) {
            if (i == cur_core)
                continue;
            std::jthread(run<scheduler_t...>, rs, i).detach();
        }

        std::jthread(run<scheduler_t...>, rs, cur_core).detach();
    }
}

#endif //ENV_SHARE_NOTHING_HH