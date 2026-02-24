
#ifndef ENV_SCHEDULER_RPC_RPC_HH
#define ENV_SCHEDULER_RPC_RPC_HH

#include "pump/sender/flat.hh"

#include "./common/struct.hh"
#include "./common/protocol.hh"
#include "./state/pending_map.hh"
#include "./state/registry.hh"
#include "./service/service_type.hh"
#include "./service/service.hh"
#include "./service/dispatch.hh"
#include "./senders/send.hh"
#include "./senders/recv.hh"
#include "./senders/call.hh"
#include "./senders/serve.hh"

namespace pump::scheduler::rpc {

    // --- rpc::call --- （F-API-1）
    // 发送请求并异步等待对应的响应
    template <service::service_type st, typename scheduler_t, typename req_t>
    inline auto
    call(scheduler_t* sche, net::common::session_id_t sid, req_t&& req) {
        return senders::call::sender<st, scheduler_t, std::decay_t<req_t>>(
            sche, sid, __fwd__(req));
    }

    template <service::service_type st, typename req_t>
    inline auto
    call(net::common::session_id_t sid, req_t&& req) {
        return pump::sender::flat_map(
            [sid, req = __fwd__(req)]<typename scheduler_t>(scheduler_t* sche) mutable {
                return call<st>(sche, sid, __mov__(req));
            });
    }

    // --- rpc::send --- （F-API-2）
    // 单向发送，不等待响应
    template <service::service_type st, typename scheduler_t, typename msg_t>
    inline auto
    send(scheduler_t* sche, net::common::session_id_t sid, msg_t&& msg) {
        return senders::send::sender<st, scheduler_t, std::decay_t<msg_t>>(
            sche, sid, __fwd__(msg));
    }

    template <service::service_type st, typename msg_t>
    inline auto
    send(net::common::session_id_t sid, msg_t&& msg) {
        return pump::sender::flat_map(
            [sid, msg = __fwd__(msg)]<typename scheduler_t>(scheduler_t* sche) mutable {
                return send<st>(sche, sid, __mov__(msg));
            });
    }

    // --- rpc::recv --- （F-API-3）
    // 接收并解码下一条完整消息
    template <service::service_type st, typename scheduler_t>
    inline auto
    recv(scheduler_t* sche, net::common::session_id_t sid) {
        return senders::recv::sender<st, scheduler_t>(sche, sid);
    }

    template <service::service_type st>
    inline auto
    recv(net::common::session_id_t sid) {
        return pump::sender::flat_map(
            [sid]<typename scheduler_t>(scheduler_t* sche) {
                return recv<st>(sche, sid);
            });
    }

    // --- rpc::serve --- （F-API-4）
    // 服务端请求处理循环
    template <service::service_type ...sids, typename scheduler_t>
    inline auto
    serve(scheduler_t* sche, net::common::session_id_t sid) {
        return senders::serve::sender<scheduler_t, sids...>(sche, sid);
    }

    template <service::service_type ...sids>
    inline auto
    serve(net::common::session_id_t sid) {
        return pump::sender::flat_map(
            [sid]<typename scheduler_t>(scheduler_t* sche) {
                return serve<sids...>(sche, sid);
            });
    }

}

#endif //ENV_SCHEDULER_RPC_RPC_HH
