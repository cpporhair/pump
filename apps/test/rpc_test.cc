
#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <cassert>
#include <cstring>
#include <variant>
#include <array>
#include <bits/types/struct_iovec.h>

#include "pump/sender/just.hh"

#include "env/scheduler/rpc/common/struct.hh"
#include "env/scheduler/rpc/common/protocol.hh"
#include "env/scheduler/rpc/state/pending_map.hh"
#include "env/scheduler/rpc/state/registry.hh"
#include "env/scheduler/rpc/service/service_type.hh"
#include "env/scheduler/rpc/service/service.hh"
#include "env/scheduler/rpc/service/dispatch.hh"

using namespace pump::scheduler::rpc::common;
using namespace pump::scheduler::rpc::state;
using namespace pump::scheduler::net::common;

namespace rpc_svc = pump::scheduler::rpc::service;
using rpc_svc::service_type;
using rpc_svc::has_handle_concept;
using rpc_svc::get_service_class_by_id;
using rpc_svc::service_decode;

// ============================================================================
// Test infrastructure
// ============================================================================

static int test_count = 0;
static int pass_count = 0;
static int fail_count = 0;

#define TEST(name) \
    do { \
        test_count++; \
        std::cout << "  TEST: " << name << " ... " << std::flush; \
    } while(0)

#define PASS() \
    do { \
        pass_count++; \
        std::cout << "PASS" << std::endl; \
    } while(0)

#define FAIL(msg) \
    do { \
        fail_count++; \
        std::cout << "FAIL: " << msg << std::endl; \
    } while(0)

// ============================================================================
// Example service definitions (plan.md §2.1, §9.2)
// ============================================================================

// service_001: simple add service
template <>
struct rpc_svc::service<service_type::service_001> {
    struct add_req { int a; int b; };
    struct add_resp { int result; };
    using message_type = std::variant<add_req, add_resp>;

    static message_type decode(uint8_t msg_type, const char* payload, size_t len) {
        switch (msg_type) {
            case 0x01: {
                add_req r;
                std::memcpy(&r, payload, sizeof(add_req));
                return r;
            }
            case 0x02: {
                add_resp r;
                std::memcpy(&r, payload, sizeof(add_resp));
                return r;
            }
            default:
                throw protocol_error("service_001: unknown msg_type");
        }
    }

    static size_t encode(const add_req& msg, std::array<iovec, 8>& buf) {
        buf[0].iov_base = const_cast<add_req*>(&msg);
        buf[0].iov_len = sizeof(add_req);
        return 1;
    }

    static size_t encode(const add_resp& msg, std::array<iovec, 8>& buf) {
        buf[0].iov_base = const_cast<add_resp*>(&msg);
        buf[0].iov_len = sizeof(add_resp);
        return 1;
    }

    static constexpr bool is_service = true;

    static auto handle(add_req&& req) {
        return pump::sender::just(add_resp{req.a + req.b});
    }
};

// service_002: sub and mul service (multiple handle overloads)
template <>
struct rpc_svc::service<service_type::service_002> {
    struct sub_req { int a; int b; };
    struct mul_req { int a; int b; };
    struct sub_resp { int result; };
    struct mul_resp { int result; };
    using message_type = std::variant<sub_req, mul_req, sub_resp, mul_resp>;

    static message_type decode(uint8_t msg_type, const char* payload, size_t len) {
        switch (msg_type) {
            case 0x01: {
                sub_req r;
                std::memcpy(&r, payload, sizeof(sub_req));
                return r;
            }
            case 0x02: {
                mul_req r;
                std::memcpy(&r, payload, sizeof(mul_req));
                return r;
            }
            case 0x03: {
                sub_resp r;
                std::memcpy(&r, payload, sizeof(sub_resp));
                return r;
            }
            case 0x04: {
                mul_resp r;
                std::memcpy(&r, payload, sizeof(mul_resp));
                return r;
            }
            default:
                throw protocol_error("service_002: unknown msg_type");
        }
    }

