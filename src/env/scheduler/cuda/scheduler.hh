#ifndef ENV_SCHEDULER_CUDA_SCHEDULER_HH
#define ENV_SCHEDULER_CUDA_SCHEDULER_HH

#include <cstdint>
#include <bits/move_only_function.h>
#include <vector>
#include <stdexcept>

#include <cuda.h>

#include "pump/core/lock_free_queue.hh"
#include "pump/core/bind_back.hh"
#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"

namespace pump::scheduler::cuda {

    // GPU dimension (mirrors CUDA's dim3)
    struct gpu_dim3 {
        unsigned x = 1, y = 1, z = 1;
        gpu_dim3() = default;
        gpu_dim3(unsigned x_) : x(x_) {}
        gpu_dim3(unsigned x_, unsigned y_) : x(x_), y(y_) {}
        gpu_dim3(unsigned x_, unsigned y_, unsigned z_) : x(x_), y(y_), z(z_) {}
    };

    // CUDA error exception
    struct cuda_error : std::runtime_error {
        CUresult code;
        cuda_error(CUresult c, const char* err_str)
            : std::runtime_error(err_str ? err_str : "unknown CUDA error")
            , code(c) {}
    };

    inline void check_cu(CUresult result, const char* fallback = "CUDA error") {
        if (result != CUDA_SUCCESS) [[unlikely]] {
            const char* err_str = nullptr;
            cuGetErrorString(result, &err_str);
            throw cuda_error(result, err_str ? err_str : fallback);
        }
    }

    // ─── Device memory pool (size-class based) ───────────────────

    struct device_memory_pool {
        // Size classes: 256B, 1K, 4K, 16K, 64K, 256K, 1M, 4M, 16M, 64M
        static constexpr size_t NUM_CLASSES = 10;
        static constexpr size_t MAX_POOLED_SIZE = 64 * 1024 * 1024;  // 64M
        static constexpr size_t class_sizes[NUM_CLASSES] = {
            256, 1024, 4096, 16384, 65536, 262144,
            1048576, 4194304, 16777216, 67108864
        };

        std::vector<CUdeviceptr> free_lists[NUM_CLASSES];

        static uint32_t size_to_class(size_t bytes) {
            for (uint32_t i = 0; i < NUM_CLASSES; i++)
                if (bytes <= class_sizes[i]) return i;
            return UINT32_MAX;  // too large for pool
        }

        CUdeviceptr alloc(size_t bytes) {
            auto cls = size_to_class(bytes);
            if (cls != UINT32_MAX && !free_lists[cls].empty()) {
                auto ptr = free_lists[cls].back();
                free_lists[cls].pop_back();
                return ptr;
            }
            // Pool miss or oversized → real allocation
            CUdeviceptr ptr;
            size_t alloc_size = (cls != UINT32_MAX) ? class_sizes[cls] : bytes;
            check_cu(cuMemAlloc(&ptr, alloc_size));
            return ptr;
        }

        void free(CUdeviceptr ptr, size_t bytes) {
            auto cls = size_to_class(bytes);
            if (cls != UINT32_MAX) {
                free_lists[cls].push_back(ptr);
            } else {
                cuMemFree(ptr);  // oversized, return immediately
            }
        }

        void trim() {
            for (uint32_t i = 0; i < NUM_CLASSES; i++) {
                for (auto p : free_lists[i])
                    cuMemFree(p);
                free_lists[i].clear();
            }
        }

        ~device_memory_pool() { trim(); }
    };

    // ─── Pinned host memory pool ────────────────────────────────

    struct pinned_memory_pool {
        static constexpr size_t NUM_CLASSES = 10;
        static constexpr size_t MAX_POOLED_SIZE = 64 * 1024 * 1024;

        std::vector<void*> free_lists[NUM_CLASSES];

