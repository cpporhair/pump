
#ifndef ENV_SHARE_NOTHING_HH
#define ENV_SHARE_NOTHING_HH

#include <thread>
#include <vector>
#include <bits/move_only_function.h>

namespace pump::env::runtime {

    template <typename ...scheduler_t>
    auto
    run(std::atomic<bool>& running, scheduler_t* ...sche) {
        const static auto st = std::chrono::milliseconds(1);
        while (running.load()) [[likely]] {
            if (!(... | (sche ? sche->advance() : false))) [[unlikely]] {
                std::this_thread::sleep_for(st);
            }
        }
    }

    template <typename ...scheduler_t>
    auto
    start(std::vector<scheduler_t*>& ...by_core) {
        uint16_t cur_core = sched_getcpu();
        uint32_t max_core = std::max({by_core.size()...});
        for (uint16_t i = 0; i < max_core; ++i) {
            if (i == cur_core)
                continue;
            if (auto has_scheduler = (... || (by_core[i] ? true : false))) {
                std::thread([...s = by_core[i]]() {
                    std::atomic<bool> running(true);
                    run(running, s...);
                });
            }
        }
        if (auto has_scheduler = (... || (by_core[cur_core] ? true : false))) {
            std::atomic<bool> running(true);
            run(running, by_core[cur_core]...);
        }
    }

    template <typename ...scheduler_t>
    auto
    start(std::vector<std::tuple<scheduler_t*...>>& schedulers_by_core) {
        const uint32_t cur_core = sched_getcpu();
        const uint32_t max_core = schedulers_by_core.size();
        for (uint32_t i = 0; i < max_core; ++i) {
            if (i == cur_core)
                continue;
            std::apply(
                [](auto *... sche) {
                    if ((... && (sche == nullptr)))
                        return;
                    std::thread([...s = sche]() {
                        std::atomic<bool> running(true);
                        run(running, s...);
                    }).detach();
                },
                schedulers_by_core[i]
            );
        }

        std::apply(
            [](auto *... sche) {
                if ((... && (sche == nullptr)))
                    return;
                std::atomic<bool> running(true);
                run(running, sche...);
            },
            schedulers_by_core[cur_core]
        );
    }


    template <typename Runtime>
    auto
    start(Runtime* rs) {
        const uint32_t cur_core = sched_getcpu();
        const uint32_t max_core = rs->schedulers_by_core.size();
        for (uint32_t i = 0; i < max_core; ++i) {
            if (i == cur_core)
                continue;
            std::apply(
                [rs](auto *... sche) {
                    if ((... && (sche == nullptr)))
                        return;
                    std::thread([rs, ...s = sche]() {
                        std::atomic<bool> running(true);
                        run(running, rs, s...);
                    }).detach();
                },
                rs->schedulers_by_core[i]
            );
        }

        std::apply(
            [rs](auto *... sche) {
                if ((... && (sche == nullptr)))
                    return;
                std::atomic<bool> running(true);
                run(running, rs, sche...);
            },
            rs->schedulers_by_core[cur_core]
        );
    }
}

#endif //ENV_SHARE_NOTHING_HH