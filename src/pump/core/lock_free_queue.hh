#ifndef PUMP_CORE_LOCK_FREE_QUEUE_HH
#define PUMP_CORE_LOCK_FREE_QUEUE_HH

#pragma once
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>
#include <memory>
#include <cstdio>
#include <cstdlib>

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

        template<typename T>
        struct queue {
        private:
            uint32_t capacity_;
            uint32_t mask_;
            alignas(64) std::atomic<uint32_t> tail_;
            alignas(64) std::atomic<uint32_t> head_;
            std::unique_ptr<cell<T>[]> buffer_;

        public:
            explicit queue(size_t capacity = 1024)
                : capacity_(capacity), mask_(capacity - 1), tail_(0), head_(0)
                , buffer_(new cell<T>[capacity]) {
                if ((capacity & (capacity - 1)) != 0) { fprintf(stderr, "FATAL: queue capacity %zu must be power of 2\n", capacity); abort(); }
                for (uint32_t i = 0; i < capacity_; ++i)
                    buffer_[i].sequence.store(i, std::memory_order_relaxed);
            }
            ~queue() = default;

            queue(const queue&) = delete;
            queue& operator=(const queue&) = delete;

            bool try_enqueue(T&& value) noexcept {
                uint32_t pos = tail_.load(std::memory_order_relaxed);
                while (true) {
                    cell<T>& c = buffer_[pos & mask_];
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
                    cell<T>& c = buffer_[pos & mask_];
                    uint32_t seq = c.sequence.load(std::memory_order_acquire);
                    int32_t dif = (int32_t)seq - (int32_t)(pos + 1);
                    if (dif == 0) {
                        if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                            result = std::move(c.value);
                            c.sequence.store(pos + capacity_, std::memory_order_release);
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
        template<typename T>
        struct queue {
        private:
            uint32_t capacity_;
            uint32_t mask_;
            alignas(64) std::atomic<uint32_t> tail_;
            alignas(64) std::atomic<uint32_t> head_;
            std::unique_ptr<mpmc::cell<T>[]> buffer_;

        public:
            explicit queue(size_t capacity = 1024)
                : capacity_(capacity), mask_(capacity - 1), tail_(0), head_(0)
                , buffer_(new mpmc::cell<T>[capacity]) {
                if ((capacity & (capacity - 1)) != 0) { fprintf(stderr, "FATAL: queue capacity %zu must be power of 2\n", capacity); abort(); }
                for (uint32_t i = 0; i < capacity_; ++i)
                    buffer_[i].sequence.store(i, std::memory_order_relaxed);
            }

            ~queue() = default;

            queue(const queue&) = delete;
            queue& operator=(const queue&) = delete;

            bool try_enqueue(T&& value) noexcept {
                uint32_t pos = tail_.load(std::memory_order_relaxed);
                while (true) {
                    mpmc::cell<T>& cell = buffer_[pos & mask_];
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
                mpmc::cell<T>& cell = buffer_[pos & mask_];
                uint32_t seq = cell.sequence.load(std::memory_order_acquire);
                int32_t dif = (int32_t)seq - (int32_t)(pos + 1);
                if (dif == 0) {
                    head_.store(pos + 1, std::memory_order_relaxed);
                    result = std::move(cell.value);
                    cell.sequence.store(pos + capacity_, std::memory_order_release);
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
        template<typename T>
        struct queue {
        private:
            size_t capacity_;
            size_t mask_;
            size_t head_ = 0;
            size_t tail_ = 0;
            std::unique_ptr<T[]> buffer_;

        public:
            explicit queue(size_t capacity = 1024)
                : capacity_(capacity), mask_(capacity - 1)
                , buffer_(new T[capacity]{}) {
                if ((capacity & (capacity - 1)) != 0) { fprintf(stderr, "FATAL: queue capacity %zu must be power of 2\n", capacity); abort(); }
            }
            ~queue() = default;
            queue(const queue&) = delete;
            queue& operator=(const queue&) = delete;
            queue(queue&& rhs) noexcept
                : capacity_(rhs.capacity_), mask_(rhs.mask_)
                , head_(rhs.head_), tail_(rhs.tail_)
                , buffer_(std::move(rhs.buffer_)) {
                rhs.head_ = 0; rhs.tail_ = 0;
            }
            queue& operator=(queue&&) = delete;

            bool try_enqueue(T&& value) noexcept {
                size_t next = (tail_ + 1) & mask_;
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
                head_ = (head_ + 1) & mask_;
                return true;
            }

            bool empty() const noexcept {
                return head_ == tail_;
            }

            bool full() const noexcept {
                return ((tail_ + 1) & mask_) == head_;
            }

            size_t size() const noexcept {
                return (tail_ + capacity_ - head_) & mask_;
            }
        };
    }

    namespace spsc {
        template<typename T>
        struct queue {
        private:
            size_t capacity_;
            size_t mask_;
            alignas(64) std::atomic<size_t> tail_;
            alignas(64) size_t tail_cache_ = 0;
            alignas(64) std::atomic<size_t> head_;
            alignas(64) size_t head_cache_ = 0;
            std::unique_ptr<T[]> buffer_;

        public:
            explicit queue(size_t capacity = 1024)
                : capacity_(capacity), mask_(capacity - 1)
                , tail_(0), head_(0)
                , buffer_(new T[capacity]{}) {
                if ((capacity & (capacity - 1)) != 0) { fprintf(stderr, "FATAL: queue capacity %zu must be power of 2\n", capacity); abort(); }
            }
            ~queue() = default;
            queue(const queue&) = delete;
            queue& operator=(const queue&) = delete;
            queue(queue&&) = default;
            queue& operator=(queue&& rhs) noexcept {
                capacity_ = rhs.capacity_;
                mask_ = rhs.mask_;
                tail_.store(rhs.tail_.load(std::memory_order_relaxed), std::memory_order_relaxed);
                tail_cache_ = rhs.tail_cache_;
                head_.store(rhs.head_.load(std::memory_order_relaxed), std::memory_order_relaxed);
                head_cache_ = rhs.head_cache_;
                buffer_ = std::move(rhs.buffer_);
                return *this;
            }

            bool try_enqueue(T&& value) noexcept {
                size_t tail = tail_.load(std::memory_order_relaxed);
                size_t next = (tail + 1) & mask_;
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
                head_.store((head + 1) & mask_, std::memory_order_release);
                return true;
            }

            bool empty() const noexcept {
                return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed);
            }

            bool full() const noexcept {
                return ((tail_.load(std::memory_order_relaxed) + 1) & mask_) == head_.load(std::memory_order_relaxed);
            }

            size_t size_approx() const noexcept {
                size_t h = head_.load(std::memory_order_relaxed);
                size_t t = tail_.load(std::memory_order_relaxed);
                return (t + capacity_ - h) & mask_;
            }
        };
    }

    // Per-core SPSC queue group with active bitmap.
    // Each source core gets a dedicated SPSC queue; bitmap tracks which cores have pending data.
    inline constexpr uint32_t MAX_CORES = 128;
    inline constexpr uint32_t BITMAP_WORDS = (MAX_CORES + 63) / 64;

    inline thread_local uint32_t this_core_id = 0;

    namespace per_core {
        template<typename T, bool abort_on_full = true>
        struct queue {
        private:
            size_t capacity_;
            std::unique_ptr<spsc::queue<T>[]> queues_;
            local::queue<T> local_q_;          // same-core fast path (zero atomics)
            uint32_t owner_core_ = UINT32_MAX; // core that calls drain()

            struct alignas(64) bitmap_word { std::atomic<uint64_t> mask{0}; };
            std::array<bitmap_word, BITMAP_WORDS> active_mask_{};

        public:
            explicit queue(size_t capacity = 1024, uint32_t owner_core = UINT32_MAX)
                : capacity_(capacity)
                , queues_(new spsc::queue<T>[MAX_CORES])
                , local_q_(capacity)
                , owner_core_(owner_core) {
                if ((capacity & (capacity - 1)) != 0) { fprintf(stderr, "FATAL: queue capacity %zu must be power of 2\n", capacity); abort(); }
                for (uint32_t i = 0; i < MAX_CORES; ++i)
                    queues_[i] = spsc::queue<T>(capacity);
            }
            ~queue() = default;
            queue(const queue&) = delete;
            queue& operator=(const queue&) = delete;

            void set_owner_core(uint32_t core) noexcept { owner_core_ = core; }

            bool try_enqueue(T&& value) noexcept {
                // Same-core fast path: local queue, zero atomics
                if (this_core_id == owner_core_) {
                    if (local_q_.try_enqueue(std::move(value)))
                        return true;
                    if constexpr (abort_on_full) {
                        fprintf(stderr, "FATAL: per_core::queue local full (capacity=%zu, core=%u)\n",
                                capacity_, this_core_id);
                        abort();
                    }
                    return false;
                }
                if (queues_[this_core_id].try_enqueue(std::move(value))) {
                    uint32_t word = this_core_id / 64;
                    uint64_t bit  = 1ULL << (this_core_id % 64);
                    active_mask_[word].mask.fetch_or(bit, std::memory_order_release);
                    return true;
                }
                if constexpr (abort_on_full) {
                    fprintf(stderr, "FATAL: per_core::queue full (capacity=%zu, core=%u)\n",
                            capacity_, this_core_id);
                    abort();
                }
                return false;
            }

            bool try_enqueue(const T& value) noexcept {
                return try_enqueue(T(value));
            }

            template<typename F>
            bool drain(F&& handler) {
                bool worked = false;
                // Local queue first (highest priority, zero overhead)
                {
                    T item;
                    while (local_q_.try_dequeue(item)) {
                        handler(std::move(item));
                        worked = true;
                    }
                }
                // Then cross-core SPSC queues
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
                // Local first
                {
                    T item;
                    if (local_q_.try_dequeue(item))
                        return std::move(item);
                }
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
                if (!local_q_.empty()) return false;
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
