
#ifndef ENV_SCHEDULER_TASK_SENDER_HH
#define ENV_SCHEDULER_TASK_SENDER_HH

#include "./tasks_scheduler.hh"
#include "pump/core/random.hh"

#ifdef __linux__
#include <sched.h>
#endif


namespace pump::scheduler::task {
    template <typename schedulers_t>
    inline
    auto
    any_scheduler(schedulers_t& list) {
        return list[pump::core::fast_random_uint32(std::size(list))];
    }

    inline
    auto
    schedule_at(scheduler* sche) {
        return sche->as_task();
    }

    inline
    auto
    schedule_preemptive_at(scheduler* sche) {
        return sche->as_preemptive_task();
    }

    inline auto
    delay(scheduler* sche, const uint64_t ms) {
        return sche->delay(ms);
    }
}

#endif //ENV_SCHEDULER_TASK_SENDER_HH