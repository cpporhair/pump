#ifndef ENV_SCHEDULER_RPC_SENDERS_REPLY_HH
#define ENV_SCHEDULER_RPC_SENDERS_REPLY_HH

#include <cstdint>
#include <cstring>
#include <vector>

#include "env/scheduler/net/common/struct.hh"
#include "env/scheduler/net/common/detail.hh"
#include "env/scheduler/task/tasks_scheduler.hh"

#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/submit.hh"
#include "pump/core/context.hh"

#include "../common/header.hh"
#include "../common/error.hh"
#include "../common/codec_concept.hh"
#include "../channel/channel.hh"
#include "../channel/dispatcher.hh"

namespace pump::rpc::detail {

    using namespace pump::scheduler::net::common;

    // Read bytes from pkt_iovec at given offset, linearize if needed
    inline const uint8_t*
    read_bytes_from_pkt(const pkt_iovec& pkt, size_t offset,
                        size_t len, uint8_t* tmp)
    {
        if (pkt.cnt == 1) {
            return static_cast<const uint8_t*>(pkt.vec[0].iov_base) + offset;
        }
        // 2 segments
        size_t first_len = pkt.vec[0].iov_len;
        if (offset >= first_len) {
            // entirely in second segment
            return static_cast<const uint8_t*>(pkt.vec[1].iov_base)
                   + (offset - first_len);
        }
        size_t avail = first_len - offset;
        if (len <= avail) {
            // entirely in first segment
            return static_cast<const uint8_t*>(pkt.vec[0].iov_base) + offset;
        }
        // spans boundary — linearize into tmp
        std::memcpy(tmp,
                     static_cast<const uint8_t*>(pkt.vec[0].iov_base) + offset,
                     avail);
        std::memcpy(tmp + avail,
                     pkt.vec[1].iov_base,
                     len - avail);
        return tmp;
    }

    // Make a payload_view for a sub-range of pkt_iovec
    // out_iov must point to at least 2 iovec elements
    inline payload_view
    make_sub_view(const pkt_iovec& pkt, size_t offset, size_t len,
                  iovec* out_iov)
    {
        if (len == 0) {
            return {nullptr, 0, 0};
        }
        if (pkt.cnt == 1) {
            out_iov[0] = {
                static_cast<uint8_t*>(pkt.vec[0].iov_base) + offset,
                len
            };
            return {out_iov, 1, len};
        }
        size_t first_len = pkt.vec[0].iov_len;
        if (offset >= first_len) {
            out_iov[0] = {
                static_cast<uint8_t*>(pkt.vec[1].iov_base) + (offset - first_len),
                len
            };
            return {out_iov, 1, len};
        }
        size_t avail = first_len - offset;
        if (len <= avail) {
            out_iov[0] = {
                static_cast<uint8_t*>(pkt.vec[0].iov_base) + offset,
                len
            };
            return {out_iov, 1, len};
        }
        // spans both segments
        out_iov[0] = {
            static_cast<uint8_t*>(pkt.vec[0].iov_base) + offset,
            avail
        };
        out_iov[1] = {
            pkt.vec[1].iov_base,
            len - avail
        };
        return {out_iov, 2, len};
    }

    // Copy data from pkt_iovec at offset to a vector
    inline std::vector<uint8_t>
    copy_from_pkt(const pkt_iovec& pkt, size_t offset, size_t len) {
        std::vector<uint8_t> buf(len);
        if (len == 0) return buf;

        size_t dst_offset = 0;
        size_t src_offset = 0;

        for (uint8_t i = 0; i < pkt.cnt && dst_offset < len; ++i) {
            size_t seg_len = pkt.vec[i].iov_len;
            auto* seg_data = static_cast<const uint8_t*>(pkt.vec[i].iov_base);

            if (src_offset + seg_len <= offset) {
                src_offset += seg_len;
                continue;
            }

            size_t seg_start = (offset > src_offset) ? (offset - src_offset) : 0;
            size_t copy_len = std::min(seg_len - seg_start, len - dst_offset);
            std::memcpy(buf.data() + dst_offset, seg_data + seg_start, copy_len);
            dst_offset += copy_len;
            src_offset += seg_len;
        }

        return buf;
    }

    // Handle Response/Error: lookup pending_map, execute callback
    template<typename channel_ptr_t>
    inline void
    handle_response(channel_ptr_t& ch, const rpc_header& header,
                    const pkt_iovec& pkt, size_t payload_offset,
                    size_t payload_len)
    {
        auto cb = ch->pending.remove(header.request_id);
        if (!cb) return; // already timed out or duplicate

        if (header.type == message_type::error) {
            uint8_t err_tmp[258]; // 2B code + 256B message max
            size_t read_len = std::min(payload_len, sizeof(err_tmp));
            const uint8_t* err_data = read_bytes_from_pkt(
                pkt, payload_offset, read_len, err_tmp);
            auto ep = parse_error_payload(err_data, read_len);
            (*cb)(std::make_exception_ptr(
                remote_rpc_error(ep.code, ep.message)));
        } else {
            // Construct payload_view pointing into ring buffer data
            // Valid until forward_head is called (which happens after this returns)
            iovec sub_iov[2];
            auto pv = make_sub_view(pkt, payload_offset, payload_len, sub_iov);
            (*cb)(pv);
        }
    }

