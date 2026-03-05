#ifndef PUMP_CORE_LOCK_FREE_QUEUE_HH
#define PUMP_CORE_LOCK_FREE_QUEUE_HH

#pragma once
#include <atomic>
#include <cstddef>
#include <optional>
#include <array>
#include <type_traits>
#include <utility>
#include <memory>

namespace pump::core {

    // Adaptive cell alignment: cache-line aligned for large types to avoid false sharing.
    template <typename T>
    constexpr size_t adaptive_cell_align = (sizeof(T) + sizeof(std::atomic<uint32_t>) > 64) ? 64 : 8;

    // Adaptive storage: small objects (<= 64B) stored inline, large objects via unique_ptr.
    template <typename T, typename = void>
    struct adaptive_storage {
        using type = T;
        static T wrap(T&& v) { return std::move(v); }
        static T& unwrap(T& v) { return v; }
        static const T& unwrap(const T& v) { return v; }
    };

    template <typename T>
    struct adaptive_storage<T, std::enable_if_t<(sizeof(T) > 64)>> {
        using type = std::unique_ptr<T>;
        static std::unique_ptr<T> wrap(T&& v) { return std::make_unique<T>(std::move(v)); }
        static T& unwrap(std::unique_ptr<T>& v) { return *v; }
        static const T& unwrap(const std::unique_ptr<T>& v) { return *v; }
    };

    template <typename T>
    using adaptive_storage_t = typename adaptive_storage<T>::type;

    namespace mpmc {
        template <typename T>
        struct alignas(adaptive_cell_align<T>) cell {
            std::atomic<uint32_t> sequence;
            T value;
            cell() : sequence(0), value{} {}
        };

        template<typename T, size_t CAPACITY = 1024>
        struct queue {
        private:
            static_assert((CAPACITY & (CAPACITY - 1)) == 0, "CAPACITY must be power of 2");
            static_assert(std::is_nothrow_move_constructible_v<T> || std::is_nothrow_move_assignable_v<T>,
                          "T must be nothrow movable for lock-free queue safety.");
            static constexpr uint32_t MASK = CAPACITY - 1;
            
            alignas(64) std::atomic<uint32_t> tail_;
            alignas(64) std::atomic<uint32_t> head_;
            std::array<cell<T>, CAPACITY> buffer_;

        public:
            queue() : tail_(0), head_(0) {
                for (uint32_t i = 0; i < CAPACITY; ++i)
                    buffer_[i].sequence.store(i, std::memory_order_relaxed);
            }
            ~queue() = default;

            queue(const queue&) = delete;
            queue& operator=(const queue&) = delete;

            bool try_enqueue(T&& value) noexcept {
                uint32_t pos = tail_.load(std::memory_order_relaxed);
                while (true) {
                    cell<T>& c = buffer_[pos & MASK];
                    uint32_t seq = c.sequence.load(std::memory_order_acquire);
                    int32_t dif = (int32_t)seq - (int32_t)pos;
                    if (dif == 0) {
                        if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                            c.value = std::move(value);
                            c.sequence.store(pos + 1, std::memory_order_release);
                            return true;
                        }
                    } else if (dif < 0) {
                        return false;
                    } else {
                        pos = tail_.load(std::memory_order_relaxed);
                    }
                }
            }

            bool try_enqueue(const T& value) noexcept {
                return try_enqueue(T(value));
            }

            std::optional<T> try_dequeue() noexcept {
                T result;
                if (try_dequeue(result)) {
                    return std::move(result);
                }
                return std::nullopt;
            }

            bool try_dequeue(T& result) noexcept {
                uint32_t pos = head_.load(std::memory_order_relaxed);
                while (true) {
                    cell<T>& c = buffer_[pos & MASK];
                    uint32_t seq = c.sequence.load(std::memory_order_acquire);
                    int32_t dif = (int32_t)seq - (int32_t)(pos + 1);
                    if (dif == 0) {
                        if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                            result = std::move(c.value);
                            c.sequence.store(pos + CAPACITY, std::memory_order_release);
                            return true;
                        }
                    } else if (dif < 0) {
                        return false;
                    } else {
                        pos = head_.load(std::memory_order_relaxed);
                    }
                }
            }