    static size_t encode(const sub_resp& msg, std::array<iovec, 8>& buf) {
        buf[0].iov_base = const_cast<sub_resp*>(&msg);
        buf[0].iov_len = sizeof(sub_resp);
        return 1;
    }

    static size_t encode(const mul_resp& msg, std::array<iovec, 8>& buf) {
        buf[0].iov_base = const_cast<mul_resp*>(&msg);
        buf[0].iov_len = sizeof(mul_resp);
        return 1;
    }

    static constexpr bool is_service = true;

    static auto handle(sub_req&& req) {
        return pump::sender::just(sub_resp{req.a - req.b});
    }

    static auto handle(mul_req&& req) {
        return pump::sender::just(mul_resp{req.a * req.b});
    }
};

// Convenience aliases
using svc1 = rpc_svc::service<service_type::service_001>;
using svc2 = rpc_svc::service<service_type::service_002>;
using svc3 = rpc_svc::service<service_type::service_003>;

// ============================================================================
// 1. rpc_header tests
// ============================================================================

void test_rpc_header() {
    std::cout << "\n[rpc_header tests]" << std::endl;

    TEST("rpc_header size is 12 bytes"); {
        if (sizeof(rpc_header) == 12) PASS();
        else FAIL("sizeof(rpc_header) != 12");
    }

    TEST("rpc_header fields layout"); {
        rpc_header hdr{};
        hdr.total_len = 100;
        hdr.request_id = 42;
        hdr.module_id = 1;
        hdr.msg_type = 0x01;
        hdr.flags = static_cast<uint8_t>(rpc_flags::request);

        if (hdr.total_len == 100 && hdr.request_id == 42 &&
            hdr.module_id == 1 && hdr.msg_type == 0x01 &&
            hdr.flags == 0x00)
            PASS();
        else
            FAIL("field values mismatch");
    }

    TEST("payload_len calculation"); {
        rpc_header hdr{};
        hdr.total_len = sizeof(rpc_header) + 64;
        if (payload_len(&hdr) == 64) PASS();
        else FAIL("payload_len mismatch");
    }

    TEST("payload_ptr offset"); {
        rpc_header hdr{};
        auto* base = reinterpret_cast<const char*>(&hdr);
        auto* p = payload_ptr(&hdr);
        if (static_cast<size_t>(p - base) == sizeof(rpc_header)) PASS();
        else FAIL("payload_ptr offset mismatch");
    }

    TEST("rpc_flags values"); {
        if (static_cast<uint8_t>(rpc_flags::request) == 0x00 &&
            static_cast<uint8_t>(rpc_flags::response) == 0x01 &&
            static_cast<uint8_t>(rpc_flags::push) == 0x02)
            PASS();
        else
            FAIL("rpc_flags values mismatch");
    }
}

// ============================================================================
// 2. Error types tests
// ============================================================================

