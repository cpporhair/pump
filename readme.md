# Pump: High-Performance Composable Asynchronous Pipeline Library

Pump is a C++ asynchronous library specifically designed for high-performance scenarios, built upon **Sender/Operator** semantics. It enables developers to compose complex asynchronous logic declaratively and switch between different execution domains (e.g., compute threads, NVMe IO, network event loops) via **Schedulers**.

## 1. Technical Architecture Overview

The architecture of Pump is divided into four core layers:

### 1.1 Orchestration Layer (Sender & Pipeline)
*   **Sender**: The fundamental building block of Pump, describing a deferred operation. It is declarative and performs no action until it is connected and started.
*   **Pipeline (`>>`)**: By overloading the `>>` operator, multiple Senders are chained into a linear pipeline. This avoids nested callback structures and presents business logic in a linear topology.

### 1.2 Execution Engine (Op & OpPusher)
The core implementation characteristics of Pump:
*   **Op (Operator)**: When a Sender is connected, a set of Ops is generated based on the context. Pump maps the Ops of the entire pipeline into a flattened `std::tuple`.
*   **OpPusher**: A lightweight execution pusher that manages the execution flow using compile-time position indices (Position).
    *   **Position Advancing**: `push_value(pos)` triggers the operation at `pos + 1`, reducing runtime virtual function dispatch and recursion depth.
    *   **Control Flow**: Supports jumping and rollback, enabling efficient implementation of `repeat`, `retry`, or exception branching.

### 1.3 Resources and Context (Scope & Context)
*   **Scope**: Defines the lifecycle boundary of asynchronous operations, managing the memory and hierarchy of the Op list.
*   **Context**: Utilizes a stack-based management mechanism. It supports direct access to high-level defined objects from deep within the pipeline via `push_context<T>` and `get_context<T>`, eliminating the need for explicit parameter passing in every function signature.

### 1.4 Scheduling Layer (Scheduler)
Pump emphasizes isolation and switching between execution domains:
*   **Task Scheduler**: General-purpose task scheduling and timers.
*   **NVMe Scheduler**: Integrated with SPDK, providing zero-copy disk IO scheduling.
*   **Net Scheduler**: Supports network event loops using `io_uring` and `epoll`.
*   **Runtime**: `env/runtime/share_nothing.hh` provides a multi-core execution model (starting scheduler combinations per core).

---

## 2. Core Semantic Layer

The project provides a rich set of Sender operators covering various aspects of flow control:

*   **Entry Points**: `just(...)`, `submit(context)`
*   **Transformation/Expansion**: `then`, `transform`, `flat/flat_map`
*   **Concurrency/Sequencing**: `concurrent(max)`, `sequential()`, `with_concurrent(...)`
*   **Streaming**: `for_each`, `generate`, `repeat`
*   **Aggregation**: `when_all`, `reduce`, `to_container`
*   **Exceptions and Skipping**: `catch_exception`, `any_exception`, `when_skipped`, `visit`
*   **Context Operations**: `push_context`, `pop_context`, `get_context`
*   **Coroutine Bridge**: `await_able()` or `await_able(context)` for `co_await` on senders

---

## 3. Comparison with Traditional Asynchronous Methods

### 3.1 Limitations of Traditional Approaches
1.  **Callbacks**: Lead to "Callback Hell," where logic is torn across multiple functions. Error handling is complex, and managing concurrency counts across steps is difficult.
2.  **Future/Promise**: Typically single-value centric, with composition limited to basic operations like `then/when_all`. In high-frequency scenarios, `std::future` may incur overhead from heap allocation and atomic reference counting.
3.  **Bare Coroutines**: While offering better readability, execution domain switching and concurrency control still require developers to manually wrap scheduling logic.

### 3.2 Improvements in Pump
1.  **Linear Declarative Flow**: Maintains logical continuity through Pipelines, making it easier to read and maintain.
2.  **Explicit Scheduling Switches**: Uses `on(...)` to clearly define execution contexts, reducing implicit jumps.
3.  **Resource Overhead Optimization**: The Op chain size is determined statically at connection time, resulting in almost zero runtime memory allocation.
4.  **Built-in Concurrency Control**: Native support for the `for_each + concurrent + reduce` pattern to express "fan-out/fan-in."

### 3.3 Code Comparison: KV Store Write Process
In a high-performance KV store, a write request involves: allocating an ID -> updating the index -> allocating disk space -> concurrent NVMe writes -> rollback on failure.

**Pump Approach (Flattened logic, composable failure paths):**
```cpp
auto apply_request = 
    get_context<batch*>()
    >> then(allocate_id)
    >> then(update_index_concurrently) // Internally uses for_each + concurrent
    >> then(allocate_disk_pages)
    >> then([](auto res) {
        return just()
            >> write_nvme_data(res->spans)
            >> write_metadata(res)
            // Automatic concurrent rollback on failure
            >> catch_exception<write_failed>([res](...) {
                return free_disk_spans(res->spans); 
            });
    })
    >> flat();
```

**Traditional Callback Approach:**
Requires maintaining multiple counters, handling nested error branches, and highly fragmented state passing.

---

## 4. Comparison with stdexec (P2300)

`stdexec` is the cornerstone of the C++26 asynchronous standard. Pump aligns with its design philosophy but differs in implementation strategy and application focus.

| Feature | stdexec (P2300) | Pump |
| :--- | :--- | :--- |
| **Execution Model** | Recursive Receiver Model | Flattened OpTuple + Position Advancing |
| **Memory Layout** | Nested State Machine Structure | Flattened Tuple Structure |
| **Context Passing** | Weak; usually requires explicit passing via parameters | Strong; supports typed Context stacks |
| **Compilation Performance** | Extremely heavy template recursion | Optimized template expansion; lower compilation overhead |
| **Hardware Integration** | General abstraction | Deep integration with high-performance components (NVMe/SPDK, io_uring) |
| **Type Dispatch** | Traditional static matching (requires `variant` or `any_sender`) | **visit** operator (promotes runtime values to compile-time types for `if constexpr` branching) |
| **Signaling Mechanism** | `set_value/error/stopped` | `push_value/exception/skip/done` |

**Summary of Differences:**
*   `stdexec` aims for **maximum generality**, serving as a universal asynchronous foundation for the C++ standard.
*   `Pump` focuses on **engineering performance and development efficiency in specific scenarios (storage/networking)**. Its `OpPusher` mechanism is particularly suited for long pipelines, while the `Context` mechanism significantly simplifies parameter passing in complex business logic.

---

## 5. Project Structure

```
src/pump/              # Orchestration Layer Core: sender, op_pusher, context, coro
src/env/               # Scheduling Layer & Runtime: task/nvme/net scheduler, runtime
apps/example/          # Examples: hello_world, echo, nvme, share_nothing
apps/kv/               # Application Example: High-performance KV store built with Pump
3rd/                   # Third-party dependencies
```

## 6. Build and Dependencies

### Key Dependencies
*   **C++26 Compiler** (GCC 13+ or Clang 17+)
*   **liburing** (For network io_uring scheduling)
*   **SPDK + DPDK** (For NVMe scheduling)
*   **numa, pthread, ssl, crypto** (For related applications)

### Build Steps
```bash
cmake -S . -B build
cmake --build build
```

---
> **Note**: This project currently contains some operators that are under development or should be used with caution (see `src/pump/sender`). It is recommended to refer to `apps/example` for standard usage.
