
#ifndef ENV_SCHEDULER_KCP_COMMON_IKCP_HH
#define ENV_SCHEDULER_KCP_COMMON_IKCP_HH

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <deque>
#include <list>
#include <vector>
#include <functional>

namespace pump::scheduler::kcp::common {

    // KCP segment header layout (24 bytes)
    //  0     4     5     6     8     12    16    20    24
    // +-----+-----+-----+-----+-----+-----+-----+-----+
    // |conv |cmd  |frg  |wnd  | ts  | sn  | una |len  |
    // +-----+-----+-----+-----+-----+-----+-----+-----+
    static constexpr uint32_t IKCP_OVERHEAD = 24;
    static constexpr uint32_t IKCP_MTU_DEF = 1400;
    static constexpr uint32_t IKCP_MSS_DEF = IKCP_MTU_DEF - IKCP_OVERHEAD;
    static constexpr uint32_t IKCP_RTO_DEF = 200;
    static constexpr uint32_t IKCP_RTO_MIN = 100;
    static constexpr uint32_t IKCP_RTO_MAX = 60000;
    static constexpr uint32_t IKCP_WND_SND = 32;
    static constexpr uint32_t IKCP_WND_RCV = 128;
    static constexpr uint32_t IKCP_ASK_SEND = 1;
    static constexpr uint32_t IKCP_ASK_TELL = 2;
    static constexpr uint32_t IKCP_THRESH_INIT = 2;
    static constexpr uint32_t IKCP_PROBE_INIT = 7000;
    static constexpr uint32_t IKCP_PROBE_LIMIT = 120000;
    static constexpr uint32_t IKCP_FASTACK_LIMIT = 5;
    static constexpr uint32_t IKCP_DEADLINK = 20;

    enum ikcp_cmd : uint8_t {
        IKCP_CMD_PUSH = 81,
        IKCP_CMD_ACK  = 82,
        IKCP_CMD_WASK = 83,
        IKCP_CMD_WINS = 84,
    };

    struct ikcp_seg {
        uint32_t conv = 0;
        uint8_t  cmd  = 0;
        uint8_t  frg  = 0;
        uint16_t wnd  = 0;
        uint32_t ts   = 0;
        uint32_t sn   = 0;
        uint32_t una  = 0;
        uint32_t len  = 0;
        uint32_t resendts = 0;
        uint32_t rto   = 0;
        uint32_t fastack = 0;
        uint32_t xmit = 0;
        std::vector<char> data;
    };

    static inline void encode32u(char* p, uint32_t v) { memcpy(p, &v, 4); }
    static inline void encode16u(char* p, uint16_t v) { memcpy(p, &v, 2); }
    static inline void encode8u(char* p, uint8_t v) { *p = static_cast<char>(v); }
    static inline uint32_t decode32u(const char* p) { uint32_t v; memcpy(&v, p, 4); return v; }
    static inline uint16_t decode16u(const char* p) { uint16_t v; memcpy(&v, p, 2); return v; }
    static inline uint8_t decode8u(const char* p) { return static_cast<uint8_t>(*p); }

    static inline int32_t itimediff(uint32_t later, uint32_t earlier) {
        return static_cast<int32_t>(later - earlier);
    }

    static inline char* ikcp_encode_seg(char* ptr, const ikcp_seg& seg) {
        encode32u(ptr, seg.conv); ptr += 4;
        encode8u(ptr, seg.cmd);   ptr += 1;
        encode8u(ptr, seg.frg);   ptr += 1;
        encode16u(ptr, seg.wnd);  ptr += 2;
        encode32u(ptr, seg.ts);   ptr += 4;
        encode32u(ptr, seg.sn);   ptr += 4;
        encode32u(ptr, seg.una);  ptr += 4;
        encode32u(ptr, seg.len);  ptr += 4;
        return ptr;
    }