void test_error_types() {
    std::cout << "\n[error types tests]" << std::endl;

    TEST("rpc_error is runtime_error"); {
        try {
            throw rpc_error("test");
        } catch (const std::runtime_error& e) {
            if (std::string(e.what()) == "test") PASS();
            else FAIL("wrong message");
        } catch (...) {
            FAIL("not caught as runtime_error");
        }
    }

    TEST("frame_error inherits rpc_error"); {
        try {
            throw frame_error("frame err");
        } catch (const rpc_error& e) {
            if (std::string(e.what()) == "frame err") PASS();
            else FAIL("wrong message");
        } catch (...) {
            FAIL("not caught as rpc_error");
        }
    }

    TEST("codec_error inherits rpc_error"); {
        try { throw codec_error("codec err"); }
        catch (const rpc_error&) { PASS(); }
        catch (...) { FAIL("not caught as rpc_error"); }
    }

    TEST("protocol_error inherits rpc_error"); {
        try { throw protocol_error("proto err"); }
        catch (const rpc_error&) { PASS(); }
        catch (...) { FAIL("not caught as rpc_error"); }
    }

    TEST("dispatch_error inherits rpc_error"); {
        try { throw dispatch_error("dispatch err"); }
        catch (const rpc_error&) { PASS(); }
        catch (...) { FAIL("not caught as rpc_error"); }
    }

    TEST("pending_full_error inherits rpc_error"); {
        try {
            throw pending_full_error();
        } catch (const rpc_error& e) {
            if (std::string(e.what()) == "pending requests map is full") PASS();
            else FAIL("wrong message");
        } catch (...) {
            FAIL("not caught as rpc_error");
        }
    }

    TEST("error types are distinguishable"); {
        bool caught_frame = false;
        try { throw frame_error("f"); }
        catch (const frame_error&) { caught_frame = true; }
        catch (...) {}

        bool caught_codec = false;
        try { throw codec_error("c"); }
        catch (const codec_error&) { caught_codec = true; }
        catch (...) {}

        bool caught_proto = false;
        try { throw protocol_error("p"); }
        catch (const protocol_error&) { caught_proto = true; }
        catch (...) {}

        bool caught_dispatch = false;
        try { throw dispatch_error("d"); }
        catch (const dispatch_error&) { caught_dispatch = true; }
        catch (...) {}

        if (caught_frame && caught_codec && caught_proto && caught_dispatch)
            PASS();
        else
            FAIL("some error type not distinguishable");
    }
}

// ============================================================================
// 3. pending_requests_map tests
// ============================================================================

void test_pending_map() {
    std::cout << "\n[pending_requests_map tests]" << std::endl;

    TEST("insert and contains"); {
        pending_requests_map pm(16);
        bool ok = pm.insert(0, [](recv_frame&&) {});
        if (ok && pm.contains(0)) PASS();
        else FAIL("insert failed or contains returned false");
    }

    TEST("extract returns callback"); {
        pending_requests_map pm(16);
        int called = 0;
        pm.insert(5, [&called](recv_frame&&) { called++; });

        auto cb = pm.extract(5);
        if (!cb.has_value()) { FAIL("extract returned nullopt"); return; }

        char* buf = new char[4];
        std::memset(buf, 0, 4);
        recv_frame frame(buf, 4);
        (*cb)(std::move(frame));

        if (called == 1 && !pm.contains(5)) PASS();
        else FAIL("callback not invoked or still in map");
    }

    TEST("extract non-existent returns nullopt"); {
        pending_requests_map pm(16);
        auto cb = pm.extract(99);
        if (!cb.has_value()) PASS();
        else FAIL("expected nullopt");
    }

    TEST("contains returns false for empty slot"); {
        pending_requests_map pm(16);
        if (!pm.contains(0)) PASS();
        else FAIL("expected false");
    }

    TEST("insert collision returns false"); {
        pending_requests_map pm(4);
        pm.insert(0, [](recv_frame&&) {});
        bool ok = pm.insert(4, [](recv_frame&&) {});
        if (!ok) PASS();
        else FAIL("expected collision");
    }

    TEST("multiple inserts and extracts"); {
        pending_requests_map pm(256);
        for (uint32_t i = 0; i < 100; i++) {
            pm.insert(i, [i](recv_frame&&) {});
        }

        bool all_contained = true;
        for (uint32_t i = 0; i < 100; i++) {
            if (!pm.contains(i)) { all_contained = false; break; }
        }

        for (uint32_t i = 0; i < 100; i += 2) {
            pm.extract(i);
        }

        bool evens_gone = true;
        bool odds_remain = true;
        for (uint32_t i = 0; i < 100; i++) {
            if (i % 2 == 0) {
                if (pm.contains(i)) { evens_gone = false; break; }
            } else {
                if (!pm.contains(i)) { odds_remain = false; break; }
            }
        }

        if (all_contained && evens_gone && odds_remain) PASS();
        else FAIL("insert/extract pattern failed");
    }

    TEST("fail_all clears all occupied slots"); {
        pending_requests_map pm(16);
        pm.insert(0, [](recv_frame&&) {});
        pm.insert(1, [](recv_frame&&) {});
        pm.insert(2, [](recv_frame&&) {});

        pm.fail_all(std::make_exception_ptr(rpc_error("test")));

        if (!pm.contains(0) && !pm.contains(1) && !pm.contains(2)) PASS();
        else FAIL("fail_all did not clear slots");
    }

    TEST("insert after extract reuses slot"); {
        pending_requests_map pm(4);
        pm.insert(0, [](recv_frame&&) {});
        pm.extract(0);
        bool ok = pm.insert(4, [](recv_frame&&) {});
        if (ok && pm.contains(4)) PASS();
        else FAIL("reuse after extract failed");
    }
}