        void* alloc(size_t bytes) {
            auto cls = device_memory_pool::size_to_class(bytes);
            if (cls != UINT32_MAX && !free_lists[cls].empty()) {
                auto ptr = free_lists[cls].back();
                free_lists[cls].pop_back();
                return ptr;
            }
            void* ptr;
            size_t alloc_size = (cls != UINT32_MAX)
                ? device_memory_pool::class_sizes[cls] : bytes;
            check_cu(cuMemAllocHost(&ptr, alloc_size));
            return ptr;
        }

        void free(void* ptr, size_t bytes) {
            auto cls = device_memory_pool::size_to_class(bytes);
            if (cls != UINT32_MAX) {
                free_lists[cls].push_back(ptr);
            } else {
                cuMemFreeHost(ptr);
            }
        }

        void trim() {
            for (uint32_t i = 0; i < NUM_CLASSES; i++) {
                for (auto p : free_lists[i])
                    cuMemFreeHost(p);
                free_lists[i].clear();
            }
        }

        ~pinned_memory_pool() { trim(); }
    };

    // ─── RAII wrappers ──────────────────────────────────────────

    struct scoped_device_mem {
        CUdeviceptr ptr = 0;
        size_t      bytes = 0;
        device_memory_pool* pool;

        scoped_device_mem(device_memory_pool* p, size_t sz)
            : pool(p), bytes(sz) { ptr = pool->alloc(sz); }

        ~scoped_device_mem() { if (ptr) pool->free(ptr, bytes); }

        scoped_device_mem(scoped_device_mem&& o) noexcept
            : ptr(o.ptr), bytes(o.bytes), pool(o.pool) { o.ptr = 0; }
        scoped_device_mem& operator=(scoped_device_mem&& o) noexcept {
            if (this != &o) {
                if (ptr) pool->free(ptr, bytes);
                ptr = o.ptr; bytes = o.bytes; pool = o.pool; o.ptr = 0;
            }
            return *this;
        }
        scoped_device_mem(const scoped_device_mem&) = delete;
        scoped_device_mem& operator=(const scoped_device_mem&) = delete;

        operator CUdeviceptr() const { return ptr; }
        CUdeviceptr release() { auto p = ptr; ptr = 0; return p; }
    };

    struct scoped_pinned_mem {
        void*  ptr = nullptr;
        size_t bytes = 0;
        pinned_memory_pool* pool;

        scoped_pinned_mem(pinned_memory_pool* p, size_t sz)
            : pool(p), bytes(sz) { ptr = pool->alloc(sz); }

        ~scoped_pinned_mem() { if (ptr) pool->free(ptr, bytes); }

        scoped_pinned_mem(scoped_pinned_mem&& o) noexcept
            : ptr(o.ptr), bytes(o.bytes), pool(o.pool) { o.ptr = nullptr; }
        scoped_pinned_mem& operator=(scoped_pinned_mem&& o) noexcept {
            if (this != &o) {
                if (ptr) pool->free(ptr, bytes);
                ptr = o.ptr; bytes = o.bytes; pool = o.pool; o.ptr = nullptr;
            }
            return *this;
        }
        scoped_pinned_mem(const scoped_pinned_mem&) = delete;
        scoped_pinned_mem& operator=(const scoped_pinned_mem&) = delete;

        template<typename T = void>
        T* get() const { return static_cast<T*>(ptr); }
        void* release() { auto p = ptr; ptr = nullptr; return p; }
    };

    // ─── CUDA Graph handle (RAII) ────────────────────────────────

    struct graph_handle {
        CUgraphExec exec = nullptr;
        CUgraph     graph = nullptr;

        graph_handle() = default;
        graph_handle(CUgraphExec e, CUgraph g) : exec(e), graph(g) {}

        ~graph_handle() {
            if (exec)  cuGraphExecDestroy(exec);
            if (graph) cuGraphDestroy(graph);
        }

        graph_handle(graph_handle&& o) noexcept
            : exec(o.exec), graph(o.graph) { o.exec = nullptr; o.graph = nullptr; }
        graph_handle& operator=(graph_handle&& o) noexcept {
            if (this != &o) {
                if (exec)  cuGraphExecDestroy(exec);
                if (graph) cuGraphDestroy(graph);
                exec = o.exec; graph = o.graph;
                o.exec = nullptr; o.graph = nullptr;
            }
            return *this;
        }
        graph_handle(const graph_handle&) = delete;
        graph_handle& operator=(const graph_handle&) = delete;
    };