    // KCP control block — one per connection (conv)
    struct ikcp {
        uint32_t conv = 0;
        uint32_t mtu = IKCP_MTU_DEF;
        uint32_t mss = IKCP_MSS_DEF;
        uint32_t state = 0;  // 0=ok, -1=dead

        uint32_t snd_una = 0;  // first unacknowledged SN
        uint32_t snd_nxt = 0;  // next SN to send
        uint32_t rcv_nxt = 0;  // next expected SN to receive

        uint32_t ssthresh = IKCP_THRESH_INIT;
        uint32_t rx_rttval = 0;
        uint32_t rx_srtt = 0;
        uint32_t rx_rto = IKCP_RTO_DEF;
        uint32_t rx_minrto = IKCP_RTO_MIN;

        uint32_t snd_wnd = IKCP_WND_SND;
        uint32_t rcv_wnd = IKCP_WND_RCV;
        uint32_t rmt_wnd = IKCP_WND_RCV;
        uint32_t cwnd = 0;
        uint32_t probe = 0;

        uint32_t current = 0;
        uint32_t interval = 100;
        uint32_t ts_flush = 0;
        uint32_t xmit = 0;
        uint32_t nodelay = 0;
        uint32_t updated = 0;
        uint32_t ts_probe = 0;
        uint32_t probe_wait = 0;
        uint32_t dead_link = IKCP_DEADLINK;
        uint32_t incr = 0;

        std::deque<ikcp_seg> snd_queue;
        std::list<ikcp_seg>  snd_buf;
        std::deque<ikcp_seg> rcv_queue;
        std::list<ikcp_seg>  rcv_buf;
        std::vector<std::pair<uint32_t, uint32_t>> acklist;  // (sn, ts) pairs

        std::vector<char> buffer;  // output buffer

        uint32_t fastresend = 0;
        uint32_t fastlimit = IKCP_FASTACK_LIMIT;
        uint32_t nocwnd = 0;

        using output_fn = std::function<void(const char* buf, uint32_t len)>;
        output_fn output;

        ikcp() = default;

        explicit ikcp(uint32_t conv_, output_fn out)
            : conv(conv_)
            , output(std::move(out))
        {
            buffer.resize((mtu + IKCP_OVERHEAD) * 3);
        }

        // Set no-delay mode for low latency
        void set_nodelay(int nodelay_, int interval_, int resend, int nc) {
            if (nodelay_ >= 0) {
                nodelay = nodelay_;
                rx_minrto = nodelay_ ? IKCP_RTO_MIN / 3 : IKCP_RTO_MIN;
            }
            if (interval_ >= 0) {
                interval = std::clamp(static_cast<uint32_t>(interval_), 10u, 5000u);
            }
            if (resend >= 0) fastresend = resend;
            if (nc >= 0) nocwnd = nc;
        }

        void set_wndsize(uint32_t sndwnd, uint32_t rcvwnd) {
            if (sndwnd > 0) snd_wnd = sndwnd;
            if (rcvwnd > 0) rcv_wnd = std::max(rcvwnd, IKCP_WND_RCV);
        }

        // --- Send interface ---

        int send(const char* data, int len) {
            if (len <= 0) return -1;

            int count = (len <= static_cast<int>(mss)) ? 1
                : (len + static_cast<int>(mss) - 1) / static_cast<int>(mss);

            if (count > 255) return -2;
            if (count == 0) count = 1;

            for (int i = 0; i < count; ++i) {
                int size = (len > static_cast<int>(mss)) ? static_cast<int>(mss) : len;
                ikcp_seg seg;
                seg.conv = conv;
                seg.cmd = IKCP_CMD_PUSH;
                seg.frg = static_cast<uint8_t>(count - i - 1);
                if (size > 0) {
                    seg.data.assign(data, data + size);
                    seg.len = size;
                }
                snd_queue.push_back(std::move(seg));
                data += size;
                len -= size;
            }
            return 0;
        }

        // --- Receive interface ---

