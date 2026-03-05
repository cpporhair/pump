
#ifndef ENV_RUNTIME_SCHEDULERS_HH
#define ENV_RUNTIME_SCHEDULERS_HH

#include <vector>
#include <tuple>
#include <cstdint>
#include <thread>
#include <cassert>

#include "pump/core/meta.hh"
#include "pump/core/random.hh"

namespace pump::env::runtime {

    template<typename T, typename = std::void_t<>>
    struct get_preemptive_scheduler {
        using type = std::tuple<>;
    };

    template<typename T>
    struct get_preemptive_scheduler<T, std::void_t<typename T::preemptive_scheduler_t>> {
        using type = std::tuple<typename T::preemptive_scheduler_t>;
    };

    template<typename... scheduler_t>
    struct runtime_preemption_queues_base {
        std::tuple<scheduler_t...> schedulers;

        template<typename T>
        [[nodiscard]]
        auto *
        get() {
            return &std::get<T>(schedulers);
        }

        template<typename T>
        [[nodiscard]]
        const auto *
        get() const {
            return &std::get<T>(schedulers);
        }

        template<typename T>
        consteval static auto
        has_scheduler() {
            if constexpr (requires { T::preemptive_scheduler_t; })
                return true;
            else
                return false;
        }
    };

    template<typename T>
    struct runtime_preemption_queues_impl;

    template<typename... Queues>
    struct runtime_preemption_queues_impl<std::tuple<Queues...>> : runtime_preemption_queues_base<Queues...> {};

    template<typename... Schedulers>
    using runtime_preemption_queues = runtime_preemption_queues_impl<
        pump::core::unique_t<
            pump::core::concat_t<typename get_preemptive_scheduler<Schedulers>::type...>
        >
    >;

    template<typename... scheduler_t>
    struct runtime_schedulers_base {
        std::vector<std::tuple<scheduler_t *...> > schedulers_by_core;
        std::tuple<std::vector<scheduler_t*>...> schedulers;
        runtime_preemption_queues<scheduler_t...> preemption_queues;
        std::vector<std::atomic<bool>> is_running_by_core;

        explicit
        runtime_schedulers_base()
            : schedulers_by_core(std::thread::hardware_concurrency())
            , is_running_by_core(std::thread::hardware_concurrency()){
            for (auto &i : is_running_by_core)
                i.store(false);
        }

        template<typename T>
        [[nodiscard]]
        auto get_by_core(uint32_t core) const {
            return std::get<T*>(schedulers_by_core[core]);
        }

        template<typename T>
        [[nodiscard]]
        auto get_by_index(uint32_t index) const {
            return std::get<std::vector<T*>>(schedulers)[index];
        }

        template<typename T>
        auto& get_schedulers() {
            return std::get<std::vector<T*>>(schedulers);
        }

        template<typename T>
        const auto& get_schedulers() const {
            return std::get<std::vector<T*>>(schedulers);
        }

        template<typename T>
        [[nodiscard]]
        auto* get_preemption_queue() {
            return preemption_queues.template get<T>();
        }

        template<typename T>
        [[nodiscard]]
        const auto* get_preemption_queue() const {
            return preemption_queues.template get<T>();
        }

        void
        add_core_schedulers(uint32_t core, scheduler_t*... scheds) {
            assert(core < schedulers_by_core.size());
            assert(std::apply([](auto *... sche) { return (... && (sche == nullptr)); }, schedulers_by_core[core]));
            schedulers_by_core[core] = std::tuple<scheduler_t *...>{scheds...};
            (std::get<std::vector<scheduler_t *> >(schedulers).push_back(scheds), ...);
        }

        template <typename T>
        auto
        any_scheduler() {
            if constexpr (requires { typename T::preemptive_scheduler_t; }) {
                return preemption_queues.template get<typename T::preemptive_scheduler_t>();
            } else {
                auto &list = get_schedulers<T>();
                return list[pump::core::fast_random_uint32(std::size(list))];
            }
        }
    };

    template<typename T>
    struct runtime_schedulers_impl;

    template<typename... Schedulers>
    struct runtime_schedulers_impl<std::tuple<Schedulers...>> : runtime_schedulers_base<Schedulers...> {};

    template<typename... Schedulers>
    using runtime_schedulers = runtime_schedulers_impl<pump::core::unique_t<std::tuple<Schedulers...>>>;
}

#endif //ENV_RUNTIME_SCHEDULERS_HH
