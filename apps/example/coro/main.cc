
#include <iostream>

#include "pump/core/meta.hh"
#include "pump/core/context.hh"
#include "pump/coro/coro.hh"
#include "pump/sender/await_sender.hh"
#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/for_each.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/submit.hh"

// Bridge coroutine: p1 -> coro -> p2
// co_await p1 output, co_yield as stream for p2
auto
bridge_coro_1() -> pump::coro::return_yields<int&> {
    auto xx = std::vector<int>(10);
    for (int i = 0; i < 10; ++i) {
        xx[i] = co_await (
            pump::sender::just(__mov__(i))
                >> pump::sender::then([](int a) { return a * 2; })
                >> pump::sender::await_able(pump::core::make_root_context())
        );
        co_yield xx[i];
    }
    co_return xx[9];
}

auto
bridge_coro_2() -> pump::coro::return_yields<int> {
    auto xx = std::vector<int>(10);
    for (int i = 0; i < 10; ++i) {
        xx[i] = co_await (
            pump::sender::just(__mov__(i))
                >> pump::sender::then([](int a) { return a * 2; })
                >> pump::sender::await_able(pump::core::make_root_context())
        );
        co_yield xx[i];
    }
    co_return xx[9];
}

// Simple coroutine that yields a reference to an external variable
auto
ref_coro(int& external) -> pump::coro::return_yields<int&> {
    co_yield external;
    co_return external;
}

// Simple coroutine that yields a copy of an external variable
auto
val_coro(int& external) -> pump::coro::return_yields<int> {
    co_yield external;
    co_return external;
}

void test_reference() {
    std::cout << "=== Test: bridge_coro_1 yields int& (reference) ===" << std::endl;
    int x = 42;
    auto coro = ref_coro(x);

    // resume to first co_yield
    auto& promise = coro.resume();
    // take() returns int& — a reference to the original variable x
    int& ref = promise.take();

    std::cout << "  original x = " << x << ", take() = " << ref << std::endl;
    std::cout << "  &x = " << &x << ", &take() = " << &ref << std::endl;

    // Modify through the reference — should change the original
    ref = 100;
    std::cout << "  after ref = 100: x = " << x << std::endl;

    if (&ref == &x && x == 100) {
        std::cout << "  [PASS] take() returned a reference to the original variable" << std::endl;
    } else {
        std::cout << "  [FAIL] reference semantics not working" << std::endl;
    }
}

void test_value() {
    std::cout << "=== Test: bridge_coro_2 yields int (value) ===" << std::endl;
    int x = 42;
    auto coro = val_coro(x);

    // resume to first co_yield
    auto& promise = coro.resume();
    // take() returns int — a copy
    int val = promise.take();

    std::cout << "  original x = " << x << ", take() = " << val << std::endl;

    // Modify the copy — should NOT change the original
    val = 100;
    std::cout << "  after val = 100: x = " << x << std::endl;

    if (x == 42) {
        std::cout << "  [PASS] take() returned a value copy, original unchanged" << std::endl;
    } else {
        std::cout << "  [FAIL] value semantics not working" << std::endl;
    }
}

int main() {
    // ---- Test reference vs value semantics ----
    test_reference();
    std::cout << std::endl;
    test_value();
    std::cout << std::endl;

    // ---- Original pipeline demo ----
    // p1 -> bridge_coro -> p2
    // bridge_coro yields p1 results as a stream; for_each feeds them into p2
    auto context = pump::core::make_root_context();

    pump::sender::just()
        >> pump::sender::for_each(pump::coro::make_view_able(bridge_coro_1()))
        >> pump::sender::then([](int a) { return a + 100; })
        >> pump::sender::then([](int val) {
            std::cout << val << std::endl;
        })
        >> pump::sender::reduce()
        >> pump::sender::then([](bool) {})
        >> pump::sender::submit(context);

    std::cout << "done" << std::endl;
    return 0;
}