        // Returns size of next complete message, or -1
        int peeksize() const {
            if (rcv_queue.empty()) return -1;

            const auto& seg = rcv_queue.front();
            if (seg.frg == 0) return static_cast<int>(seg.len);

            if (rcv_queue.size() < static_cast<size_t>(seg.frg) + 1)
                return -1;

            int length = 0;
            for (const auto& s : rcv_queue) {
                length += static_cast<int>(s.len);
                if (s.frg == 0) break;
            }
            return length;
        }

        int recv(char* buffer_, int len) {
            int peeklen = peeksize();
            if (peeklen < 0) return -1;
            if (peeklen > len) return -2;

            bool recover = (rcv_queue.size() >= rcv_wnd);

            int offset = 0;
            while (!rcv_queue.empty()) {
                auto& seg = rcv_queue.front();
                if (!seg.data.empty()) {
                    memcpy(buffer_ + offset, seg.data.data(), seg.len);
                    offset += static_cast<int>(seg.len);
                }
                bool last = (seg.frg == 0);
                rcv_queue.pop_front();
                if (last) break;
            }

            // Move from rcv_buf to rcv_queue
            move_rcv_buf_to_queue();

            // Fast recover
            if (recover && rcv_queue.size() < rcv_wnd) {
                probe |= IKCP_ASK_TELL;
            }

            return offset;
        }

        // --- Input: process incoming UDP data ---

        int input(const char* data, int size) {
            if (size < static_cast<int>(IKCP_OVERHEAD)) return -1;

            uint32_t prev_una = snd_una;
            uint32_t maxack = 0;
            bool flag = false;

            while (size >= static_cast<int>(IKCP_OVERHEAD)) {
                uint32_t conv_ = decode32u(data); data += 4;
                if (conv_ != conv) return -1;

                uint8_t cmd = decode8u(data); data += 1;
                uint8_t frg = decode8u(data); data += 1;
                uint16_t wnd = decode16u(data); data += 2;
                uint32_t ts = decode32u(data); data += 4;
                uint32_t sn = decode32u(data); data += 4;
                uint32_t una = decode32u(data); data += 4;
                uint32_t len = decode32u(data); data += 4;

                size -= IKCP_OVERHEAD;
                if (static_cast<uint32_t>(size) < len) return -2;

                if (cmd != IKCP_CMD_PUSH && cmd != IKCP_CMD_ACK &&
                    cmd != IKCP_CMD_WASK && cmd != IKCP_CMD_WINS)
                    return -3;

                rmt_wnd = wnd;
                parse_una(una);
                shrink_buf();

                if (cmd == IKCP_CMD_ACK) {
                    if (itimediff(current, ts) >= 0) {
                        update_ack(itimediff(current, ts));
                    }
                    parse_ack(sn);
                    shrink_buf();
                    if (!flag) {
                        flag = true;
                        maxack = sn;
                    } else if (itimediff(sn, maxack) > 0) {
                        maxack = sn;
                    }
                } else if (cmd == IKCP_CMD_PUSH) {
                    if (itimediff(sn, rcv_nxt + rcv_wnd) < 0) {
                        acklist.emplace_back(sn, ts);
                        if (itimediff(sn, rcv_nxt) >= 0) {
                            ikcp_seg seg;
                            seg.conv = conv_;
                            seg.cmd = cmd;
                            seg.frg = frg;
                            seg.wnd = wnd;
                            seg.ts = ts;
                            seg.sn = sn;
                            seg.una = una;
                            seg.len = len;
                            if (len > 0) {
                                seg.data.assign(data, data + len);
                            }
                            parse_data(std::move(seg));
                        }
                    }
                } else if (cmd == IKCP_CMD_WASK) {
                    probe |= IKCP_ASK_TELL;
                } else if (cmd == IKCP_CMD_WINS) {
                    // do nothing, rmt_wnd already updated
                }

                data += len;
                size -= static_cast<int>(len);
            }

            if (flag) {
                parse_fastack(maxack);
            }

            // Update cwnd
            if (itimediff(snd_una, prev_una) > 0) {
                if (cwnd < rmt_wnd) {
                    if (cwnd < ssthresh) {
                        cwnd++;
                        incr += mss;
                    } else {
                        if (incr < mss) incr = mss;
                        incr += (mss * mss) / incr + (mss / 16);
                        if ((cwnd + 1) * mss <= incr) {
                            cwnd = (incr + mss - 1) / ((mss > 0) ? mss : 1);
                        }
                    }
                    if (cwnd > rmt_wnd) {
                        cwnd = rmt_wnd;
                        incr = rmt_wnd * mss;
                    }
                }
            }

            return 0;
        }

