
#ifndef ENV_SCHEDULER_KCP_COMMON_LAYERS_HH
#define ENV_SCHEDULER_KCP_COMMON_LAYERS_HH

#include "env/scheduler/net/common/session_tags.hh"
#include "env/scheduler/net/common/frame_receiver.hh"
#include "./struct.hh"

namespace pump::scheduler::kcp::common {

    // KCP-specific tags
    struct get_conv{};  constexpr inline get_conv get_conv{};

    // kcp_bind<scheduler_t>: holds conv_id, scheduler pointer, and connection status
    template<typename scheduler_t>
    struct
    kcp_bind {
        conv_id_t conv;
        scheduler_t* scheduler;

        explicit kcp_bind(conv_id_t c, scheduler_t* s) : conv(c), scheduler(s) {}

        template<typename owner_t>
        conv_id_t
        invoke(const struct get_conv&, owner_t&) {
            return conv;
        }

        template<typename owner_t>
        void
        invoke(const struct ::pump::scheduler::net::do_close&, owner_t&) {
        }

        template<typename owner_t>
        void
        invoke(const struct ::pump::scheduler::net::on_error&, owner_t&, std::exception_ptr) {
        }

        template<typename owner_t>
        void
        invoke(const struct ::pump::scheduler::net::do_send&, owner_t&, ::pump::scheduler::net::frame_send_req* req) {
            auto* send = new send_req{
                conv,
                static_cast<char*>(req->data),
                req->len,
                std::move(req->cb)
            };
            scheduler->schedule_send(send);
            delete req;
        }
    };

}  // namespace pump::scheduler::kcp::common

#endif //ENV_SCHEDULER_KCP_COMMON_LAYERS_HH