// ============================================================================
// 4. session_rpc_state and registry tests
// ============================================================================

void test_registry() {
    std::cout << "\n[registry tests]" << std::endl;

    TEST("session_rpc_state initial values"); {
        session_rpc_state state{};
        if (state.next_request_id == 0) PASS();
        else FAIL("initial request_id not 0");
    }

    TEST("rpc_state_registry get_or_bind creates state"); {
        rpc_state_registry reg;
        session_id_t sid{42};
        auto& state = reg.get_or_bind(sid);
        if (state.next_request_id == 0) PASS();
        else FAIL("new state not initialized");
    }

    TEST("get_or_bind returns same state"); {
        rpc_state_registry reg;
        session_id_t sid{42};
        auto& s1 = reg.get_or_bind(sid);
        s1.next_request_id = 10;
        auto& s2 = reg.get_or_bind(sid);
        if (s2.next_request_id == 10) PASS();
        else FAIL("not same state");
    }

    TEST("different sessions have independent state"); {
        rpc_state_registry reg;
        session_id_t sid1{1};
        session_id_t sid2{2};
        auto& s1 = reg.get_or_bind(sid1);
        auto& s2 = reg.get_or_bind(sid2);
        s1.next_request_id = 100;
        s2.next_request_id = 200;
        if (reg.get_or_bind(sid1).next_request_id == 100 &&
            reg.get_or_bind(sid2).next_request_id == 200)
            PASS();
        else
            FAIL("states not independent");
    }

    TEST("unbind removes state"); {
        rpc_state_registry reg;
        session_id_t sid{42};
        auto& s = reg.get_or_bind(sid);
        s.next_request_id = 99;
        reg.unbind(sid);
        auto& s2 = reg.get_or_bind(sid);
        if (s2.next_request_id == 0) PASS();
        else FAIL("state not reset after unbind");
    }

    TEST("get_registry returns thread_local instance"); {
        auto& reg1 = get_registry();
        auto& reg2 = get_registry();
        if (&reg1 == &reg2) PASS();
        else FAIL("not same instance");
    }

    TEST("request_id auto-increment pattern"); {
        rpc_state_registry reg;
        session_id_t sid{1};
        auto& state = reg.get_or_bind(sid);
        auto rid0 = state.next_request_id++;
        auto rid1 = state.next_request_id++;
        auto rid2 = state.next_request_id++;
        if (rid0 == 0 && rid1 == 1 && rid2 == 2) PASS();
        else FAIL("auto-increment pattern broken");
    }

    TEST("pending_map integration with registry"); {
        rpc_state_registry reg;
        session_id_t sid{1};
        auto& state = reg.get_or_bind(sid);
        auto rid = state.next_request_id++;
        bool ok = state.pending.insert(rid, [](recv_frame&&) {});
        if (ok && state.pending.contains(rid)) PASS();
        else FAIL("pending_map integration failed");
    }
}

// ============================================================================
// 5. service_type and service template tests
// ============================================================================

