//
//
//

#ifndef PUMP_CORE_CONTEXT_HH
#define PUMP_CORE_CONTEXT_HH

#include <any>

#include "./meta.hh"
#include "./scope.hh"

namespace pump::core {

    // ================================================================
    // context_ptr: trivially copyable raw pointer wrapper (no refcount)
    // ================================================================
    template<typename T>
    struct context_ptr {
        using element_type = T;
        T* ptr_ = nullptr;

        context_ptr() = default;
        explicit context_ptr(T* p) : ptr_(p) {}
        context_ptr(const context_ptr&) = default;
        context_ptr& operator=(const context_ptr&) = default;

        T* operator->() const { return ptr_; }
        T& operator*() const { return *ptr_; }
        T* get() const { return ptr_; }
        T* release() {
            auto* raw = ptr_;
            ptr_ = nullptr;
            return raw;
        }
    };

    template <typename ...content_t>
    struct
    __ncp__(root_context) {
        constexpr static bool root_flag = true;
        constexpr static uint32_t data_size = sizeof...(content_t);
        constexpr static bool pop_able = false;
        constexpr static bool find_base = false;

        template<typename T>
        constexpr static bool has_type = contains<T, content_t...>::value;

        std::tuple<content_t...> datas;

        root_context(root_context&& o)noexcept
            :datas(__fwd__(o.datas)){
        }

        explicit root_context(content_t&& ...t)
            : datas(__fwd__(t)...){
        }

        template<uint64_t check_id>
        constexpr static
        bool
        has_id() {
            return false;
        }
    };

    template <uint64_t id, typename base_t, typename ...content_t>
    struct
    __ncp__(pushed_context) {
        constexpr static uint64_t pushed_id = id;
        constexpr static uint32_t data_size = sizeof...(content_t);
        constexpr static bool pop_able = true;
        constexpr static bool find_base = true;
        using base_type = base_t;

        template<uint64_t check_id>
        constexpr static
        bool
        has_id() {
            if constexpr (check_id == pushed_id)
                return true;
            return base_t::element_type::template has_id<check_id>();
        }

        template<typename T>
        constexpr static bool has_type = contains<T, content_t...>::value;

        base_t base_context;
        std::tuple<content_t...> datas;

        explicit
        pushed_context(base_t& base, std::tuple<content_t...>&& t)
            : datas(__fwd__(t))
            , base_context(base){
        }

        static void* operator new(size_t) {
            constexpr auto sc = scope_size_class(sizeof(pushed_context));
            if constexpr (sc > 0)
                return scope_slab<sc>::alloc();
            else
                return ::operator new(sizeof(pushed_context));
        }

        static void operator delete(void* p) {
            constexpr auto sc = scope_size_class(sizeof(pushed_context));
            if constexpr (sc > 0)
                scope_slab<sc>::dealloc(p);
            else
                ::operator delete(p);
        }
    };

    template <typename need_t,size_t index, typename context_t>
    auto&
    _get(context_t& context){
        if constexpr (index < context_t::element_type::data_size) {
            if constexpr (std::is_same_v<need_t, std::tuple_element_t<index, __typ__(context->datas)>>)
                return std::get<index>(context->datas);
            else
                return _get<need_t, index + 1, context_t>(context);
        }
        else if constexpr (!context_t::element_type::find_base) {
            static_assert(context_t::element_type::find_base, "No data of this type was found in the context");
        }
        else {
            return _get<need_t, 0, __typ__(context->base_context)>(context->base_context);
        }
    }

    template <typename context_t, typename ...need_t>
    auto
    get_all_from_context(context_t& context){
        return std::forward_as_tuple(_get<need_t, 0, context_t>(context)...);
    }

    template <typename ...content_t>
    auto
    make_root_context(content_t&& ...c) {
        return
            std::make_shared<root_context<
                    std::remove_cvref_t<content_t>...>
            >(
                std::forward<remove_ref_if_pointer_t<content_t>>(c)...
            );
    }
}
#endif //PUMP_CORE_CONTEXT_HH