    // ─── GPU execution environment ──────────────────────────────

    struct gpu_env {
        CUstream             stream;
        CUdevice             device;
        int                  device_id;
        device_memory_pool*  dev_pool;
        pinned_memory_pool*  pin_pool;

        // Device memory (pool-backed)
        CUdeviceptr alloc(size_t bytes) {
            return dev_pool->alloc(bytes);
        }

        void free(CUdeviceptr ptr, size_t bytes) {
            dev_pool->free(ptr, bytes);
        }

        // RAII device memory
        scoped_device_mem scoped_alloc(size_t bytes) {
            return scoped_device_mem(dev_pool, bytes);
        }

        // Pinned host memory (pool-backed)
        void* alloc_pinned(size_t bytes) {
            return pin_pool->alloc(bytes);
        }

        void free_pinned(void* ptr, size_t bytes) {
            pin_pool->free(ptr, bytes);
        }

        // RAII pinned memory
        scoped_pinned_mem scoped_alloc_pinned(size_t bytes) {
            return scoped_pinned_mem(pin_pool, bytes);
        }

        // Async memory transfers
        void memcpy_h2d(CUdeviceptr dst, const void* src, size_t bytes) {
            check_cu(cuMemcpyHtoDAsync(dst, src, bytes, stream));
        }

        void memcpy_d2h(void* dst, CUdeviceptr src, size_t bytes) {
            check_cu(cuMemcpyDtoHAsync(dst, src, bytes, stream));
        }

        void memcpy_d2d(CUdeviceptr dst, CUdeviceptr src, size_t bytes) {
            check_cu(cuMemcpyDtoDAsync(dst, src, bytes, stream));
        }

        // Type-safe kernel launch
        template<typename... Args>
        void launch(CUfunction f, gpu_dim3 grid, gpu_dim3 block,
                    unsigned shared_mem, Args... args) {
            if constexpr (sizeof...(Args) == 0) {
                check_cu(cuLaunchKernel(f,
                    grid.x, grid.y, grid.z,
                    block.x, block.y, block.z,
                    shared_mem, stream, nullptr, nullptr));
            } else {
                void* params[] = { (void*)&args... };
                check_cu(cuLaunchKernel(f,
                    grid.x, grid.y, grid.z,
                    block.x, block.y, block.z,
                    shared_mem, stream, params, nullptr));
            }
        }
    };

    // ─── gpu::run sender ───────────────────────────────────────────

    namespace _gpu_run {
        struct req {
            std::move_only_function<void(gpu_env&)> work;
            std::move_only_function<void()> cb;
            std::move_only_function<void(std::exception_ptr)> err_cb;
        };

        template<typename scheduler_t, typename func_t>
        struct
        op {
            constexpr static bool gpu_run_op = true;
            scheduler_t* scheduler;
            func_t func;

            op(scheduler_t* s, func_t&& f)
                : scheduler(s), func(__fwd__(f)) {}

            op(op&& o) noexcept
                : scheduler(o.scheduler), func(__fwd__(o.func)) {}

            template<uint32_t pos, typename context_t, typename scope_t>
            auto
            start(context_t& context, scope_t& scope) {
                auto* r = new req {
                    [func = __mov__(func)](gpu_env& env) mutable {
                        func(env);
                    },
                    [context = context, scope = scope]() mutable {
                        core::op_pusher<pos + 1, scope_t>::push_value(context, scope);
                    },
                    [context = context, scope = scope](std::exception_ptr e) mutable {
                        core::op_pusher<pos + 1, scope_t>::push_exception(context, scope, e);
                    }
                };
                scheduler->schedule(r);
            }
        };

