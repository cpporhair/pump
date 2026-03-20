//
// Created by null on 25-4-15.
//

#ifndef ENV_SCHEDULER_NVME_SCHEDULER_HH
#define ENV_SCHEDULER_NVME_SCHEDULER_HH

#include <algorithm>
#include <expected>
#include <iostream>
#include <cstring>
#include "spdk/nvme.h"
#include "spdk/log.h"

#include "./get_page.hh"
#include "./put_page.hh"
#include "./ssd.hh"
#include "pump/core/ring_queue.hh"
#include "pump/core/lock_free_queue.hh"

namespace pump::scheduler::nvme {
    // Zero-allocation open-addressing hash table for in-flight read merge.
    // Fixed size, linear probing, designed for short-lived entries (IO duration).
    template<typename req_t, size_t TABLE_SIZE = 1024, size_t MAX_WAITERS = 16>
    struct merge_table {
        static_assert((TABLE_SIZE & (TABLE_SIZE - 1)) == 0, "TABLE_SIZE must be power of 2");

        enum slot_state : uint8_t { EMPTY = 0, OCCUPIED = 1, DELETED = 2 };
        struct slot {
            uint64_t pos;
            slot_state state = EMPTY;
            req_t* waiters[MAX_WAITERS];
            uint32_t n_waiters;
        };

        slot slots_[TABLE_SIZE]{};
        uint32_t occupied_count = 0;
        uint64_t merge_count = 0;
        uint64_t total_get_count = 0;

        // Fibonacci hash → top bits
        static uint32_t
        index(uint64_t pos) {
            return static_cast<uint32_t>((pos * 0x9E3779B97F4A7C15ULL) >> (64 - __builtin_ctzll(TABLE_SIZE)));
        }

        // Find slot for pos, nullptr if not found
        slot*
        find(uint64_t pos) {
            uint32_t idx = index(pos);
            for (uint32_t i = 0; i < 8; ++i) {
                auto& s = slots_[(idx + i) & (TABLE_SIZE - 1)];
                if (s.state == EMPTY) return nullptr;
                if (s.state == OCCUPIED && s.pos == pos) return &s;
                // DELETED: continue probing
            }
            return nullptr;
        }

        // Insert new entry, returns slot or nullptr if probe limit exceeded
        slot*
        insert(uint64_t pos) {
            uint32_t idx = index(pos);
            for (uint32_t i = 0; i < 8; ++i) {
                auto& s = slots_[(idx + i) & (TABLE_SIZE - 1)];
                if (s.state != OCCUPIED) {
                    s.pos = pos;
                    s.state = OCCUPIED;
                    s.n_waiters = 0;
                    ++occupied_count;
                    return &s;
                }
            }
            return nullptr;
        }

        // Add waiter to existing slot, returns false if full
        bool
        add_waiter(slot* s, req_t* r) {
            if (s->n_waiters >= MAX_WAITERS) return false;
            s->waiters[s->n_waiters++] = r;
            merge_count++;
            return true;
        }

        void
        erase(slot* s) {
            s->state = DELETED;
            --occupied_count;
        }

        [[nodiscard]] bool
        empty() const { return occupied_count == 0; }
    };
}

// Forward declare scheduler for merge callback
namespace pump::scheduler::nvme { template <page_concept page_t> struct scheduler; }

namespace pump::scheduler::nvme::_scheduler {
    inline auto
    disconnect_cb(spdk_nvme_qpair *qpair, void *ctx){
    }

    inline auto
    spdk_poll(spdk_nvme_poll_group *group) {
        const auto res = spdk_nvme_poll_group_process_completions(group, 0, disconnect_cb);
        if (res < 0) [[unlikely]]
            SPDK_ERRLOG("error,%ld",res);
    }

    inline auto
    spdk_poll(spdk_nvme_qpair* qp) {
        const auto res = spdk_nvme_qpair_process_completions(qp,0);
        if (res < 0) [[unlikely]]
            SPDK_ERRLOG("error,%d", res);
    }

