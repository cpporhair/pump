
#ifndef ENV_COMMON_FRAME_RECEIVER_HH
#define ENV_COMMON_FRAME_RECEIVER_HH

#include <bits/move_only_function.h>

#include "pump/core/lock_free_queue.hh"
#include "env/scheduler/net/common/frame.hh"
#include "env/scheduler/net/common/errors.hh"
#include "env/scheduler/net/common/session_tags.hh"

namespace pump::scheduler::net {

    struct frame_recv_req {
        std::move_only_function<void(net_frame&&)> cb;
        std::move_only_function<void(std::exception_ptr)> cb_err;
    };

    struct frame_send_req {
        void* data;
        uint32_t len;
        std::move_only_function<void(bool)> cb;
    };

    struct
    frame_receiver {
        core::local::queue<frame_recv_req*> recv_q;
        core::local::queue<net_frame*> ready_q;
        bool closed = false;

        template<typename owner_t>
        void
        invoke(const struct on_frame&, owner_t&, net_frame&& frame) {
            if (auto opt = recv_q.try_dequeue()) {
                opt.value()->cb(std::move(frame));
                delete opt.value();
            } else {
                ready_q.try_enqueue(new net_frame(std::move(frame)));
            }
        }

        template<typename owner_t>
        void
        invoke(const struct do_recv&, owner_t&, frame_recv_req* req) {
            if (closed) {
                req->cb_err(std::make_exception_ptr(session_closed_error()));
                delete req;
                return;
            }
            if (auto opt = ready_q.try_dequeue()) {
                req->cb(std::move(*opt.value()));
                delete opt.value();
                delete req;
            } else {
                if (!recv_q.try_enqueue(req)) {
                    req->cb_err(std::make_exception_ptr(enqueue_failed_error()));
                    delete req;
                }
            }
        }

        template<typename owner_t>
        void
        invoke(const struct on_error&, owner_t&, std::exception_ptr ex) {
            closed = true;
            while (auto opt = recv_q.try_dequeue()) {
                opt.value()->cb_err(ex);
                delete opt.value();
            }
            while (auto opt = ready_q.try_dequeue()) {
                delete opt.value();
            }
        }
    };

}

#endif //ENV_COMMON_FRAME_RECEIVER_HH
