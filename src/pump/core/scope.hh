#ifndef PUMP_CORE_SCOPE_HH
#define PUMP_CORE_SCOPE_HH

#include <cstdint>
#include <new>

#include "./meta.hh"

namespace pump::core {

    // ================================================================
    // scope_slab: thread_local free list pooling by size class
    //
    // 可配置项（在 include scope.hh 之前 #define）：
    //   PUMP_SCOPE_SLAB_ALIGN     — size class 对齐粒度（默认 64 字节）
    //   PUMP_SCOPE_SLAB_MAX_SIZE  — 池化的最大 scope 大小（默认 4096，超过走系统 allocator）
    //   PUMP_SCOPE_SLAB_MAX_FREE  — 每个 size class 最大缓存数（默认 0 = 不限）
    // ================================================================
#ifndef PUMP_SCOPE_SLAB_ALIGN
#define PUMP_SCOPE_SLAB_ALIGN 64
#endif

#ifndef PUMP_SCOPE_SLAB_MAX_SIZE
#define PUMP_SCOPE_SLAB_MAX_SIZE 4096
#endif

#ifndef PUMP_SCOPE_SLAB_MAX_FREE
#define PUMP_SCOPE_SLAB_MAX_FREE 0
#endif

    constexpr size_t scope_size_class(size_t n) {
        if (n > PUMP_SCOPE_SLAB_MAX_SIZE) return 0;
        constexpr size_t align = PUMP_SCOPE_SLAB_ALIGN;
        return ((n + align - 1) / align) * align;
    }

    struct scope_slab_stats {
        static inline thread_local uint64_t total_allocs = 0;
        static inline thread_local uint64_t pool_hits = 0;

        static void reset() {
            total_allocs = 0;
            pool_hits = 0;
        }
    };

    template<size_t SizeClass>
    struct scope_slab {
        struct node { node* next; };
        static inline thread_local node* head = nullptr;
        static inline thread_local uint32_t free_count = 0;

        static void* alloc() {
            scope_slab_stats::total_allocs++;
            if (head) {
                scope_slab_stats::pool_hits++;
                auto* p = head;
                head = p->next;
                free_count--;
                return p;
            }
            return ::operator new(SizeClass);
        }

        static void dealloc(void* p) {
            if constexpr (PUMP_SCOPE_SLAB_MAX_FREE > 0) {
                if (free_count >= PUMP_SCOPE_SLAB_MAX_FREE) {
                    ::operator delete(p);
                    return;
                }
            }
            auto* n = static_cast<node*>(p);
            n->next = head;
            head = n;
            free_count++;
        }
    };

    // ================================================================
    // scope_ptr: trivially copyable raw pointer wrapper (no refcount)
    // ================================================================
    template<typename T>
    struct scope_ptr {
        using element_type = T;
        T* ptr_ = nullptr;

        scope_ptr() = default;
        explicit scope_ptr(T* p) : ptr_(p) {}
        scope_ptr(const scope_ptr&) = default;
        scope_ptr& operator=(const scope_ptr&) = default;

        T* operator->() const { return ptr_; }
        T& operator*() const { return *ptr_; }
        T* get() const { return ptr_; }

        static constexpr uint32_t get_scope_level_id() {
            return T::get_scope_level_id();
        }
    };

    // ================================================================
    // scope_holder: type-erased ownership for await_sender / for_each
    // ================================================================
    struct scope_holder_base {
        virtual ~scope_holder_base() = default;
    };

    template<typename T>
    struct scope_holder_impl : scope_holder_base {
        T* ptr;
        explicit scope_holder_impl(T* p) : ptr(p) {}
        ~scope_holder_impl() override { delete ptr; }
    };

    enum struct
    runtime_scope_type {
        root,
        stream_starter,
        other
    };

    template <typename op_tuple_t>
    struct
    root_scope {
        constexpr static runtime_scope_type scope_type = runtime_scope_type::root;
        using op_tuple_type = op_tuple_t;
        op_tuple_t op_tuple;
        auto&
        get_op_tuple() {
            return op_tuple;
        }

        constexpr static
        uint32_t
        get_scope_level_id() {
            return 0;
        }

        auto
        get_scope_type() {
            return scope_type;
        }

        root_scope(op_tuple_t&& opt)
            : op_tuple(__fwd__(opt)) {
        }

        static void* operator new(size_t) {
            constexpr auto sc = scope_size_class(sizeof(root_scope));
            if constexpr (sc > 0)
                return scope_slab<sc>::alloc();
            else
                return ::operator new(sizeof(root_scope));
        }

        static void operator delete(void* p) {
            constexpr auto sc = scope_size_class(sizeof(root_scope));
            if constexpr (sc > 0)
                scope_slab<sc>::dealloc(p);
            else
                ::operator delete(p);
        }
    };