        template<typename prev_t, typename scheduler_t, typename func_t>
        struct
        __ncp__(sender) {
            using prev_type = prev_t;
            prev_t prev;
            scheduler_t* scheduler;
            func_t func;

            sender(prev_t&& p, scheduler_t* s, func_t&& f)
                : prev(__fwd__(p)), scheduler(s), func(__fwd__(f)) {}

            sender(sender&& o) noexcept
                : prev(__fwd__(o.prev)), scheduler(o.scheduler), func(__fwd__(o.func)) {}

            inline auto
            make_op() {
                return op<scheduler_t, func_t>{scheduler, __mov__(func)};
            }

            template<typename context_t>
            auto
            connect() {
                return prev.template connect<context_t>().push_back(make_op());
            }
        };

        struct
        fn {
            template<typename sender_t, typename scheduler_t, typename func_t>
            constexpr decltype(auto)
            operator()(sender_t&& sender, scheduler_t* sched, func_t&& func) const {
                return _gpu_run::sender<sender_t, scheduler_t, func_t>{
                    __fwd__(sender), sched, __fwd__(func)
                };
            }

            template<typename scheduler_t, typename func_t>
            constexpr decltype(auto)
            operator()(scheduler_t* sched, func_t&& func) const {
                return core::bind_back<fn, scheduler_t*, func_t>(fn{}, sched, __fwd__(func));
            }
        };
    }

    namespace gpu {
        inline constexpr _gpu_run::fn run{};

        // ─── Fine-grained senders (syntactic sugar over gpu::run) ───

        template<typename scheduler_t>
        auto memcpy_h2d(scheduler_t* sched, CUdeviceptr dst,
                        const void* src, size_t bytes) {
            return run(sched, [=](gpu_env& env) {
                env.memcpy_h2d(dst, src, bytes);
            });
        }

        template<typename scheduler_t>
        auto memcpy_d2h(scheduler_t* sched, void* dst,
                        CUdeviceptr src, size_t bytes) {
            return run(sched, [=](gpu_env& env) {
                env.memcpy_d2h(dst, src, bytes);
            });
        }

        template<typename scheduler_t>
        auto memcpy_d2d(scheduler_t* sched, CUdeviceptr dst,
                        CUdeviceptr src, size_t bytes) {
            return run(sched, [=](gpu_env& env) {
                env.memcpy_d2d(dst, src, bytes);
            });
        }

        template<typename scheduler_t, typename... Args>
        auto launch(scheduler_t* sched, CUfunction kernel,
                    gpu_dim3 grid, gpu_dim3 block,
                    unsigned shared_mem, Args... args) {
            return run(sched, [=](gpu_env& env) {
                env.launch(kernel, grid, block, shared_mem, args...);
            });
        }

        // Replay a captured CUDA graph (syntactic sugar over gpu::run)
        template<typename scheduler_t>
        auto replay(scheduler_t* sched, graph_handle* handle) {
            return run(sched, [handle](gpu_env& env) {
                check_cu(cuGraphLaunch(handle->exec, env.stream));
            });
        }
    }

    // ─── GPU Scheduler ─────────────────────────────────────────────

    struct scheduler {
        // CUDA resources
        CUdevice  device_;
        CUcontext cu_ctx_;
        CUstream  stream_;
        int       device_id_;
        bool      ctx_bound_ = false;

        // Memory pools (single-threaded, no locking)
        device_memory_pool dev_pool_;
        pinned_memory_pool pin_pool_;

        // Request queue (other cores enqueue here)
        core::per_core::queue<_gpu_run::req*, 1024> req_q_;

        // In-flight operations waiting for GPU completion
        struct pending_op {
            CUevent event;
            std::move_only_function<void()> cb;
        };
        std::vector<pending_op> pending_;

        // Event pool (reuse to avoid create/destroy overhead)
        std::vector<CUevent> event_pool_;

        void schedule(_gpu_run::req* req) {
            req_q_.try_enqueue(req);
        }

