//
// Created by null on 25-4-15.
//

#ifndef ENV_SCHEDULER_NVME_SCHEDULER_HH
#define ENV_SCHEDULER_NVME_SCHEDULER_HH

#include <algorithm>
#include <expected>
#include <iostream>
#include "spdk/nvme.h"
#include "spdk/log.h"

#include "./get_page.hh"
#include "./put_page.hh"
#include "./ssd.hh"
#include "pump/core/ring_queue.hh"

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
    };

    template <page_concept page_t>
    inline auto
    on_get_data_done(void *cb_arg, const spdk_nvme_cpl *cpl) {
        auto *arg = static_cast<spdk_get_data_callback_arg<page_t> *>(cb_arg);
        auto *req = arg->raw_req;
        auto *qpr = arg->qp;

        delete arg;

        qpr->used--;

        req->cb(get::res<page_t>{req->page, cpl && !spdk_nvme_cpl_is_error(cpl)});
        delete req;
    }

    template <page_concept page_t>
    inline auto
    spdk_get(get::req<page_t>* req, qpair<page_t>* qp) {
        const auto res = spdk_nvme_ns_cmd_read(
            qp->owner->ns,
            qp->impl,
            req->page->get_payload(),
            req->page->get_pos() * req->page->get_size() / qp->owner->sector_size,
            req->page->get_size() / qp->owner->sector_size,
            on_get_data_done<page_t>,
            new spdk_get_data_callback_arg<page_t>{req, qp},
            req->io_flags // SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS
        );

        if(res < 0) [[unlikely]] {
            SPDK_ERRLOG("error,%d", res);
            on_get_data_done<page_t>(new spdk_get_data_callback_arg<page_t>{req, qp}, nullptr);
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

        req->cb(put::res<page_t>{req->page, cpl && !spdk_nvme_cpl_is_error(cpl)});
        delete req;
    }

    template <page_concept page_t>
    inline auto
    spdk_put(put::req<page_t>* req, qpair<page_t>* qp) {
        const auto res = spdk_nvme_ns_cmd_write(
            qp->owner->ns,
            qp->impl,
            req->page->get_payload(),
            req->page->get_pos() * req->page->get_size() / qp->owner->sector_size,
            req->page->get_size() / qp->owner->sector_size,
            on_put_data_done<page_t>,
            new spdk_put_data_callback_arg<page_t>{req, qp},
            SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS
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
        spdk_ring* put_data_page_req_queue;
        spdk_ring* get_data_page_req_queue;
        core::ring_queue<put::req<page_t>*> local_put_q;
        core::ring_queue<get::req<page_t>*> local_get_q;
    private:
        auto
        schedule(put::req<page_t>* r) const {
            return 0 < spdk_ring_enqueue(put_data_page_req_queue, reinterpret_cast<void **>(&r), 1, nullptr);
        }

        auto
        schedule(get::req<page_t>* r) const {
            return 0 < spdk_ring_enqueue(get_data_page_req_queue, reinterpret_cast<void **>(&r), 1, nullptr);
        }

        void
        handle_one_request(put::req<page_t>* r) const {
            _scheduler::spdk_put(r, qp);
        }

        void
        handle_one_request(get::req<page_t>* r) const {
            _scheduler::spdk_get(r, qp);
        }

        template <typename request_t>
        void
        handle_local_queue(core::ring_queue<request_t*>& q) {
            while (!q.empty()) [[unlikely]] {
                if (!qp->busy()) {
                    request_t* r;
                    assert(q.dequeue(r));
                    handle_one_request(r);
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

        scheduler(spdk_ring* put_r,  spdk_ring* get_r, qpair<page_t>* qpair)
            : qp(qpair)
            , put_data_page_req_queue(put_r)
            , get_data_page_req_queue(get_r)
            , local_put_q(128)
            , local_get_q(128) {
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
            if (!qp->busy()) {
                poll();
                return true;
            }
            handle_local_queue(local_put_q);
            handle_local_queue(local_get_q);

            auto count = spdk_ring_dequeue(
                put_data_page_req_queue,
                static_cast<void **>(local_put_q.tail_buffer()),
                local_put_q.size()
            );

            if (0 < count) {
                local_put_q.tail_offset(count);
            }

            count = spdk_ring_dequeue(
                get_data_page_req_queue,
                static_cast<void **>(local_get_q.tail_buffer()),
                local_get_q.size()
            );

            if (0 < count) {
                local_get_q.tail_offset(count);
            }

            handle_local_queue(local_put_q);
            handle_local_queue(local_get_q);

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
