//
//
//

#ifndef SIDER_BASE_HH
#define SIDER_BASE_HH

#include <memory>
#include <exception>
#include <coroutine>

namespace pump::coro {

    struct __co_flag__{};

    template <typename handle_type>
    concept is_corou_handle = requires (handle_type h){ h.promise().get_return_object(); };

    template <typename handle_type>
    concept is_yields_handle = requires (handle_type h){ typename std::remove_reference_t<decltype(h.promise())>::yield; };

    template <typename handle_type>
    concept is_return_handle = requires (handle_type h){ h.promise().get_return_object(); };

    template<typename T>
    concept is_co_object = std::is_base_of_v<__co_flag__,std::decay_t<T>>;

    template<typename T>
    concept is_co_yields = is_co_object<T> && is_yields_handle<typename T::handle_type>;

    template<typename T>
    concept is_co_return = is_co_object<T> && !is_co_yields<T>;

    template <typename value_type>
    struct promise_base {
    protected:
        std::exception_ptr          exception;

        consteval static auto
        compute_this_value_type() {
            if constexpr (std::is_lvalue_reference_v<value_type>)
                return std::type_identity<std::remove_reference_t<value_type>*>{};
            else
                return std::type_identity<std::unique_ptr<std::remove_reference_t<value_type>>>{};
        }

        typename decltype(compute_this_value_type())::type value;
    public:
        auto
        initial_suspend() const noexcept {
            return std::suspend_always{};
        }

        auto
        final_suspend() const noexcept {
            return std::suspend_always{};
        }

        void
        rethrow_if_exception() {
            if (exception) {
                std::rethrow_exception(exception);
            }
        }

        void
        unhandled_exception() noexcept {
            exception = std::current_exception();
        }

        value_type
        take() {
            if constexpr (std::is_lvalue_reference_v<value_type>)
                return *value;
            else
                return *value;
        }

    };
}

#endif //SIDER_BASE_HH