void test_service() {
    std::cout << "\n[service tests]" << std::endl;

    TEST("service_type enum values"); {
        if (static_cast<uint16_t>(service_type::service_001) == 0 &&
            static_cast<uint16_t>(service_type::service_002) == 1 &&
            static_cast<uint16_t>(service_type::service_003) == 2)
            PASS();
        else
            FAIL("service_type values mismatch");
    }

    TEST("unspecialized service has is_service=false"); {
        if (!svc3::is_service) PASS();
        else FAIL("expected is_service=false");
    }

    TEST("service_001 specialization has is_service=true"); {
        if (svc1::is_service) PASS();
        else FAIL("expected is_service=true");
    }

    TEST("service_002 specialization has is_service=true"); {
        if (svc2::is_service) PASS();
        else FAIL("expected is_service=true");
    }

    TEST("has_handle_concept for service_001 add_req"); {
        if constexpr (has_handle_concept<svc1, svc1::add_req>) PASS();
        else FAIL("expected has_handle_concept=true");
    }

    TEST("has_handle_concept for service_002 sub_req"); {
        if constexpr (has_handle_concept<svc2, svc2::sub_req>) PASS();
        else FAIL("expected has_handle_concept=true");
    }

    TEST("has_handle_concept for service_002 mul_req"); {
        if constexpr (has_handle_concept<svc2, svc2::mul_req>) PASS();
        else FAIL("expected has_handle_concept=true");
    }

    TEST("service_001 decode add_req"); {
        svc1::add_req orig{3, 7};
        auto msg = svc1::decode(0x01, reinterpret_cast<const char*>(&orig), sizeof(orig));
        if (auto* p = std::get_if<svc1::add_req>(&msg)) {
            if (p->a == 3 && p->b == 7) PASS();
            else FAIL("decoded values mismatch");
        } else {
            FAIL("wrong variant alternative");
        }
    }

    TEST("service_001 decode add_resp"); {
        svc1::add_resp orig{42};
        auto msg = svc1::decode(0x02, reinterpret_cast<const char*>(&orig), sizeof(orig));
        if (auto* p = std::get_if<svc1::add_resp>(&msg)) {
            if (p->result == 42) PASS();
            else FAIL("decoded value mismatch");
        } else {
            FAIL("wrong variant alternative");
        }
    }

    TEST("service_001 decode unknown msg_type throws"); {
        try {
            svc1::decode(0xFF, nullptr, 0);
            FAIL("expected exception");
        } catch (const protocol_error&) {
            PASS();
        } catch (...) {
            FAIL("wrong exception type");
        }
    }

    TEST("service_001 encode add_req"); {
        svc1::add_req msg{5, 10};
        std::array<iovec, 8> buf{};
        auto cnt = svc1::encode(msg, buf);
        if (cnt == 1 && buf[0].iov_len == sizeof(svc1::add_req)) PASS();
        else FAIL("encode result mismatch");
    }

    TEST("service_002 decode sub_req"); {
        svc2::sub_req orig{10, 3};
        auto msg = svc2::decode(0x01, reinterpret_cast<const char*>(&orig), sizeof(orig));
        if (auto* p = std::get_if<svc2::sub_req>(&msg)) {
            if (p->a == 10 && p->b == 3) PASS();
            else FAIL("decoded values mismatch");
        } else {
            FAIL("wrong variant alternative");
        }
    }

    TEST("service_002 decode mul_req"); {
        svc2::mul_req orig{4, 5};
        auto msg = svc2::decode(0x02, reinterpret_cast<const char*>(&orig), sizeof(orig));
        if (auto* p = std::get_if<svc2::mul_req>(&msg)) {
            if (p->a == 4 && p->b == 5) PASS();
            else FAIL("decoded values mismatch");
        } else {
            FAIL("wrong variant alternative");
        }
    }
}

// ============================================================================
// 6. dispatch tests
// ============================================================================