    template <page_concept page_t>
    struct
    spdk_get_data_callback_arg {
        get::req<page_t> *raw_req;
        qpair<page_t> *qp;
        scheduler<page_t> *sched; // for page merge
    };

    template <page_concept page_t>
    inline auto
    on_get_data_done(void *cb_arg, const spdk_nvme_cpl *cpl) {
        auto *arg = static_cast<spdk_get_data_callback_arg<page_t> *>(cb_arg);
        auto *req = arg->raw_req;
        auto *qpr = arg->qp;
        auto *sched = arg->sched;

        delete arg;

        qpr->used--;

        uint08_t status = static_cast<uint08_t>((!cpl || spdk_nvme_cpl_is_error(cpl)) ? 1 : 0);

        // Page merge: notify waiters that were merged onto this read
        if (sched) {
            auto* slot = sched->merge_.find(req->page->get_pos());
            if (slot) {
                for (uint32_t i = 0; i < slot->n_waiters; ++i) {
                    auto* waiter = slot->waiters[i];
                    if (status == 0)
                        std::memcpy(waiter->page->get_payload(),
                                    req->page->get_payload(),
                                    req->page->get_size());
                    waiter->cb(get::res<page_t>{waiter->page, status});
                    delete waiter;
                }
                sched->merge_.erase(slot);
            }
        }

        req->cb(get::res<page_t>{req->page, status});
        delete req;
    }

    template <page_concept page_t>
    inline auto
    spdk_get(get::req<page_t>* req, qpair<page_t>* qp, scheduler<page_t>* sched = nullptr) {
        qp->used++;
        const auto res = spdk_nvme_ns_cmd_read(
            qp->owner->ns,
            qp->impl,
            req->page->get_payload(),
            req->page->get_pos() * req->page->get_size() / qp->owner->sector_size,
            req->page->get_size() / qp->owner->sector_size,
            on_get_data_done<page_t>,
            new spdk_get_data_callback_arg<page_t>{req, qp, sched},
            req->io_flags // SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS
        );

        if(res < 0) [[unlikely]] {
            SPDK_ERRLOG("error,%d", res);
            on_get_data_done<page_t>(new spdk_get_data_callback_arg<page_t>{req, qp, sched}, nullptr);
        }
    }

    template <page_concept page_t>
    struct
    spdk_put_data_callback_arg {
        put::req<page_t>* raw_req;
        qpair<page_t>* qp;
    };

    template <page_concept page_t>
    inline auto
    on_put_data_done(void *cb_arg, const spdk_nvme_cpl *cpl) {
        auto *arg = static_cast<spdk_put_data_callback_arg<page_t> *>(cb_arg);
        auto *req = arg->raw_req;
        auto *qpr = arg->qp;

        delete arg;

        qpr->used--;

        req->cb(put::res<page_t>{req->page, static_cast<uint08_t>((!cpl || spdk_nvme_cpl_is_error(cpl)) ? 1 : 0)});
        delete req;
    }

    template <page_concept page_t>
    inline auto
    spdk_put(put::req<page_t>* req, qpair<page_t>* qp) {
        qp->used++;
        const auto res = spdk_nvme_ns_cmd_write(
            qp->owner->ns,
            qp->impl,
            req->page->get_payload(),
            req->page->get_pos() * req->page->get_size() / qp->owner->sector_size,
            req->page->get_size() / qp->owner->sector_size,
            on_put_data_done<page_t>,
            new spdk_put_data_callback_arg<page_t>{req, qp},
            req->io_flags
        );

        if (res < 0) [[unlikely]] {
            SPDK_ERRLOG("error,%d", res);
            on_put_data_done<page_t>(new spdk_put_data_callback_arg<page_t>{req, qp}, nullptr);
        }
    }

