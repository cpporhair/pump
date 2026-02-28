
#ifndef ENV_RUNTIME_SCHEDULERS_HH
#define ENV_RUNTIME_SCHEDULERS_HH

#include <vector>
#include <tuple>
#include <cstdint>

#include "pump/core/meta.hh"

namespace pump::env::runtime {

    template<typename T, typename = std::void_t<>>
    struct get_preemption_queues {
        using type = std::tuple<>;
    };

    template<typename T>
    struct get_preemption_queues<T, std::void_t<typename T::preemption_queues>> {
        using type = typename T::preemption_queues;
    };

    template<typename... Queues>
    struct runtime_preemption_queues_base {
        std::tuple<Queues...> queues;

        template<typename T>
        [[nodiscard]]
        auto* get() {
            return &std::get<T>(queues);
        }

        template<typename T>
        [[nodiscard]]
        const auto* get() const {
            return &std::get<T>(queues);
        }
    };

    template<typename T>
    struct runtime_preemption_queues_impl;

    template<typename... Queues>
    struct runtime_preemption_queues_impl<std::tuple<Queues...>> : runtime_preemption_queues_base<Queues...> {};

    template<typename... Schedulers>
    using runtime_preemption_queues = runtime_preemption_queues_impl<
        pump::core::unique_t<
            pump::core::concat_t<typename get_preemption_queues<Schedulers>::type...>
        >
    >;

    template<typename... Schedulers>
    struct runtime_schedulers_base {
        std::vector<std::tuple<Schedulers *...> > schedulers_by_core;
        std::tuple<std::vector<Schedulers*>...> schedulers;
        runtime_preemption_queues<Schedulers...> preemption_queues;

        explicit
        runtime_schedulers_base()
            : schedulers_by_core(std::thread::hardware_concurrency()) {
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
        add_core_schedulers(uint32_t core, Schedulers*... scheds) {
            assert(core < schedulers_by_core.size());
            assert(std::apply([](auto *... sche) { return (... && (sche == nullptr)); }, schedulers_by_core[core]));
            schedulers_by_core[core] = std::tuple<Schedulers *...>{scheds...};
            (std::get<std::vector<Schedulers *> >(schedulers).push_back(scheds), ...);
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