void test_dispatch() {
    std::cout << "\n[dispatch tests]" << std::endl;

    TEST("get_service_class_by_id for service_001"); {
        auto svc = get_service_class_by_id<
            service_type::service_001,
            service_type::service_002>(service_type::service_001);
        bool ok = std::visit([](auto&& s) {
            return std::decay_t<decltype(s)>::is_service;
        }, svc);
        if (ok) PASS();
        else FAIL("service not found");
    }

    TEST("get_service_class_by_id for service_002"); {
        auto svc = get_service_class_by_id<
            service_type::service_001,
            service_type::service_002>(service_type::service_002);
        bool ok = std::visit([](auto&& s) {
            return std::decay_t<decltype(s)>::is_service;
        }, svc);
        if (ok) PASS();
        else FAIL("service not found");
    }

    TEST("get_service_class_by_id for unknown throws"); {
        try {
            get_service_class_by_id<
                service_type::service_001,
                service_type::service_002>(service_type::service_003);
            FAIL("expected exception");
        } catch (const dispatch_error&) {
            PASS();
        } catch (...) {
            FAIL("wrong exception type");
        }
    }

    TEST("service_decode for service_001"); {
        svc1::add_req orig{7, 8};
        auto result = service_decode<
            service_type::service_001,
            service_type::service_002>(
            static_cast<uint16_t>(service_type::service_001),
            0x01,
            reinterpret_cast<const char*>(&orig),
            sizeof(orig));
        bool ok = std::visit([](auto&& inner) {
            return std::visit([](auto&& msg) {
                using T = std::decay_t<decltype(msg)>;
                if constexpr (std::is_same_v<T, svc1::add_req>) {
                    return msg.a == 7 && msg.b == 8;
                }
                return false;
            }, inner);
        }, result);
        if (ok) PASS();
        else FAIL("service_decode result mismatch");
    }

    TEST("service_decode for unknown module_id throws"); {
        try {
            service_decode<
                service_type::service_001,
                service_type::service_002>(
                static_cast<uint16_t>(service_type::service_003),
                0x01, nullptr, 0);
            FAIL("expected exception");
        } catch (const dispatch_error&) {
            PASS();
        } catch (...) {
            FAIL("wrong exception type");
        }
    }
}

// ============================================================================
// 7. Protocol concept tests
// ============================================================================

void test_protocol_concept() {
    std::cout << "\n[Protocol concept tests]" << std::endl;

    TEST("service_001 satisfies Protocol concept"); {
        if constexpr (Protocol<svc1>) PASS();
        else FAIL("service_001 does not satisfy Protocol");
    }

    TEST("service_002 satisfies Protocol concept"); {
        if constexpr (Protocol<svc2>) PASS();
        else FAIL("service_002 does not satisfy Protocol");
    }

    TEST("unspecialized service_003 does not satisfy Protocol"); {
        if constexpr (!Protocol<svc3>) PASS();
        else FAIL("service_003 should not satisfy Protocol");
    }
}

// ============================================================================
// 8. RPC header serialization round-trip test
// ============================================================================

void test_header_roundtrip() {
    std::cout << "\n[header round-trip tests]" << std::endl;

    TEST("rpc_header write and read back"); {
        svc1::add_req req{100, 200};

        size_t frame_size = sizeof(rpc_header) + sizeof(svc1::add_req);
        char* buf = new char[frame_size];

        auto* hdr = reinterpret_cast<rpc_header*>(buf);
        hdr->total_len = static_cast<uint32_t>(frame_size);
        hdr->request_id = 42;
        hdr->module_id = static_cast<uint16_t>(service_type::service_001);
        hdr->msg_type = 0x01;
        hdr->flags = static_cast<uint8_t>(rpc_flags::request);

        std::memcpy(buf + sizeof(rpc_header), &req, sizeof(svc1::add_req));

        auto* read_hdr = reinterpret_cast<const rpc_header*>(buf);
        auto pl = payload_len(read_hdr);
        auto* pl_ptr = payload_ptr(read_hdr);

        auto msg = svc1::decode(read_hdr->msg_type, pl_ptr, pl);

        bool ok = false;
        if (auto* p = std::get_if<svc1::add_req>(&msg)) {
            ok = (p->a == 100 && p->b == 200 &&
                  read_hdr->request_id == 42 &&
                  read_hdr->module_id == static_cast<uint16_t>(service_type::service_001) &&
                  read_hdr->flags == static_cast<uint8_t>(rpc_flags::request));
        }

        delete[] buf;

        if (ok) PASS();
        else FAIL("round-trip failed");
    }

    TEST("response header flags"); {
        rpc_header hdr{};
        hdr.flags = static_cast<uint8_t>(rpc_flags::response);
        if (hdr.flags == 0x01) PASS();
        else FAIL("response flag value mismatch");
    }

    TEST("push header flags"); {
        rpc_header hdr{};
        hdr.flags = static_cast<uint8_t>(rpc_flags::push);
        if (hdr.flags == 0x02) PASS();
        else FAIL("push flag value mismatch");
    }
}

