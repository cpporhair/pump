#ifndef ENV_SCHEDULER_TASKS_SCHEDULER_HH
#define ENV_SCHEDULER_TASKS_SCHEDULER_HH

#include <cstdint>
#include <bits/move_only_function.h>
#include <functional>
#include <set>
#include <vector>
#include <mutex>
#include <algorithm>
#include <chrono>

#ifdef __linux__
#include <numa.h>
#endif

#include "pump/core/lock_free_queue.hh"
#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"

namespace pump::scheduler::task {
    namespace _tasks {
        struct
        req {
            std::move_only_function<void()> cb;
        };

        template<typename scheduler_t>
        struct
        op {
            constexpr static bool scheduler_task_op = true;
            scheduler_t *scheduler;
            bool preemptive = false;

            template<uint32_t pos, typename context_t, typename scope_t>
            auto
            start(context_t& context, scope_t& scope) {
                auto* r = new req {
                    [context = context, scope = scope]() mutable {
                        core::op_pusher<pos + 1, scope_t>::push_value(context, scope);
                    }
                };

                if (preemptive) {
                    using Qs = typename scheduler_t::preemption_queues;
                    if constexpr (std::tuple_size_v<Qs> > 0) {
                        using Q = std::tuple_element_t<0, Qs>;
                        if constexpr (requires { context.template get_preemption_queue<Q>(); }) {
                            if (auto* q = context.template get_preemption_queue<Q>()) {
                                q->schedule(r);
                                return;
                            }
                        }
                    }
                }
                scheduler->schedule(r);
            }
        };

        template<typename scheduler_t>
        struct
        sender {
        public:
            explicit sender(scheduler_t *schd, bool p = false) noexcept
                : scheduler(schd)
                , preemptive(p) {}

            inline
            auto
            make_op() {
                return op<scheduler_t>{scheduler, preemptive};
            }

            template<typename context_t>
            auto
            connect() {
                return core::builder::op_list_builder<0>().push_back(make_op());
            }

            scheduler_t *scheduler;
            bool preemptive;
        };
    }

    namespace _timer {
        struct
        req {
            uint64_t timestamp;
            std::move_only_function<void()> cb;
        };

        template<typename scheduler_t>
        struct
        op {
            constexpr static bool scheduler_timer_op = true;
            scheduler_t *scheduler;
            uint64_t timestamp;

            template<uint32_t pos, typename context_t, typename scope_t>
            auto
            start(context_t& context, scope_t& scope) {
                scheduler->schedule(
                    new req {
                        timestamp,
                        [context = context, scope = scope]() mutable {
                            core::op_pusher<pos + 1, scope_t>::push_value(context, scope);
                        }
                    }
                );
            }
        };

        template<typename scheduler_t>
        struct
        sender {
            uint64_t timestamp;
        public:
            explicit
            sender(scheduler_t *s, uint64_t ts) noexcept
                : scheduler(s)
                , timestamp(ts){
            }

            inline
            auto
            make_op() {
                return op<scheduler_t>{scheduler, timestamp};
            }

            template<typename context_t>
            auto
            connect() {
                return core::builder::op_list_builder<0>().push_back(make_op());
            }

            scheduler_t *scheduler;
        };
    }

    struct
    preemptive_scheduler {
        core::mpmc::queue<_tasks::req*, 2048> tasks_request_q;

        auto
        schedule(_tasks::req* req) {
            return tasks_request_q.try_enqueue(req);
        }

        auto
        as_task() noexcept {
            return _tasks::sender<preemptive_scheduler>{this, false};
        }
    };

    struct
    scheduler {
        friend struct _tasks::op<scheduler>;
        friend struct _timer::op<scheduler>;
        using preemption_queues = std::tuple<preemptive_scheduler>;
    public:
        core::mpsc::queue<_tasks::req*, 2048> tasks_request_q;
        core::mpsc::queue<_timer::req*, 1024> timer_request_q;
        std::set<_timer::req*> timer_set;
        uint32_t core;

    private:
        auto
        schedule(_tasks::req* req){
            return tasks_request_q.try_enqueue(req);
        }

    public:
    private:
        auto
        schedule(_timer::req* req){
            return timer_request_q.try_enqueue(req);
        }

    public:
        explicit
        scheduler(uint32_t c)
            :core(c){
        }

        auto
        as_task() noexcept {
            return _tasks::sender<scheduler>{this, false};
        }

        auto
        as_preemptive_task() noexcept {
            return _tasks::sender<scheduler>{this, true};
        }

        bool
        handle_tasks() {
            bool worked = false;
            while (const auto req = tasks_request_q.try_dequeue()) {
                req.value()->cb();
                delete req.value();
                worked = true;
            }
            return worked;
        }


        template<typename Runtime>
        [[nodiscard]]
        bool
        handle_preemptive(Runtime &rt) const {
            return handle_preemptive_impl<0>(rt);
        }

        template<std::size_t Index, typename Runtime>
        [[nodiscard]]
        bool
        handle_preemptive_impl(Runtime &rt) const {
            if constexpr (Index < std::tuple_size_v<preemption_queues>) {
                using Q = std::tuple_element_t<Index, preemption_queues>;
                if constexpr (requires { rt.template get_preemption_queue<Q>(); }) {
                    if (auto *q = rt.template get_preemption_queue<Q>()) {
                        if (auto req = q->tasks_request_q.try_dequeue()) {
                            req.value()->cb();
                            delete req.value();
                            return true;
                        }
                    }
                }
                return handle_preemptive_impl<Index + 1>(rt);
            }
            return false;
        }

        bool
        handle_timer() {
            bool worked = false;
            while (auto req = timer_request_q.try_dequeue()) {
                timer_set.emplace(req.value());
                worked = true;
            }

            while (!timer_set.empty()) {
                if (now_ms() < (*timer_set.begin())->timestamp)
                    break;
                auto req = *timer_set.begin();
                timer_set.erase(timer_set.begin());
                req->cb();
                delete req;
                worked = true;
            }
            return worked;
        }

        static uint64_t
        now_ms() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        auto
        delay(const uint64_t ms) {
            return _timer::sender<scheduler>(this, now_ms() + ms);
        }

        bool
        advance() {
            bool worked = false;
            worked |= handle_timer();
            worked |= handle_tasks();
            return worked;
        }

        template<typename Runtime>
        bool
        advance(Runtime& rt) {
            if (!advance())
                return handle_preemptive(rt);
            return true;
        }
    };
}

namespace pump::core {
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
    && (get_current_op_type_t<pos, scope_t>::scheduler_task_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename context_t>
        static void
        push_value(context_t& context, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(context, scope);
        }
    };

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
    && (get_current_op_type_t<pos, scope_t>::scheduler_timer_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename context_t>
        static void
        push_value(context_t& context, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(context, scope);
        }
    };

    template <typename context_t, typename scheduler_t>
    struct
    compute_sender_type<context_t, scheduler::task::_tasks::sender<scheduler_t>> {
        consteval static uint32_t
        count_value() {
            return 0;
        }
    };

    template <typename context_t, typename scheduler_t>
    struct
    compute_sender_type<context_t, scheduler::task::_timer::sender<scheduler_t>> {
        consteval static uint32_t
        count_value() {
            return 0;
        }
    };
}

#endif //ENV_SCHEDULER_TASKS_SCHEDULER_HH