        // --- Flush: output pending data ---

        void flush() {
            if (!updated) return;

            ikcp_seg seg_ack;
            seg_ack.conv = conv;
            seg_ack.cmd = IKCP_CMD_ACK;
            seg_ack.wnd = static_cast<uint16_t>(std::min(rcv_wnd - static_cast<uint32_t>(rcv_queue.size()), rcv_wnd));
            seg_ack.una = rcv_nxt;

            // Flush ACKs
            char* ptr = buffer.data();
            for (auto& [sn, ts] : acklist) {
                if ((ptr - buffer.data()) + static_cast<int>(IKCP_OVERHEAD) > static_cast<int>(mtu)) {
                    output(buffer.data(), static_cast<uint32_t>(ptr - buffer.data()));
                    ptr = buffer.data();
                }
                seg_ack.sn = sn;
                seg_ack.ts = ts;
                ptr = ikcp_encode_seg(ptr, seg_ack);
            }
            acklist.clear();

            // Probe window size
            if (rmt_wnd == 0) {
                if (probe_wait == 0) {
                    probe_wait = IKCP_PROBE_INIT;
                    ts_probe = current + probe_wait;
                } else if (itimediff(current, ts_probe) >= 0) {
                    probe_wait = std::max(probe_wait, IKCP_PROBE_INIT);
                    probe_wait += probe_wait / 2;
                    probe_wait = std::min(probe_wait, IKCP_PROBE_LIMIT);
                    ts_probe = current + probe_wait;
                    probe |= IKCP_ASK_SEND;
                }
            } else {
                ts_probe = 0;
                probe_wait = 0;
            }

            // Flush window probing commands
            if (probe & IKCP_ASK_SEND) {
                ikcp_seg seg_probe;
                seg_probe.conv = conv;
                seg_probe.cmd = IKCP_CMD_WASK;
                seg_probe.wnd = seg_ack.wnd;
                seg_probe.una = rcv_nxt;
                if ((ptr - buffer.data()) + static_cast<int>(IKCP_OVERHEAD) > static_cast<int>(mtu)) {
                    output(buffer.data(), static_cast<uint32_t>(ptr - buffer.data()));
                    ptr = buffer.data();
                }
                ptr = ikcp_encode_seg(ptr, seg_probe);
            }
            if (probe & IKCP_ASK_TELL) {
                ikcp_seg seg_probe;
                seg_probe.conv = conv;
                seg_probe.cmd = IKCP_CMD_WINS;
                seg_probe.wnd = seg_ack.wnd;
                seg_probe.una = rcv_nxt;
                if ((ptr - buffer.data()) + static_cast<int>(IKCP_OVERHEAD) > static_cast<int>(mtu)) {
                    output(buffer.data(), static_cast<uint32_t>(ptr - buffer.data()));
                    ptr = buffer.data();
                }
                ptr = ikcp_encode_seg(ptr, seg_probe);
            }
            probe = 0;

            // Calculate send window
            uint32_t cwnd_ = std::min(snd_wnd, rmt_wnd);
            if (!nocwnd) cwnd_ = std::min(cwnd, cwnd_);

            // Move segments from snd_queue to snd_buf
            while (itimediff(snd_nxt, snd_una + cwnd_) < 0 && !snd_queue.empty()) {
                auto& newseg = snd_queue.front();
                newseg.conv = conv;
                newseg.cmd = IKCP_CMD_PUSH;
                newseg.wnd = seg_ack.wnd;
                newseg.ts = current;
                newseg.sn = snd_nxt++;
                newseg.una = rcv_nxt;
                newseg.resendts = current;
                newseg.rto = rx_rto;
                newseg.fastack = 0;
                newseg.xmit = 0;
                snd_buf.push_back(std::move(newseg));
                snd_queue.pop_front();
            }

            uint32_t resent = (fastresend > 0) ? fastresend : 0xffffffff;
            uint32_t rtomin = (nodelay == 0) ? (rx_rto >> 3) : 0;

            bool lost = false;
            bool change = false;

            for (auto& seg : snd_buf) {
                bool needsend = false;
                if (seg.xmit == 0) {
                    // First transmit
                    needsend = true;
                    seg.xmit++;
                    seg.rto = rx_rto;
                    seg.resendts = current + seg.rto + rtomin;
                } else if (itimediff(current, seg.resendts) >= 0) {
                    // Timeout retransmit
                    needsend = true;
                    seg.xmit++;
                    xmit++;
                    if (nodelay == 0) {
                        seg.rto += std::max(seg.rto, rx_rto);
                    } else {
                        uint32_t step = (nodelay < 2) ? seg.rto : rx_rto;
                        seg.rto += step / 2;
                    }
                    seg.rto = std::min(seg.rto, IKCP_RTO_MAX);
                    seg.resendts = current + seg.rto;
                    lost = true;
                } else if (seg.fastack >= resent) {
                    // Fast retransmit
                    if (seg.xmit <= fastlimit || fastlimit == 0) {
                        needsend = true;
                        seg.xmit++;
                        seg.fastack = 0;
                        seg.resendts = current + seg.rto;
                        change = true;
                    }
                }

                if (needsend) {
                    seg.ts = current;
                    seg.wnd = seg_ack.wnd;
                    seg.una = rcv_nxt;

                    uint32_t need = IKCP_OVERHEAD + seg.len;
                    if ((ptr - buffer.data()) + static_cast<int>(need) > static_cast<int>(mtu)) {
                        output(buffer.data(), static_cast<uint32_t>(ptr - buffer.data()));
                        ptr = buffer.data();
                    }
                    ptr = ikcp_encode_seg(ptr, seg);
                    if (seg.len > 0) {
                        memcpy(ptr, seg.data.data(), seg.len);
                        ptr += seg.len;
                    }

                    if (seg.xmit >= dead_link) {
                        state = static_cast<uint32_t>(-1);
                    }
                }
            }

            // Flush remaining
            if (ptr > buffer.data()) {
                output(buffer.data(), static_cast<uint32_t>(ptr - buffer.data()));
            }

            // Congestion control
            if (change) {
                uint32_t inflight = snd_nxt - snd_una;
                ssthresh = std::max(inflight / 2, 2u);
                cwnd = ssthresh + resent;
                incr = cwnd * mss;
            }
            if (lost) {
                ssthresh = std::max(cwnd_ / 2, 2u);
                cwnd = 1;
                incr = mss;
            }
            if (cwnd < 1) {
                cwnd = 1;
                incr = mss;
            }
        }

