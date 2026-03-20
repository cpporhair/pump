//
// Created by null on 25-5-20.
//

#ifndef PUMP_CORE_RING_QUEUE_HH
#define PUMP_CORE_RING_QUEUE_HH

#include <vector>

namespace pump::core {
    template<typename T>
    struct ring_queue {
        explicit
        ring_queue(size_t capacity)
            : buffer_(capacity)
            , capacity_(capacity)
            , head_(0)
            , tail_(0)
            , size_(0) {
        }

        bool
        enqueue(const T& item) {
            if (size_ >= capacity_) {
                return false;
            }
            buffer_[tail_] = item;
            tail_ = (tail_ + 1) % capacity_;
            ++size_;
            return true;
        }

        auto
        front() {
            return buffer_[head_];
        }

        void*
        tail_buffer() {
            return buffer_.data() + tail_;
        }

        void
        tail_offset(size_t offset) {
            tail_ = (tail_ + offset) % capacity_;
        }

        bool
        dequeue(T& item) {
            if (size_ == 0) {
                return false;
            }
            item = buffer_[head_];
            head_ = (head_ + 1) % capacity_;
            --size_;
            return true;
        }

        [[nodiscard]] bool
        empty() const {
            return size_ == 0;
        }

        [[nodiscard]] size_t
        size() const {
            return size_;
        }

        [[nodiscard]] size_t
        capacity() const {
            return capacity_;
        }

    private:
        std::vector<T> buffer_;
        size_t capacity_;
        size_t head_;
        size_t tail_;
        size_t size_;
    };
}


#endif //PUMP_CORE_RING_QUEUE_HH