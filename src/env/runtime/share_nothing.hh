
#ifndef ENV_SHARE_NOTHING_HH
#define ENV_SHARE_NOTHING_HH

#include <thread>
#include <vector>
#include <span>
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

    // ── run: per-core advance loop ──
    // on_init is called after this_core_id is set, before entering the loop.

    template <typename ...scheduler_t, typename on_init_t>
    auto
    run(global_runtime_t<scheduler_t...> *runtime, uint32_t core, on_init_t&& on_init) {
        pump::core::this_core_id = core;
        runtime->is_running_by_core[core].store(true);

        on_init(runtime, core);

        while (runtime->is_running_by_core[core].load()) [[likely]] {
            std::apply(
                [runtime](auto *... sche) {
                    if (!(... | (sche ? advance_one(runtime, sche) : false))) [[unlikely]]
                        std::this_thread::yield();
                },
                runtime->schedulers_by_core[core]
            );
        }
    }

    template <typename ...scheduler_t>
    auto
    run(global_runtime_t<scheduler_t...> *runtime, uint32_t core) {
        run(runtime, core, [](auto*, uint32_t){});
    }

    // ── Default thread launcher: std::thread + CPU affinity ──

    struct default_launcher {
        static void launch(uint32_t core, auto&& fn) {
            std::jthread([core, f = std::forward<decltype(fn)>(fn)]() mutable {
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                CPU_SET(core, &cpuset);
                pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
                f();
            }).detach();
        }
    };

    // ── start: launch worker threads + block on main core ──
    //
    // launcher_t::launch(core, fn) — how to create a thread for the given core.
    // on_init(runtime, core)       — per-core init before advance loop.
    // Main core runs on the calling thread (blocking).

    template <typename launcher_t = default_launcher, typename ...scheduler_t, typename on_init_t>
    auto
    start(global_runtime_t<scheduler_t...>* rs,
          std::span<const uint32_t> cores, uint32_t main_core,
          on_init_t&& on_init) {
        for (auto core : cores) {
            if (core == main_core) continue;
            launcher_t::launch(core, [rs, core, on_init]() {
                run(rs, core, on_init);
            });
        }
        run(rs, main_core, std::forward<on_init_t>(on_init));
    }

    // Convenience: no on_init, no custom launcher.
    template <typename ...scheduler_t>
    auto
    start(global_runtime_t<scheduler_t...>* rs,
          std::span<const uint32_t> cores, uint32_t main_core) {
        start(rs, cores, main_core, [](auto*, uint32_t){});
    }

    // Legacy: launch all hardware cores (all detached, non-blocking).
    template <typename ...scheduler_t>
    auto
    start(global_runtime_t<scheduler_t...>* rs) {
        const uint32_t max_core = rs->schedulers_by_core.size();
        for (uint32_t i = 0; i < max_core; ++i)
            default_launcher::launch(i, [rs, i]() { run(rs, i); });
    }
}

#endif //ENV_SHARE_NOTHING_HH
