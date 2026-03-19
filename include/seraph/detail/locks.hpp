#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

#if defined(__aarch64__) || defined(__arm64__)
#include <arm_acle.h>
#endif

namespace seraph::detail {
#if defined(__cpp_lib_hardware_interference_size)
    static constexpr size_t k_spinlock_alignment{std::hardware_destructive_interference_size};
#else
    static constexpr size_t k_spinlock_alignment{128};
#endif

    class alignas(k_spinlock_alignment) Spinlock {
      public:
        Spinlock() noexcept : state_(0) {}

        void lock() noexcept {
            if (state_.exchange(1, std::memory_order_acquire) == 0) [[likely]] {
                return;
            }

            while (true) {
                while (state_.load(std::memory_order_relaxed) != 0) {
#if defined(__aarch64__) || defined(__arm64__)
                    __builtin_arm_yield();
#else
                    __builtin_ia32_pause();
#endif
                }

                if (state_.exchange(1, std::memory_order_acquire) == 0) {
                    return;
                }
            }
        }

        void unlock() noexcept {
            state_.store(0, std::memory_order_release);
        }

        bool try_lock() noexcept {
            return state_.exchange(1, std::memory_order_acquire) == 0;
        }

      private:
        std::atomic<std::uint32_t> state_;
    };

    class [[nodiscard]] SpinlockGuard {
      public:
        explicit SpinlockGuard(Spinlock& lock) noexcept : lock_(lock) {
            lock_.lock();
        }

        ~SpinlockGuard() noexcept {
            lock_.unlock();
        }

        SpinlockGuard(const SpinlockGuard&) = delete;
        SpinlockGuard& operator=(const SpinlockGuard&) = delete;

      private:
        Spinlock& lock_;
    };
} // namespace seraph::detail
