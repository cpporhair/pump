
// RPC 层示例：在同一个程序中同时实现客户端和服务端
//
// 架构：
//   所有 scheduler 在同一线程中运行，通过手动 advance 循环驱动
//
// 流程：
//   1. 服务端在 0.0.0.0:9090 监听
//   2. 客户端连接 127.0.0.1:9090
//   3. 客户端通过 rpc::call 发送 add_req{3, 4}
//   4. 服务端收到请求，解码，调用 handler，编码响应发回
//   5. 客户端收到响应 add_resp{7}，打印结果

#include <print>
#include <cassert>
#include <thread>
#include <cstring>
#include <array>
#include <variant>
#include <atomic>

#include "pump/sender/flat.hh"
#include "pump/sender/sequential.hh"
#include "pump/sender/just.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/on.hh"
#include "pump/sender/repeat.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/visit.hh"
#include "pump/sender/when_all.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/pop_context.hh"
#include "env/scheduler/task/tasks_scheduler.hh"
#include "env/scheduler/net/net.hh"
#include "env/scheduler/net/io_uring/scheduler.hh"
#include "env/scheduler/net/io_uring/connect_scheduler.hh"

#include "env/scheduler/rpc/rpc.hh"

using namespace pump;
using namespace pump::sender;

using task_scheduler_t = scheduler::task::scheduler;

// ============================================================================
// Service 定义（plan.md §2.1）
// service_001: 加法服务
// ============================================================================

using service_type = scheduler::rpc::service::service_type;

template <>
struct scheduler::rpc::service::service<service_type::service_001> {
    struct add_req { int a; int b; };
    struct add_resp { int result; };
    using message_type = std::variant<add_req, add_resp>;

    // msg_type 编码：按 payload 大小区分（简化实现）
    // rpc::call 当前发送 msg_type=0，服务端按 payload 大小区分请求/响应
    static message_type decode(uint8_t msg_type, const char* payload, size_t len) {
        if (len == sizeof(add_req)) {
            add_req r;
            std::memcpy(&r, payload, sizeof(add_req));
            return r;
        }
        if (len == sizeof(add_resp)) {
            add_resp r;
            std::memcpy(&r, payload, sizeof(add_resp));
            return r;
        }
        throw scheduler::rpc::common::protocol_error("service_001: unknown payload size");
    }

    static uint8_t msg_type_of(const add_req&) { return 0x01; }
    static uint8_t msg_type_of(const add_resp&) { return 0x02; }

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
        return just(add_resp{req.a + req.b});
    }
};

using svc = scheduler::rpc::service::service<service_type::service_001>;
using rpc_header = scheduler::rpc::common::rpc_header;
using rpc_flags = scheduler::rpc::common::rpc_flags;

// ============================================================================
// Scheduler 类型定义
// ============================================================================

using accept_sched_t = scheduler::net::io_uring::accept_scheduler<
    scheduler::net::senders::conn::op>;
using connect_sched_t = scheduler::net::io_uring::connect_scheduler<
    scheduler::net::senders::conn::op>;
using session_sched_t = scheduler::net::io_uring::session_scheduler<
    scheduler::net::senders::join::op,
    scheduler::net::senders::recv::op,
    scheduler::net::senders::send::op,
    scheduler::net::senders::stop::op>;

// 示例完成标志
static std::atomic<bool> example_done{false};

// ============================================================================
// 服务端：接受连接 → join → recv → decode → handle → encode → send 响应
// ============================================================================