        // --- Update: called periodically ---

        void update(uint32_t current_ms) {
            current = current_ms;
            if (!updated) {
                updated = 1;
                ts_flush = current;
            }

            int32_t slap = itimediff(current, ts_flush);
            if (slap >= 10000 || slap < -10000) {
                ts_flush = current;
                slap = 0;
            }

            if (slap >= 0) {
                ts_flush += interval;
                if (itimediff(current, ts_flush) >= 0) {
                    ts_flush = current + interval;
                }
                flush();
            }
        }

        // Returns when next update should be called
        uint32_t check(uint32_t current_ms) const {
            if (!updated) return current_ms;

            uint32_t ts_flush_ = ts_flush;
            int32_t slap = itimediff(current_ms, ts_flush_);
            if (slap >= 10000 || slap < -10000) {
                return current_ms;
            }
            if (slap >= 0) return current_ms;

            uint32_t tm_flush = static_cast<uint32_t>(itimediff(ts_flush_, current_ms));
            uint32_t tm_packet = 0xffffffff;
            for (const auto& seg : snd_buf) {
                int32_t diff = itimediff(seg.resendts, current_ms);
                if (diff <= 0) return current_ms;
                if (static_cast<uint32_t>(diff) < tm_packet) tm_packet = static_cast<uint32_t>(diff);
            }

            uint32_t minimal = std::min(tm_flush, tm_packet);
            minimal = std::min(minimal, interval);
            return current_ms + minimal;
        }

