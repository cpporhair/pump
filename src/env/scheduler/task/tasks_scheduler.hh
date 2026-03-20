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

            template<uint32_t pos, typename context_t, typename scope_t>
            auto
            start(context_t& context, scope_t& scope) {
                auto* r = new req {
                    [context = context, scope = scope]() mutable {
                        core::op_pusher<pos + 1, scope_t>::push_value(context, scope);
                    }
                };
                scheduler->schedule(r);
            }
        };

        template<typename scheduler_t>
        struct
        sender {
        public:
            explicit
            sender(scheduler_t *schd, bool p = false) noexcept
                : scheduler(schd) {
            }

            auto
            make_op() {
                return op<scheduler_t>{scheduler};
            }

            template<typename context_t>
            auto
            connect() {
                return core::builder::op_list_builder<0>().push_back(make_op());
            }

            scheduler_t *scheduler;
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
                return op<scheduler_t>(scheduler);
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
        core::mpmc::queue<_tasks::req*> tasks_request_q;
        core::mpmc::queue<_timer::req*> timer_request_q;

        explicit preemptive_scheduler(size_t queue_depth = 8192)
            : tasks_request_q(queue_depth), timer_request_q(queue_depth) {}

        auto
        schedule(_tasks::req* req) {
            return tasks_request_q.try_enqueue(req);
        }

        auto
        schedule(_timer::req* req) {
            return timer_request_q.try_enqueue(req);
        }

        auto
        as_task() {
            return _tasks::sender{this};
        }

        static uint64_t
        now_ms() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        auto
        delay(const uint64_t ms) noexcept {
            return _timer::sender{this, now_ms() + ms};
        }
    };


    struct
    scheduler {
        friend struct _tasks::op<scheduler>;
        friend struct _timer::op<scheduler>;
        using preemptive_scheduler_t = preemptive_scheduler;
    public:
        core::per_core::queue<_tasks::req*> tasks_request_q;
        core::per_core::queue<_timer::req*> timer_request_q;
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
        scheduler(uint32_t c, size_t queue_depth = 2048)
            : tasks_request_q(queue_depth, c)
            , timer_request_q(queue_depth, c)
            , core(c) {
        }

        auto
        as_task() noexcept {
            return _tasks::sender<scheduler>{this};
        }

        bool
        handle_tasks() {
            return tasks_request_q.drain([](_tasks::req* req) {
                req->cb();
                delete req;
            });
        }

        auto
        handle_preemptive_req(_tasks::req* req) {
            req->cb();
            delete req;
            return true;
        }

        auto
        handle_preemptive_req(_timer::req* req) {
            timer_set.emplace(req);
            return true;
        }

        template<typename Runtime>
        [[nodiscard]]
        bool
        handle_preemptive(Runtime &rt) {
            if constexpr (requires { rt.template get_preemption_queue<preemptive_scheduler_t>(); }) {
                if (auto *q = rt.template get_preemption_queue<preemptive_scheduler_t>()) {
                    if (auto req = q->tasks_request_q.try_dequeue(); req) {
                        return handle_preemptive_req(req.value());
                    }
                    if (auto req = q->timer_request_q.try_dequeue(); req) {
                        return handle_preemptive_req(req.value());
                    }
                }
            }
            return false;
        }

        bool
        handle_timer() {
            bool worked = timer_request_q.drain([this](_timer::req* req) {
                timer_set.emplace(req);
            });

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