    template <runtime_scope_type type, typename op_tuple_t, typename base_t>
    struct
    runtime_scope {
        constexpr static runtime_scope_type scope_type = type;

        using op_tuple_type = op_tuple_t;

        op_tuple_t op_tuple;
        base_t base_scope;

        constexpr static
        uint32_t
        get_scope_level_id() {
            return base_t::get_scope_level_id() + 1;
        }

        auto&
        get_op_tuple() {
            return op_tuple;
        }

        auto
        get_scope_type() {
            return scope_type;
        }

        runtime_scope(op_tuple_t&& opt, base_t& base)
            : op_tuple(__fwd__(opt)) {
            base_scope = base;
        }

        ~runtime_scope() {
        }

        static void* operator new(size_t) {
            constexpr auto sc = scope_size_class(sizeof(runtime_scope));
            if constexpr (sc > 0)
                return scope_slab<sc>::alloc();
            else
                return ::operator new(sizeof(runtime_scope));
        }

        static void operator delete(void* p) {
            constexpr auto sc = scope_size_class(sizeof(runtime_scope));
            if constexpr (sc > 0)
                scope_slab<sc>::dealloc(p);
            else
                ::operator delete(p);
        }
    };

    template <runtime_scope_type scope_type,typename base_t, typename op_tuple_t>
    auto
    make_runtime_scope(base_t& scope, op_tuple_t&& opt) {
        using scope_t = runtime_scope<scope_type, op_tuple_t, std::decay_t<base_t>>;
        return scope_ptr<scope_t>(new scope_t(__fwd__(opt), scope));
    }

    template <uint32_t pos, uint32_t n, typename op_t, typename prev_t, typename T0, typename ...TS>
    auto
    change_tuple(op_t&& new_op, prev_t&& p, T0 &&t, TS&& ...ts) {
        if constexpr (pos == n) {
            return std::tuple_cat(__fwd__(p), std::forward_as_tuple(__fwd__(new_op)), std::forward_as_tuple(__fwd__(ts)...));
        } else {
            return change_tuple<pos + 1, n>(__fwd__(new_op), std::tuple_cat(__fwd__(p), std::forward_as_tuple(__fwd__(t))), __fwd__(ts)...);
        }
    }

    template <uint32_t pos, typename scope_t, typename new_op_t>
    auto*
    change_opt(scope_t& scope, new_op_t&& op) {
        return std::apply(
            [b = scope->base_scope, o = __fwd__(op)](auto&& ...args) mutable {
                return new runtime_scope<
                    scope_t::scope_type,
                    __typ__(b),
                    std::decay_t<decltype((change_tuple<0,pos,new_op_t,std::tuple<>, __typ__(args)...>(__fwd__(o), std::make_tuple(), __mov__(args)...)))>
                >(
                    b,
                    change_tuple<0,pos,new_op_t,std::tuple<>, __typ__(args)...>(__fwd__(o), std::make_tuple(), __mov__(args)...)
                );
            },
            __fwd__(scope->get_op_tuple())
        );
    }

    template <uint32_t pos, typename scope_t>
    struct
    get_current_op_type {
        using type = std::decay_t<std::tuple_element_t<pos, typename scope_t::element_type::op_tuple_type>>;
    };

    template <uint32_t pos, typename scope_t>
    requires std::is_same_v<
        std::decay_t<typename std::decay_t<std::tuple_element_t<pos, typename scope_t::element_type::op_tuple_type>>::op_type>,
        std::decay_t<typename std::decay_t<std::tuple_element_t<pos, typename scope_t::element_type::op_tuple_type>>::op_type>
    >
    struct
    get_current_op_type<pos, scope_t> {
        using type = std::decay_t<typename std::decay_t<std::tuple_element_t<pos, typename std::decay_t<scope_t>::element_type::op_tuple_type>>::op_type>;
    };
    template<uint32_t pos, typename scope_t>
    using get_current_op_type_t = get_current_op_type<pos, scope_t>::type;

    template <typename find_scope_t>
    inline
    auto&
    find_stream_starter(find_scope_t& scope){
        if constexpr (find_scope_t::element_type::scope_type == runtime_scope_type::stream_starter)
            return scope;
        else
            return find_stream_starter(scope->base_scope);
    }

    template<typename pop_scope_t>
    inline
    auto
    pop_to_loop_starter(pop_scope_t& scope) {
        if constexpr (pop_scope_t::element_type::scope_type == runtime_scope_type::root) {
            static_assert(pop_scope_t::element_type::scope_type != runtime_scope_type::root, "pop_to_loop_starter should not to root");
        }
        else if constexpr (pop_scope_t::element_type::scope_type == runtime_scope_type::stream_starter) {
            auto base = scope->base_scope;
            delete scope.get();
            return base;
        }
        else {
            auto& base = scope->base_scope;
            return pop_to_loop_starter(base);
        }
    }
}

#endif //PUMP_CORE_SCOPE_HH