            bool empty() const noexcept {
                return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed);
            }

            size_t size_approx() const noexcept {
                return static_cast<size_t>(tail_.load(std::memory_order_relaxed) - head_.load(std::memory_order_relaxed));
            }
        };
    }

    namespace mpsc {
        template<typename T, size_t CAPACITY = 1024>
        struct queue {
        private:
            static_assert((CAPACITY & (CAPACITY - 1)) == 0, "CAPACITY must be power of 2");
            static_assert(std::is_nothrow_move_constructible_v<T> || std::is_nothrow_move_assignable_v<T>,
                          "T must be nothrow movable for lock-free queue safety.");
            static constexpr uint32_t MASK = CAPACITY - 1;

            alignas(64) std::atomic<uint32_t> tail_;
            alignas(64) std::atomic<uint32_t> head_;
            std::array<mpmc::cell<T>, CAPACITY> buffer_;

        public:
            queue() : tail_(0), head_(0) {
                for (uint32_t i = 0; i < CAPACITY; ++i)
                    buffer_[i].sequence.store(i, std::memory_order_relaxed);
            }

            ~queue() = default;

            queue(const queue&) = delete;
            queue& operator=(const queue&) = delete;

            bool try_enqueue(T&& value) noexcept {
                uint32_t pos = tail_.load(std::memory_order_relaxed);
                while (true) {
                    mpmc::cell<T>& cell = buffer_[pos & MASK];
                    uint32_t seq = cell.sequence.load(std::memory_order_acquire);
                    int32_t dif = (int32_t)seq - (int32_t)pos;
                    if (dif == 0) {
                        if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                            cell.value = std::move(value);
                            cell.sequence.store(pos + 1, std::memory_order_release);
                            return true;
                        }
                    } else if (dif < 0) {
                        return false;
                    } else {
                        pos = tail_.load(std::memory_order_relaxed);
                    }
                }
            }

            bool try_enqueue(const T& value) noexcept {
                return try_enqueue(T(value));
            }

            std::optional<T> try_dequeue() noexcept {
                T result;
                if (try_dequeue(result)) {
                    return std::move(result);
                }
                return std::nullopt;
            }

            bool try_dequeue(T& result) noexcept {
                uint32_t pos = head_.load(std::memory_order_relaxed);
                mpmc::cell<T>& cell = buffer_[pos & MASK];
                uint32_t seq = cell.sequence.load(std::memory_order_acquire);
                int32_t dif = (int32_t)seq - (int32_t)(pos + 1);
                if (dif == 0) {
                    head_.store(pos + 1, std::memory_order_relaxed);
                    result = std::move(cell.value);
                    cell.sequence.store(pos + CAPACITY, std::memory_order_release);
                    return true;
                }
                return false;
            }

            bool empty() const noexcept {
                return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed);
            }

            size_t size_approx() const noexcept {
                return static_cast<size_t>(tail_.load(std::memory_order_relaxed) - head_.load(std::memory_order_relaxed));
            }
        };
    }

    // Single-thread ring buffer: no atomics, no memory barriers.
    // Use when producer and consumer are guaranteed to be the same thread.
    namespace local {
        template<typename T, size_t CAPACITY = 1024>
        struct queue {
        private:
            static_assert((CAPACITY & (CAPACITY - 1)) == 0, "CAPACITY must be power of 2");
            static_assert(std::is_nothrow_move_constructible_v<T> || std::is_nothrow_move_assignable_v<T>,
                          "T must be nothrow movable for queue safety.");
            static constexpr size_t MASK = CAPACITY - 1;

            size_t head_ = 0;
            size_t tail_ = 0;
            T buffer_[CAPACITY];

        public:
            queue() {
                for (size_t i = 0; i < CAPACITY; ++i) buffer_[i] = T{};
            }
            ~queue() = default;
            queue(const queue&) = delete;
            queue& operator=(const queue&) = delete;

            bool try_enqueue(T&& value) noexcept {
                size_t next = (tail_ + 1) & MASK;
                if (next == head_) return false;
                buffer_[tail_] = std::move(value);
                tail_ = next;
                return true;
            }

            bool try_enqueue(const T& value) noexcept {
                return try_enqueue(T(value));
            }

            std::optional<T> try_dequeue() noexcept {
                T result;
                if (try_dequeue(result)) {
                    return std::move(result);
                }
                return std::nullopt;
            }

            bool try_dequeue(T& result) noexcept {
                if (head_ == tail_) return false;
                result = std::move(buffer_[head_]);
                head_ = (head_ + 1) & MASK;
                return true;
            }

            bool empty() const noexcept {
                return head_ == tail_;
            }

            bool full() const noexcept {
                return ((tail_ + 1) & MASK) == head_;
            }

            size_t size() const noexcept {
                return (tail_ + CAPACITY - head_) & MASK;
            }
        };
    }

    namespace spsc {
        template<typename T, size_t CAPACITY = 1024>
        struct queue {
        private:
            static_assert((CAPACITY & (CAPACITY - 1)) == 0, "CAPACITY must be power of 2");
            static_assert(std::is_nothrow_move_constructible_v<T> || std::is_nothrow_move_assignable_v<T>,
                          "T must be nothrow movable for lock-free queue safety.");
            static constexpr size_t MASK = CAPACITY - 1;

            alignas(64) std::atomic<size_t> tail_;
            alignas(64) size_t tail_cache_ = 0;
            alignas(64) std::atomic<size_t> head_;
            alignas(64) size_t head_cache_ = 0;
            alignas(64) T buffer_[CAPACITY];

        public:
            queue() : tail_(0), head_(0) {
                for (size_t i = 0; i < CAPACITY; ++i) buffer_[i] = T{};
            }
            ~queue() = default;
            queue(const queue&) = delete;
            queue& operator=(const queue&) = delete;

            bool try_enqueue(T&& value) noexcept {
                size_t tail = tail_.load(std::memory_order_relaxed);
                size_t next = (tail + 1) & MASK;
                if (next == head_cache_) {
                    head_cache_ = head_.load(std::memory_order_acquire);
                    if (next == head_cache_) return false;
                }
                buffer_[tail] = std::move(value);
                tail_.store(next, std::memory_order_release);
                return true;
            }

            bool try_enqueue(const T& value) noexcept {
                return try_enqueue(T(value));
            }

            std::optional<T> try_dequeue() noexcept {
                T result;
                if (try_dequeue(result)) {
                    return std::move(result);
                }
                return std::nullopt;
            }

            bool try_dequeue(T& result) noexcept {
                size_t head = head_.load(std::memory_order_relaxed);
                if (head == tail_cache_) {
                    tail_cache_ = tail_.load(std::memory_order_acquire);
                    if (head == tail_cache_) return false;
                }
                result = std::move(buffer_[head]);
                head_.store((head + 1) & MASK, std::memory_order_release);
                return true;
            }

            bool empty() const noexcept {
                return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed);
            }

            bool full() const noexcept {
                return ((tail_.load(std::memory_order_relaxed) + 1) & MASK) == head_.load(std::memory_order_relaxed);
            }

            size_t size_approx() const noexcept {
                size_t h = head_.load(std::memory_order_relaxed);
                size_t t = tail_.load(std::memory_order_relaxed);
                return (t + CAPACITY - h) & MASK;
            }
        };
    }

    // Per-core SPSC queue group with active bitmap.
    // Each source core gets a dedicated SPSC queue; bitmap tracks which cores have pending data.
    inline constexpr uint32_t MAX_CORES = 128;
    inline constexpr uint32_t BITMAP_WORDS = (MAX_CORES + 63) / 64;

    inline thread_local uint32_t this_core_id = 0;

    namespace per_core {
        template<typename T, size_t CAPACITY = 1024>
        struct queue {
        private:
            static_assert((CAPACITY & (CAPACITY - 1)) == 0, "CAPACITY must be power of 2");

            std::array<spsc::queue<T, CAPACITY>, MAX_CORES> queues_;

            struct alignas(64) bitmap_word { std::atomic<uint64_t> mask{0}; };
            std::array<bitmap_word, BITMAP_WORDS> active_mask_{};

        public:
            queue() = default;
            ~queue() = default;
            queue(const queue&) = delete;
            queue& operator=(const queue&) = delete;

            bool try_enqueue(T&& value) noexcept {
                if (queues_[this_core_id].try_enqueue(std::move(value))) {
                    uint32_t word = this_core_id / 64;
                    uint64_t bit  = 1ULL << (this_core_id % 64);
                    active_mask_[word].mask.fetch_or(bit, std::memory_order_release);
                    return true;
                }
                return false;
            }

            bool try_enqueue(const T& value) noexcept {
                return try_enqueue(T(value));
            }

            template<typename F>
            bool drain(F&& handler) {
                bool worked = false;
                for (uint32_t w = 0; w < BITMAP_WORDS; ++w) {
                    uint64_t mask = active_mask_[w].mask.exchange(0, std::memory_order_acquire);
                    while (mask) {
                        uint32_t bit = __builtin_ctzll(mask);
                        mask &= mask - 1;
                        uint32_t core = w * 64 + bit;
                        T item;
                        while (queues_[core].try_dequeue(item)) {
                            handler(std::move(item));
                            worked = true;
                        }
                    }
                }
                return worked;
            }

            std::optional<T> try_dequeue() noexcept {
                for (uint32_t w = 0; w < BITMAP_WORDS; ++w) {
                    uint64_t mask = active_mask_[w].mask.load(std::memory_order_acquire);
                    while (mask) {
                        uint32_t bit = __builtin_ctzll(mask);
                        mask &= mask - 1;
                        uint32_t core = w * 64 + bit;
                        T item;
                        if (queues_[core].try_dequeue(item))
                            return std::move(item);
                    }
                }
                return std::nullopt;
            }

            bool empty() const noexcept {
                for (uint32_t w = 0; w < BITMAP_WORDS; ++w) {
                    if (active_mask_[w].mask.load(std::memory_order_relaxed) != 0)
                        return false;
                }
                return true;
            }
        };
    }
}
#endif // PUMP_CORE_LOCK_FREE_QUEUE_HH