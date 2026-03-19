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
    static constexpr size_t k_spinlock_alignment{64};
#endif

    class alignas(k_spinlock_alignment) Spinlock {
      public:
        Spinlock() noexcept : state_(0) {}

        void lock() noexcept {
            // Optimized TTAS: Try to acquire immediately first.
            // On ARM64, this generates a 'CAS' or 'LDSET' instruction if LSE is enabled.
            if (state_.exchange(1, std::memory_order_acquire) == 0) [[likely]] {
                return;
            }

            // Contented path
            while (true) {
                // 1. Busy-wait with 'yield' to save power and signal the hardware
                // that this core is in a spin-loop.
                while (state_.load(std::memory_order_relaxed) != 0) {
#if defined(__aarch64__) || defined(__arm64__)
                    __builtin_arm_yield();
#else
                    __builtin_ia32_pause();
#endif
                }

                // 2. Try to grab the lock again
                if (state_.exchange(1, std::memory_order_acquire) == 0) {
                    return;
                }
            }
        }

        void unlock() noexcept {
            // Release semantics ensure all previous writes are visible to the
            // next thread that acquires this lock.
            state_.store(0, std::memory_order_release);
        }

        // Try_lock is highly recommended for performance-critical ARM code
        bool try_lock() noexcept {
            return state_.exchange(1, std::memory_order_acquire) == 0;
        }

      private:
        // Using uint32_t instead of atomic_flag allows for easier
        // integration with WFE (Wait For Event) if needed later.
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
