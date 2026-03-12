#pragma once

#include <cstring>
#include <string>
#include "env/scheduler/net/common/frame.hh"

namespace inference::protocol {

// Response flags (first byte of payload after length-prefix unpacker strips the 4-byte header)
enum flags : uint8_t {
    TOKEN = 0,
    EOS   = 1,
    ERROR = 2,
};

// Parse prompt from received frame
// (length_prefix_unpacker already stripped the 4-byte length header)
inline std::string parse_prompt(const pump::scheduler::net::net_frame& frame) {
    return {frame.data(), frame.size()};
}

// Encode a token response: [flags:1][text:N]
// Returns heap-allocated buffer (ownership transfers to tcp::send)
inline auto encode_token(const char* text, uint32_t text_len) {
    uint32_t total = 1 + text_len;
    auto* buf = new char[total];
    buf[0] = TOKEN;
    memcpy(buf + 1, text, text_len);
    return std::pair{buf, total};
}

// Encode EOS response: [flags:1]
inline auto encode_eos() {
    auto* buf = new char[1];
    buf[0] = EOS;
    return std::pair{buf, uint32_t{1}};
}

// Encode error response: [flags:1][message:N]
inline auto encode_error(const char* msg) {
    auto msg_len = static_cast<uint32_t>(strlen(msg));
    uint32_t total = 1 + msg_len;
    auto* buf = new char[total];
    buf[0] = ERROR;
    memcpy(buf + 1, msg, msg_len);
    return std::pair{buf, total};
}

} // namespace inference::protocol