    // Handle Request: copy payload, submit dispatch pipeline fire-and-forget
    template<auto ...service_ids, typename channel_ptr_t>
    inline void
    handle_request(channel_ptr_t& ch, const rpc_header& header,
                   const pkt_iovec& pkt, size_t payload_offset,
                   size_t payload_len)
    {
        // Copy payload for async handler safety
        auto payload_data = copy_from_pkt(pkt, payload_offset, payload_len);

        auto rid = header.request_id;
        auto mid = header.method_id;

        // Create handler context with owned data
        handler_context ctx{rid, mid, std::move(payload_data)};

        // Submit dispatch pipeline fire-and-forget
        pump::sender::just(std::move(ctx))
            >> dispatch<service_ids...>()
            >> pump::sender::then(
                [ch, rid, mid](auto&& result) {
                    auto encoded = ch->codec().encode_payload(result);
                    ch->send_response(rid, mid, std::move(encoded));
                })
            >> pump::sender::any_exception(
                [ch, rid](std::exception_ptr) {
                    ch->send_error(rid, error_code::remote_error,
                                   "handler error");
                    return pump::sender::just();
                })
            >> pump::sender::submit(core::make_root_context());
    }

    // Handle Notification: copy payload, submit dispatch pipeline (no response)
    template<auto ...service_ids, typename channel_ptr_t>
    inline void
    handle_notification(channel_ptr_t& ch, const rpc_header& header,
                        const pkt_iovec& pkt, size_t payload_offset,
                        size_t payload_len)
    {
        auto payload_data = copy_from_pkt(pkt, payload_offset, payload_len);
        handler_context ctx{0, header.method_id, std::move(payload_data)};

        pump::sender::just(std::move(ctx))
            >> dispatch<service_ids...>()
            >> pump::sender::ignore_results()
            >> pump::sender::any_exception(
                [](std::exception_ptr) {
                    return pump::sender::just();
                })
            >> pump::sender::submit(core::make_root_context());
    }

    // Process all complete messages from a packet_buffer
    template<auto ...service_ids, typename channel_ptr_t>
    inline void
    process_buffer(channel_ptr_t& ch, packet_buffer* buf) {
        namespace nd = pump::scheduler::net::common::detail;

        while (nd::has_full_pkt(buf)) {
            // Read length prefix
            auto total_len = buf->handle_data(
                sizeof(uint16_t), nd::read_pkt_len);
            auto net_payload_len =
                static_cast<size_t>(total_len - sizeof(uint16_t));

            // Skip length prefix
            buf->forward_head(sizeof(uint16_t));

            if (net_payload_len < rpc_header::size) {
                // Malformed: too small for RPC header
                buf->forward_head(net_payload_len);
                continue;
            }

            // Get pkt_iovec for the net payload
            auto pkt = buf->handle_data(
                net_payload_len, nd::recv_pkt_getter);

            if (pkt.cnt == 0) {
                buf->forward_head(net_payload_len);
                continue;
            }

            // Parse RPC header from first 7 bytes
            uint8_t hdr_tmp[rpc_header::size];
            const uint8_t* hdr_data =
                read_bytes_from_pkt(pkt, 0, rpc_header::size, hdr_tmp);
            auto header = parse_header(hdr_data);

            size_t rpc_payload_len = net_payload_len - rpc_header::size;

            // Dispatch by message type
            switch (header.type) {
                case message_type::response:
                case message_type::error:
                    handle_response(
                        ch, header, pkt,
                        rpc_header::size, rpc_payload_len);
                    break;

                case message_type::request:
                    if constexpr (sizeof...(service_ids) > 0) {
                        handle_request<service_ids...>(
                            ch, header, pkt,
                            rpc_header::size, rpc_payload_len);
                    } else {
                        ch->send_error(header.request_id,
                            error_code::method_not_found, "no services");
                    }
                    break;

                case message_type::notification:
                    if constexpr (sizeof...(service_ids) > 0) {
                        handle_notification<service_ids...>(
                            ch, header, pkt,
                            rpc_header::size, rpc_payload_len);
                    }
                    break;
            }

            // Cleanup pkt_iovec (allocated by recv_pkt_getter)
            delete[] pkt.vec;

            // Advance past the message
            buf->forward_head(net_payload_len);
        }

        // Check for timed-out pending requests
        ch->pending.check_timeouts(
            scheduler::task::scheduler::now_ms());
    }

}

#endif //ENV_SCHEDULER_RPC_SENDERS_REPLY_HH