        // --- Waitsnd: number of segments waiting to be sent ---
        uint32_t waitsnd() const {
            return static_cast<uint32_t>(snd_buf.size() + snd_queue.size());
        }

    private:
        void parse_una(uint32_t una) {
            snd_buf.remove_if([una](const ikcp_seg& seg) {
                return itimediff(una, seg.sn) > 0;
            });
        }

        void shrink_buf() {
            snd_una = snd_buf.empty() ? snd_nxt : snd_buf.front().sn;
        }

        void parse_ack(uint32_t sn) {
            if (itimediff(sn, snd_una) < 0 || itimediff(sn, snd_nxt) >= 0) return;
            snd_buf.remove_if([sn](const ikcp_seg& seg) {
                return seg.sn == sn;
            });
        }

        void parse_fastack(uint32_t sn) {
            if (itimediff(sn, snd_una) < 0 || itimediff(sn, snd_nxt) >= 0) return;
            for (auto& seg : snd_buf) {
                if (itimediff(sn, seg.sn) < 0) break;
                if (sn != seg.sn) seg.fastack++;
            }
        }

        void update_ack(int32_t rtt) {
            if (rx_srtt == 0) {
                rx_srtt = rtt;
                rx_rttval = rtt / 2;
            } else {
                int32_t delta = rtt - static_cast<int32_t>(rx_srtt);
                if (delta < 0) delta = -delta;
                rx_rttval = (3 * rx_rttval + delta) / 4;
                rx_srtt = (7 * rx_srtt + rtt) / 8;
                if (rx_srtt < 1) rx_srtt = 1;
            }
            uint32_t rto = rx_srtt + std::max(interval, 4 * rx_rttval);
            rx_rto = std::clamp(rto, rx_minrto, IKCP_RTO_MAX);
        }

        void parse_data(ikcp_seg&& newseg) {
            uint32_t sn = newseg.sn;
            if (itimediff(sn, rcv_nxt + rcv_wnd) >= 0 || itimediff(sn, rcv_nxt) < 0) {
                return;
            }

            // Insert into rcv_buf (ordered by sn), skip duplicate
            bool repeat = false;
            auto it = rcv_buf.end();
            while (it != rcv_buf.begin()) {
                auto prev = std::prev(it);
                if (prev->sn == sn) { repeat = true; break; }
                if (itimediff(prev->sn, sn) < 0) break;
                it = prev;
            }
            if (!repeat) {
                rcv_buf.insert(it, std::move(newseg));
            }

            move_rcv_buf_to_queue();
        }

        void move_rcv_buf_to_queue() {
            while (!rcv_buf.empty()) {
                auto& seg = rcv_buf.front();
                if (seg.sn != rcv_nxt || rcv_queue.size() >= rcv_wnd) break;
                rcv_queue.push_back(std::move(seg));
                rcv_buf.pop_front();
                rcv_nxt++;
            }
        }
    };

}  // namespace pump::scheduler::kcp::common

#endif //ENV_SCHEDULER_KCP_COMMON_IKCP_HH
