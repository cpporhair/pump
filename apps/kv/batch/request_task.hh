//

//

#ifndef APPS_KV_BATCH_REQUEST_READ_TASK_HH
#define APPS_KV_BATCH_REQUEST_READ_TASK_HH


namespace apps::kv::batch {
    struct
    request_get_id_task {
        std::move_only_function<void(data::snapshot*, uint64_t)> cb{};
    };

    struct
    request_put_id_task {
        std::move_only_function<void(data::snapshot*, uint64_t)> cb{};
    };
}

#endif //APPS_KV_BATCH_REQUEST_READ_TASK_HH