    template <page_concept page_t>
    struct
    ssd_handler {
        ssd<page_t> *dev;
        qpair<page_t> *get_qp;
        qpair<page_t> *put_qp;
    };
}

namespace pump::scheduler::nvme {

    template <page_concept page_t>
    struct
    scheduler {
        friend struct get::op<scheduler<page_t>, page_t>;
        friend struct put::op<scheduler<page_t>, page_t>;
    private:
        qpair<page_t>* qp;
        core::per_core::queue<put::req<page_t>*> put_data_page_req_queue;
        core::per_core::queue<get::req<page_t>*> get_data_page_req_queue;
        core::ring_queue<put::req<page_t>*> local_put_q;
        core::ring_queue<get::req<page_t>*> local_get_q;

    public:
        // Page merge: zero-allocation in-flight read dedup
        merge_table<get::req<page_t>> merge_;

    private:
        auto
        schedule(put::req<page_t>* r) {
            return put_data_page_req_queue.try_enqueue(r);
        }

        auto
        schedule(get::req<page_t>* r) {
            return get_data_page_req_queue.try_enqueue(r);
        }

        void
        handle_one_request(put::req<page_t>* r) const {
            _scheduler::spdk_put(r, qp);
        }

        void
        handle_one_request(get::req<page_t>* r) const {
            _scheduler::spdk_get(r, qp);
        }

        void
        handle_put_queue() {
            while (!local_put_q.empty()) [[unlikely]] {
                if (!qp->busy()) {
                    put::req<page_t>* r;
                    local_put_q.dequeue(r);
                    _scheduler::spdk_put(r, qp);
                } else {
                    break;
                }
            }
        }

        void
        handle_get_queue() {
            while (!local_get_q.empty()) [[unlikely]] {
                if (!qp->busy()) {
                    get::req<page_t>* r;
                    local_get_q.dequeue(r);

                    uint64_t pos = r->page->get_pos();
                    merge_.total_get_count++;

                    // Fast path: no in-flight reads → skip merge lookup
                    if (!merge_.empty()) {
                        auto* slot = merge_.find(pos);
                        if (slot) {
                            if (merge_.add_waiter(slot, r))
                                continue; // merged, no IO needed
                            // Waiter array full, fall through to submit separate read
                        }
                    }

                    // Register in merge table for potential future merges
                    merge_.insert(pos);
                    // Submit to SPDK with merge-aware callback
                    _scheduler::spdk_get(r, qp, this);
                } else {
                    break;
                }
            }
        }

        void
        poll() const {
            _scheduler::spdk_poll(qp->impl);
        }

    public:

        const ssd<page_t>*
        get_ssd() const {
            return qp->owner;
        }

        explicit
        scheduler(qpair<page_t>* qpair, size_t queue_depth = 2048, size_t local_depth = 128)
            : qp(qpair)
            , put_data_page_req_queue(queue_depth)
            , get_data_page_req_queue(queue_depth)
            , local_put_q(local_depth)
            , local_get_q(local_depth) {
        }

        auto
        put(page_t* p) {
            return put::sender<scheduler<page_t>, page_t>(this, p);
        }

        auto
        get(page_t* p) {
            return get::sender<scheduler<page_t>, page_t>(this, p);
        }

        bool
        advance() {
            while (local_put_q.size() < local_put_q.capacity()) {
                auto item = put_data_page_req_queue.try_dequeue();
                if (!item) break;
                local_put_q.enqueue(std::move(*item));
            }

            while (local_get_q.size() < local_get_q.capacity()) {
                auto item = get_data_page_req_queue.try_dequeue();
                if (!item) break;
                local_get_q.enqueue(std::move(*item));
            }

            handle_put_queue();
            handle_get_queue();

            if (qp->empty())
                return false;
            poll();
            return true;
        }

        template <typename runtime_t>
        bool
        advance(runtime_t& runtime) {
            return advance();
        }

    };
}

#endif //ENV_SCHEDULER_NVME_SCHEDULER_HH
