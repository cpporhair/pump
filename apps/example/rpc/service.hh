//
// Created by null on 2026/2/27.
//

#ifndef APPS_EXAMPLE_RPC_SERVICE_HH
#define APPS_EXAMPLE_RPC_SERVICE_HH

#include <print>
#include <cstdio>
#include <cassert>
#include <thread>

#include "env/runtime/runner.hh"
#include "env/scheduler/net/rpc/common/struct.hh"
#include "env/scheduler/task/tasks_scheduler.hh"

namespace apps::rpc::service {
    enum class type : uint16_t {
        add,
        sub,
    };

    template <type sid>
    struct
    compute_service {};

    template <>
    struct
    compute_service<type::sub> {
        static int compute(const int& a, const int& b) {return a - b;}
    };

    template <>
    struct
    compute_service<type::add> {
        static int compute(const int& a, const int& b) {return a + b;}
    };
}

namespace pump::scheduler::rpc {
    template <apps::rpc::service::type sid>
    struct
    service<sid> : apps::rpc::service::compute_service<sid> {
        constexpr static bool is_service = true;
        struct __attribute__((packed)) req_struct { int a, b; };
        struct __attribute__((packed)) res_struct { int v;  };

        static auto
        handle(rpc_frame_helper& rpc_req, rpc_frame_helper& rpc_res) {
            auto req = reinterpret_cast<req_struct *>(rpc_req.get_payload());
            rpc_res.realloc_frame(sizeof(res_struct));
            auto res = reinterpret_cast<res_struct *>(rpc_res.get_payload());
            res->v = service::compute(req->a , req->b);
            return pump::sender::just();
        }

        static auto
        req_to_pkt(rpc_frame_helper& rpc_req, int a, int b) {
            rpc_req.realloc_frame(sizeof(req_struct));
            reinterpret_cast<req_struct *>(rpc_req.get_payload())->a = a;
            reinterpret_cast<req_struct *>(rpc_req.get_payload())->b = b;
        }

        static auto
        pkt_to_res(rpc_frame_helper& rpc_req) {
            auto pkt = reinterpret_cast<res_struct *>(rpc_req.get_payload());
            return res_struct{pkt->v};
        }
    };
}

#endif //APPS_EXAMPLE_RPC_SERVICE_HH