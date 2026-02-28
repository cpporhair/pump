#ifndef ENV_SCHEDULER_RPC_SENDERS_CALL_HH
#define ENV_SCHEDULER_RPC_SENDERS_CALL_HH

#include <cstdint>
#include <variant>

#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"
#include "pump/sender/then.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/submit.hh"
#include "pump/core/context.hh"

#include "env/scheduler/net/net.hh"
#include "env/scheduler/task/tasks_scheduler.hh"

#include "../common/header.hh"
#include "../common/error.hh"
#include "../common/codec_concept.hh"
#include "../channel/channel.hh"

namespace pump::rpc::_call {

    template<typename channel_ptr_t>
    struct op {
        constexpr static bool rpc_call_op = true;

        channel_ptr_t ch;
        uint16_t      method_id;
        encode_result encoded_payload;
        uint64_t      timeout_ms;

        op(channel_ptr_t c, uint16_t mid, encode_result&& ep, uint64_t tms)
            : ch(std::move(c))
            , method_id(mid)
            , encoded_payload(std::move(ep))
            , timeout_ms(tms) {
        }

        op(op&& rhs) noexcept
            : ch(std::move(rhs.ch))
            , method_id(rhs.method_id)
            , encoded_payload(std::move(rhs.encoded_payload))
            , timeout_ms(rhs.timeout_ms) {
        }

        template<uint32_t pos, typename ctx_t, typename scope_t>
        void
        start(ctx_t& ctx, scope_t& scope) {
            // 1. check channel status
            if (ch->status != channel_status::active) {
                core::op_pusher<pos + 1, scope_t>::push_exception(
                    ctx, scope,
                    std::make_exception_ptr(channel_closed_error()));
                return;
            }

            // 2. allocate request_id
            uint32_t rid = ch->alloc_request_id();

            // 3. compute deadline
            uint64_t deadline = timeout_ms > 0
                ? scheduler::task::scheduler::now_ms() + timeout_ms
                : 0;

            // 4. register in pending_map
            auto inserted = ch->pending.insert(rid, deadline,
                [ctx = ctx, scope = scope](
                    std::variant<payload_view, std::exception_ptr> result
                ) mutable {
                    if (result.index() == 0) [[likely]] {
                        core::op_pusher<pos + 1, scope_t>::push_value(
                            ctx, scope, std::get<0>(std::move(result)));
                    } else {
                        core::op_pusher<pos + 1, scope_t>::push_exception(
                            ctx, scope, std::get<1>(std::move(result)));
                    }
                });

            if (!inserted) {
                core::op_pusher<pos + 1, scope_t>::push_exception(
                    ctx, scope,
                    std::make_exception_ptr(pending_overflow_error()));
                return;
            }

            // 5. build send iovec: [rpc_header | payload]
            size_t vec_cnt = 1 + encoded_payload.cnt;
            auto* send_vec = new iovec[vec_cnt];
            auto* header_buf = new uint8_t[rpc_header::size];
            write_header(header_buf, {message_type::request, rid, method_id});
            send_vec[0] = {header_buf, rpc_header::size};
            for (size_t i = 0; i < encoded_payload.cnt; ++i) {
                send_vec[i + 1] = encoded_payload.vec[i];
            }

            // 6. send via net layer (fire-and-forget)
            scheduler::net::send(ch->session_sched, ch->session_id, send_vec, vec_cnt)
                >> pump::sender::then(
                    [header_buf, send_vec,
                     payload = std::move(encoded_payload)](bool) mutable {
                        delete[] header_buf;
                        delete[] send_vec;
                    })
                >> pump::sender::ignore_all_exception()
                >> pump::sender::submit(core::make_root_context());
        }
    };

    template<typename channel_ptr_t>
    struct sender {
        channel_ptr_t ch;
        uint16_t      method_id;
        encode_result encoded_payload;
        uint64_t      timeout_ms;

        sender(channel_ptr_t c, uint16_t mid, encode_result&& ep, uint64_t tms)
            : ch(std::move(c))
            , method_id(mid)
            , encoded_payload(std::move(ep))
            , timeout_ms(tms) {
        }

        sender(sender&& rhs) noexcept
            : ch(std::move(rhs.ch))
            , method_id(rhs.method_id)
            , encoded_payload(std::move(rhs.encoded_payload))
            , timeout_ms(rhs.timeout_ms) {
        }

        auto
        make_op() {
            return op<channel_ptr_t>{
                ch, method_id, std::move(encoded_payload), timeout_ms};
        }

        template<typename ctx_t>
        auto
        connect() {
            return core::builder::op_list_builder<0>().push_back(make_op());
        }
    };

}

// Public API
namespace pump::rpc {

    // Generic version: accepts pre-encoded payload
    template<typename channel_ptr_t>
    auto
    call(channel_ptr_t ch, uint16_t method_id,
         encode_result&& payload, uint64_t timeout_ms = 0)
    {
        return _call::sender<channel_ptr_t>{
            std::move(ch), method_id, std::move(payload), timeout_ms};
    }

    // Convenience version: auto-encode with channel's codec
    template<typename Request, typename channel_ptr_t>
    auto
    call(channel_ptr_t ch, uint16_t method_id,
         const Request& req, uint64_t timeout_ms = 0)
    {
        auto encoded = ch->codec().encode_payload(req);
        return _call::sender<channel_ptr_t>{
            std::move(ch), method_id, std::move(encoded), timeout_ms};
    }

}

// op_pusher specialization
namespace pump::core {

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::rpc_call_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename context_t>
        static void
        push_value(context_t& context, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(context, scope);
        }
    };

    // compute_sender_type specialization
    template<typename context_t, typename channel_ptr_t>
    struct
    compute_sender_type<context_t, rpc::_call::sender<channel_ptr_t>> {
        consteval static uint32_t
        count_value() {
            return 1;
        }

        consteval static auto
        get_value_type_identity() {
            return std::type_identity<rpc::payload_view>{};
        }
    };

}

#endif //ENV_SCHEDULER_RPC_SENDERS_CALL_HH
