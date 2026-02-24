对于消息响应函数的设计,应该是这样的
1. 服务注册和定义.
    ```c++
    // 由rpc层提供 应用层会增加服务类型 "service_00N"
    enum class service_type {
        service_001,
        service_002,
        service_003,
        max_service
    };
    
    // 由rpc层提供
    template<typename T, typename Req>
    concept has_handle_concept = requires(Req &&r) {
        T::handle(std::forward<Req>(r));
    };
    
    // 由rpc层提供
    template<service_type sid>
    struct service {
        static constexpr bool is_service = false;
    };
    
    // 由rpc层提供
    template <std::size_t... Is>
    consteval auto
    make_services_helper(std::index_sequence<Is...>) {
        return std::tuple<
            service<static_cast<service_type>(Is)>...
        >{};
    }
    
    // 应用层A自己实现service<service_type::service_001>
    template <>
    struct
    service<service_type::service_001> {
        struct add_req {
            int a;
            int b;
        };
        
        struct sleep_and_add_req {
            int a;
            int b;
            int sleep_ms;
        };
        
        static constexpr bool is_service = true;
        static auto
        handle(add_req&& req) {
            return just(req.a + req.b);
        }
    
        static auto
        handle(sleep_and_add_req&& req) {
            return just()
                >> then([req = __fwd__(req)]() {
                    return ... /*异步操作的pipeline*/;
                })
                >> flat();
        }
    };

    // 应用层B自己实现service<service_type::service_002>    
    template <>
    struct
    service<service_type::service_002> {
        struct sub_req {
            int a;
            int b;
        };
        
        static constexpr bool is_service = true;
    
        static auto
        handle(sub_req&& req) {
            return just(req.a - req.b);
        }
    };
    ```

2. 服务端的方式
    ```c++

    template<service_type ...service_ids>
    auto
    get_service_class_by_id(service_type sid) {
        using res_t = std::variant<service<service_ids>...>;
        std::optional<res_t> result;
        (void)((sid == service_ids && (result.emplace(service<service_ids>{}), true)) || ...);
        if (result) 
            return result.value();
        throw std::logic_error("unknown service type");
    }
    
    template<service_type ...service_ids>
    auto
    dispatch() {
        return flat_map([](uint16_t service_id, auto &&msg) {
            return just()
                >> visit(get_service_class_by_id<service_ids...>(static_cast<service_type>(service_id)))
                >> flat_map([req = __fwd__(msg)](auto &&result) mutable {
                    if constexpr (std::decay_t<decltype(result)>::is_service) {
                        if constexpr (has_handle_concept<std::decay_t<decltype(result)>, decltype(req)>)
                            return std::decay_t<decltype(result)>::handle(__mov__(req));
                        else
                            return just_exception(std::logic_error("unknown service type")) ;
                    } else {
                        return just_exception(std::logic_error("unknown service type"));
                    }
                });
        });
    }

    template <typename session_scheduler_t>
    auto
    check_session(const session_data<session_scheduler_t>& sd) -> coro::return_yields<bool> {
        while (!sd.closed.load())
            co_yield true;
        co_return false;
    }
    
    template<service_type ...service_ids>
    auto
    dispatch_proc(session_data& sd) {
       return for_each(coro::make_view_able(check_session(sd)))
        >> flat_map([&sd](auto&& is_running) {
            return scheduler::net::recv(sd.scheduler, sd.id);
        })
        >> decode_msg(...)
        >> dispatch()
        >> then([](auto&& response) {
            return scheduler::net::send(sd.scheduler, sd.id, ... /*response*/);
        })
        >> reduce();
    }

    // 服务端对每个 session 调用 rpc::serv 实现分发
    template<service_type ...service_ids>
    auto
    serv(session_scheduler_t* sc, scheduler::net::common::session_id_t id) {
        return scheduler::net::join(session_sched, sid)
            >> dispatch_proc<service_ids...>(session_data<session_scheduler_t>(sc,id));
    }
    
    ```
3. 客户端的方式

   ```c++
   // 客户端调用rpc::call,此时应该已经调用过join绑定了session和scheduler
   template<service_type st>
   auto
   call(session_scheduler_t* sc, scheduler::net::common::session_id_t id, add_req&& req) {
       return rpc::encode_msg<st>(req) 
           >> flat_map([sd](char* buf, size_t len) {
               return scheduler::net::send(sc, id, buf, len);
           }
           >> rpc::wait_response_at(sc, id);
   }
   
   // 客户端调用rpc::send,此时应该已经调用过join绑定了session和scheduler
   auto
   send(session_scheduler_t* sc, scheduler::net::common::session_id_t id, add_req&& req) {
       return rpc::encode_msg<st>(req) 
           >> flat_map([sd](char* buf, size_t len) {
               return scheduler::net::send(sc, id, buf, len);
           };
   }
   ```