auto
server_session(session_sched_t* session_sched, scheduler::net::common::session_id_t sid) {
    return scheduler::net::join(session_sched, sid)
        >> flat_map([session_sched, sid](...) {
            std::println("[server] session {} joined, waiting for requests...", sid.raw());
            return scheduler::net::recv(session_sched, sid);
        })
        >> then([session_sched, sid](scheduler::net::common::recv_frame&& frame) {
            std::println("[server] received frame, size={}", frame.size());

            // 解析 RPC 帧头
            auto* hdr = frame.as<rpc_header>();
            auto rid = hdr->request_id;
            auto msg_type = hdr->msg_type;
            auto* payload = scheduler::rpc::common::payload_ptr(hdr);
            auto plen = scheduler::rpc::common::payload_len(hdr);

            std::println("[server] request_id={}, module_id={}, msg_type={}, flags={}",
                rid, hdr->module_id, msg_type, hdr->flags);

            // 解码请求
            auto decoded = svc::decode(msg_type, payload, plen);

            // 处理请求
            svc::add_resp resp{0};
            if (auto* req = std::get_if<svc::add_req>(&decoded)) {
                std::println("[server] handling add_req: {} + {}", req->a, req->b);
                resp.result = req->a + req->b;
            }
            std::println("[server] sending response: result={}", resp.result);

            // 编码响应帧：rpc_header + payload
            auto resp_size = sizeof(rpc_header) + sizeof(svc::add_resp);
            auto* resp_buf = new char[resp_size];

            auto* resp_hdr = reinterpret_cast<rpc_header*>(resp_buf);
            resp_hdr->total_len = static_cast<uint32_t>(resp_size);
            resp_hdr->request_id = rid;
            resp_hdr->module_id = static_cast<uint16_t>(service_type::service_001);
            resp_hdr->msg_type = svc::msg_type_of(resp);
            resp_hdr->flags = static_cast<uint8_t>(rpc_flags::response);
            std::memcpy(resp_buf + sizeof(rpc_header), &resp, sizeof(svc::add_resp));

            return std::make_pair(resp_buf, resp_size);
        })
        >> flat_map([session_sched, sid](std::pair<char*, size_t> resp_data) {
            auto* vec = new iovec{resp_data.first, resp_data.second};
            auto* resp_buf = resp_data.first;
            return scheduler::net::send(session_sched, sid, vec, 1)
                >> then([vec, resp_buf](bool) {
                    std::println("[server] response sent");
                    delete[] resp_buf;
                    delete vec;
                });
        })
        >> then([](...) {
            std::println("[server] request handled successfully");
        })
        >> any_exception([](std::exception_ptr e) {
            try { std::rethrow_exception(e); }
            catch (const std::exception& ex) {
                std::println(stderr, "[server] error: {}", ex.what());
            }
            return just();
        });
}

// ============================================================================
// 客户端：connect → join → rpc::call → 打印结果
// ============================================================================

auto
client_proc(connect_sched_t* connect_sched, session_sched_t* session_sched) {
    return scheduler::net::connect(connect_sched, "127.0.0.1", 9090)
        >> then([session_sched](scheduler::net::common::session_id_t sid) {
            std::println("[client] connected, session_id={}", sid.raw());
            return sid;
        })
        >> flat_map([session_sched](scheduler::net::common::session_id_t sid) {
            return scheduler::net::join(session_sched, sid)
                >> flat_map([session_sched, sid](...) {
                    std::println("[client] session joined, sending rpc::call add_req{{3, 4}}...");
                    return scheduler::rpc::call<service_type::service_001>(
                        session_sched, sid, svc::add_req{3, 4});
                })
                >> then([](auto&& msg) {
                    // msg 是 svc::message_type (variant<add_req, add_resp>)
                    std::visit([](auto&& m) {
                        using T = std::decay_t<decltype(m)>;
                        if constexpr (std::is_same_v<T, svc::add_resp>) {
                            std::println("[client] received response: 3 + 4 = {}", m.result);
                        } else {
                            std::println("[client] unexpected message type");
                        }
                    }, msg);
                    std::println("");
                    std::println("=== RPC Example Success ===");
                    example_done.store(true);
                });
        })
        >> any_exception([](std::exception_ptr e) {
            try { std::rethrow_exception(e); }
            catch (const std::exception& ex) {
                std::println(stderr, "[client] error: {}", ex.what());
            }
            example_done.store(true);
            return just();
        });
}

// ============================================================================
// Main
// ============================================================================

int
main() {
    std::println("=== PUMP RPC Example: Client + Server in one process ===");
    std::println("Server listens on 0.0.0.0:9090");
    std::println("Client connects to 127.0.0.1:9090");
    std::println("");

    // 初始化 scheduler
    auto* accept_sched = new accept_sched_t();
    if (accept_sched->init("0.0.0.0", 9090, 256) < 0) {
        std::println(stderr, "Failed to init accept_scheduler");
        return 1;
    }

    auto* connect_sched = new connect_sched_t();
    if (connect_sched->init(256) < 0) {
        std::println(stderr, "Failed to init connect_scheduler");
        return 1;
    }

    auto* session_sched = new session_sched_t();
    if (session_sched->init(256) < 0) {
        std::println(stderr, "Failed to init session_scheduler");
        return 1;
    }

    // 启动服务端：等待一个连接
    scheduler::net::wait_connection(accept_sched)
        >> then([session_sched](scheduler::net::common::session_id_t sid) {
            std::println("[server] accepted connection, session_id={}", sid.raw());
            server_session(session_sched, sid) >> submit(core::make_root_context());
        })
        >> submit(core::make_root_context());

    // 启动客户端：发起连接
    client_proc(connect_sched, session_sched)
        >> submit(core::make_root_context());

    // 手动事件循环：轮询所有 scheduler 直到示例完成
    while (!example_done.load()) {
        accept_sched->advance();
        connect_sched->advance();
        session_sched->advance();
    }

    std::println("");
    std::println("Example finished.");

    delete session_sched;
    delete connect_sched;
    delete accept_sched;

    return 0;
}
