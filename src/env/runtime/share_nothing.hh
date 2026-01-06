
#ifndef ENV_SHARE_NOTHING_HH
#define ENV_SHARE_NOTHING_HH

#include <thread>
#include <vector>
#include <bits/move_only_function.h>

namespace pump::env::runtime {
    
    struct
    scheduler_runner{
        std::atomic<bool> running = true;
        uint16_t core{};
        std::vector<std::move_only_function<bool()>> runners;
        auto
        operator () () {
            bool res = false;
            for (std::move_only_function<bool()>& runner : runners)
                res |= runner();
            return res;
        }
    };

    template <typename scheduler_t0>
    auto
    make_runner(std::vector<std::move_only_function<bool()>>& c,scheduler_t0* s) {
        if (s)
            c.push_back([s](){return s->advance();});
    }

    template <typename scheduler_t0, typename ...scheduler_t1>
    auto
    make_runner(std::vector<std::move_only_function<bool()>>& c,scheduler_t0* s,  scheduler_t1* ...others) {
        if (s)
            c.push_back([s](){return s->advance();});
        make_runner(c, others...);
    }

    template <typename ...scheduler_t>
    auto
    make_runner(scheduler_t* ...sche) {
        std::vector<std::move_only_function<bool()>> runners;
        make_runner(runners, sche...);
        return runners;
    }

    template <typename ...scheduler_t>
    auto
    make_runner(uint16_t max_core, scheduler_t** ...by_core) {
        std::vector<scheduler_runner> runners_by_core(max_core);
        for (uint16_t i = 0; i < max_core; i++) {
            runners_by_core[i] = make_runner(by_core[i]...);
        }
        return runners_by_core;
    }

    inline
    auto
    start(const std::vector<scheduler_runner*>&  runners) {
        const uint16_t this_core = sched_getcpu();
        scheduler_runner* this_core_runner =nullptr;
        for (scheduler_runner* runner : runners) {
            if (runner->core != this_core)
                std::thread([runner](){runner->operator()();});
            else
                this_core_runner = runner ;
        }

        if (this_core_runner)
            this_core_runner->operator()();
    }

    template <typename Runtime, typename ...scheduler_t>
    auto
    run(std::atomic<bool>& running, Runtime* rt, scheduler_t* ...sche) {
        const static auto st = std::chrono::milliseconds(1);
        while (running.load()) {
            if (!(... || (sche ? sche->advance(rt) : false))) {
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