// ============================================================================
// 9. End-to-end encode/decode test (simulated RPC flow)
// ============================================================================

void test_e2e_encode_decode() {
    std::cout << "\n[end-to-end encode/decode tests]" << std::endl;

    TEST("service_001 add: encode request -> build frame -> decode"); {
        svc1::add_req req{15, 27};

        std::array<iovec, 8> iov_buf{};
        auto cnt = svc1::encode(req, iov_buf);

        size_t payload_size = 0;
        for (size_t i = 0; i < cnt; i++)
            payload_size += iov_buf[i].iov_len;

        size_t frame_size = sizeof(rpc_header) + payload_size;
        char* frame_buf = new char[frame_size];

        auto* hdr = reinterpret_cast<rpc_header*>(frame_buf);
        hdr->total_len = static_cast<uint32_t>(frame_size);
        hdr->request_id = 7;
        hdr->module_id = static_cast<uint16_t>(service_type::service_001);
        hdr->msg_type = 0x01;
        hdr->flags = static_cast<uint8_t>(rpc_flags::request);

        char* dst = frame_buf + sizeof(rpc_header);
        for (size_t i = 0; i < cnt; i++) {
            std::memcpy(dst, iov_buf[i].iov_base, iov_buf[i].iov_len);
            dst += iov_buf[i].iov_len;
        }

        auto* recv_hdr = reinterpret_cast<const rpc_header*>(frame_buf);
        auto msg = svc1::decode(recv_hdr->msg_type,
                                payload_ptr(recv_hdr),
                                payload_len(recv_hdr));

        bool ok = false;
        if (auto* p = std::get_if<svc1::add_req>(&msg)) {
            ok = (p->a == 15 && p->b == 27);
        }

        delete[] frame_buf;

        if (ok) PASS();
        else FAIL("e2e encode/decode failed");
    }

    TEST("service_002 sub: full round trip"); {
        svc2::sub_req req{50, 13};

        size_t frame_size = sizeof(rpc_header) + sizeof(svc2::sub_req);
        char* frame_buf = new char[frame_size];

        auto* hdr = reinterpret_cast<rpc_header*>(frame_buf);
        hdr->total_len = static_cast<uint32_t>(frame_size);
        hdr->request_id = 0;
        hdr->module_id = static_cast<uint16_t>(service_type::service_002);
        hdr->msg_type = 0x01;
        hdr->flags = static_cast<uint8_t>(rpc_flags::request);
        std::memcpy(frame_buf + sizeof(rpc_header), &req, sizeof(req));

        auto* recv_hdr = reinterpret_cast<const rpc_header*>(frame_buf);
        auto msg = svc2::decode(recv_hdr->msg_type,
                                payload_ptr(recv_hdr),
                                payload_len(recv_hdr));

        bool ok = false;
        if (auto* p = std::get_if<svc2::sub_req>(&msg)) {
            ok = (p->a == 50 && p->b == 13);
        }

        delete[] frame_buf;

        if (ok) PASS();
        else FAIL("service_002 round trip failed");
    }
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "PUMP RPC Layer Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    test_rpc_header();
    test_error_types();
    test_pending_map();
    test_registry();
    test_service();
    test_dispatch();
    test_protocol_concept();
    test_header_roundtrip();
    test_e2e_encode_decode();

    std::cout << "\n========================================" << std::endl;
    std::cout << "Results: " << pass_count << "/" << test_count << " passed";
    if (fail_count > 0) {
        std::cout << ", " << fail_count << " FAILED";
    }
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    return fail_count > 0 ? 1 : 0;
}