        CUevent acquire_event() {
            if (!event_pool_.empty()) {
                auto e = event_pool_.back();
                event_pool_.pop_back();
                return e;
            }
            CUevent e;
            check_cu(cuEventCreate(&e, CU_EVENT_DISABLE_TIMING));
            return e;
        }

        void release_event(CUevent e) {
            event_pool_.push_back(e);
        }

        void ensure_ctx_bound() {
            if (!ctx_bound_) {
                check_cu(cuCtxSetCurrent(cu_ctx_));
                ctx_bound_ = true;
            }
        }

    public:
        explicit scheduler(int device_id = 0) : device_id_(device_id) {
            check_cu(cuDeviceGet(&device_, device_id));
            check_cu(cuCtxCreate(&cu_ctx_, nullptr, 0, device_));
            check_cu(cuStreamCreate(&stream_, CU_STREAM_NON_BLOCKING));
        }

        ~scheduler() {
            for (auto& p : pending_)
                cuEventDestroy(p.event);
            for (auto e : event_pool_)
                cuEventDestroy(e);
            cuStreamDestroy(stream_);
            cuCtxDestroy(cu_ctx_);
        }

        CUcontext cu_context() const { return cu_ctx_; }
        CUdevice  cu_device()  const { return device_; }
        int       device_id()  const { return device_id_; }

        // Module/function loading helpers
        static CUmodule load_module(const char* path) {
            CUmodule module;
            check_cu(cuModuleLoad(&module, path));
            return module;
        }

        static CUmodule load_module_data(const void* data) {
            CUmodule module;
            check_cu(cuModuleLoadData(&module, data));
            return module;
        }

        static CUfunction get_function(CUmodule module, const char* name) {
            CUfunction func;
            check_cu(cuModuleGetFunction(&func, module, name));
            return func;
        }

        // Capture a sequence of CUDA ops into a graph for efficient replay.
        // Must be called from the scheduler's thread (typically during init,
        // before the advance loop starts).
        template<typename F>
        graph_handle capture(F&& func) {
            ensure_ctx_bound();

            check_cu(cuStreamBeginCapture(stream_, CU_STREAM_CAPTURE_MODE_GLOBAL));
            gpu_env env{stream_, device_, device_id_, &dev_pool_, &pin_pool_};
            func(env);

            CUgraph graph;
            check_cu(cuStreamEndCapture(stream_, &graph));

            CUgraphExec exec;
            check_cu(cuGraphInstantiate(&exec, graph, 0));

            return graph_handle{exec, graph};
        }

        bool advance() {
            ensure_ctx_bound();
            bool did_work = false;

            // 1. Drain new requests → submit CUDA work
            did_work |= req_q_.drain([this](_gpu_run::req* r) {
                gpu_env env{stream_, device_, device_id_, &dev_pool_, &pin_pool_};
                try {
                    r->work(env);
                    CUevent e = acquire_event();
                    cuEventRecord(e, stream_);
                    pending_.push_back({e, __mov__(r->cb)});
                } catch (...) {
                    r->err_cb(std::current_exception());
                }
                delete r;
            });

            // 2. Poll pending completions
            for (auto it = pending_.begin(); it != pending_.end(); ) {
                if (cuEventQuery(it->event) == CUDA_SUCCESS) {
                    it->cb();
                    release_event(it->event);
                    it = pending_.erase(it);
                    did_work = true;
                } else {
                    ++it;
                }
            }

            return did_work;
        }
    };
}

// ─── op_pusher specialization ──────────────────────────────────────

namespace pump::core {
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::gpu_run_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename context_t, typename ...value_t>
        static inline void
        push_value(context_t& context, scope_t& scope, value_t&& ...) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(context, scope);
        }
    };

    template<typename context_t, typename prev_t, typename scheduler_t, typename func_t>
    struct
    compute_sender_type<context_t, scheduler::cuda::_gpu_run::sender<prev_t, scheduler_t, func_t>> {
        consteval static uint32_t
        count_value() {
            return 0;
        }
    };
}

#endif // ENV_SCHEDULER_CUDA_SCHEDULER_HH
