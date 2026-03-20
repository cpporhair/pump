#ifndef PUMP_CORE_LOCK_FREE_STATE_HH
#define PUMP_CORE_LOCK_FREE_STATE_HH

#include <atomic>
#include <iostream>
#include <thread>
#include <vector>
#include <iomanip>

namespace pump::core {
    template<unsigned offset_v, unsigned width_v>
    struct bit_field {
        static_assert(offset_v + width_v <= 64, "Bit field exceeds 64 bits");
        static constexpr uint64_t Mask = (1ULL << width_v) - 1;
        static uint64_t decode(uint64_t raw) { return (raw >> offset_v) & Mask; }

        static uint64_t update(uint64_t raw, uint64_t val) {
            return (raw & ~(Mask << offset_v)) | ((val & Mask) << offset_v);
        }
    };

    // ==========================================
    // 2. 核心枚举与结果结构
    // ==========================================

    // 定义三种核心行为
    enum class action {
        abort, // 不更新，直接返回失败（逻辑终止）
        try_once, // 尝试更新一次，如果 CAS 失败（遇到竞争），则放弃并返回失败
        retry // 尝试更新，如果 CAS 失败（遇到竞争），拿到新值再次循环尝试（死磕到底）
    };

    // 前置声明
    struct state_view;

    // 用户回调的返回值封装
    struct step_result {
        uint64_t new_raw_value;
        action action;
    };

    // ==========================================
    // 3. 状态机快照包装器
    // ==========================================
    struct state_view {
        uint64_t raw_value;

        state_view(uint64_t val = 0) : raw_value(val) {
        }

        template<typename FieldType>
        uint64_t get() const { return FieldType::decode(raw_value); }

        template<typename FieldType>
        state_view &set(uint64_t val) {
            raw_value = FieldType::update(raw_value, val);
            return *this;
        }

        // --- 辅助函数：生成不同行为的返回值 ---

        // 1. 逻辑不满足，直接中止
        step_result abort() const {
            return {raw_value, action::abort};
        }

        // 2. 尝试一次，如果有竞争就放弃 (适用于非关键更新，比如统计数据采样)
        step_result commit_try_once() const {
            return {raw_value, action::try_once};
        }

        // 3. 循环直到成功 (适用于关键状态流转)
        step_result commit_loop() const {
            return {raw_value, action::retry};
        }

        void print_hex() const {
            std::cout << "0x" << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << raw_value <<
                std::dec << std::endl;
        }
    };

    // ==========================================
    // 4. 无锁状态机核心类
    // ==========================================
    class lock_free_state_machine {
    private:
        std::atomic<uint64_t> state_;

    public:
        lock_free_state_machine(uint64_t init_val = 0) : state_(init_val) {
        }

        // 核心 transition 函数
        // 返回值: true 表示更新成功，false 表示更新失败（无论是逻辑Abort还是CAS失败）
        template<typename Transformer>
        bool transition(Transformer &&transform_func) {
            // 读取初始值
            uint64_t old_raw = state_.load(std::memory_order_acquire);

            while (true) {
                state_view curr_view(old_raw);

                // 1. 调用用户逻辑，获取决策
                step_result result = transform_func(curr_view);

                // 2. 处理 Abort (用户逻辑决定不更新)
                if (result.action == action::abort) {
                    return false;
                }

                // 3. 优化：如果值没变，视为成功
                if (result.new_raw_value == old_raw) {
                    return true;
                }

                // 4. 执行 CAS
                // compare_exchange_weak:
                // - 成功: 写入新值，返回 true
                // - 失败: 将内存中的新值写入 old_raw，返回 false
                bool cas_success = state_.compare_exchange_weak(
                    old_raw,
                    result.new_raw_value,
                    std::memory_order_release,
                    std::memory_order_relaxed
                );

                if (cas_success) {
                    return true; // 更新成功
                }

                // 如果用户指定 TryOnce，遇到失败（竞争）则不再重试，直接返回失败
                if (result.action == action::try_once) {
                    return false;
                }

                // 如果是 Action::Retry，循环会自动继续
                // 此时 old_raw 已经被 compare_exchange_weak 更新为最新的值
                // transform_func 将在下一次循环中基于新值重新计算
            }
        }
    };
}

#endif //PUMP_CORE_LOCK_FREE_STATE_HH