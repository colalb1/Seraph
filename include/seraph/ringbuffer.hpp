#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace seraph {
    template <typename T> class RingBuffer {
      private:
        static constexpr std::size_t k_destructive_interference_size{
                std::hardware_destructive_interference_size
        };
        static constexpr std::size_t k_backoff_batch{32};

        struct Slot {
            std::atomic<std::size_t> sequence{0};
            mutable std::atomic<std::uint32_t> readers{0};
            std::optional<T> value{};
        };

        struct alignas(k_destructive_interference_size) Cursor {
            std::atomic<std::size_t> value{0};
        };

        struct ReaderGuard {
            const Slot* slot{nullptr};

            ~ReaderGuard() {
                if (slot != nullptr) {
                    slot->readers.fetch_sub(1, std::memory_order_release);
                }
            }
        };

        static void batched_backoff(std::size_t& spins) noexcept {
            ++spins;

            if ((spins & (k_backoff_batch - 1)) == 0) {
                std::this_thread::yield();
            }
            else {
                __asm__ __volatile__("yield");
            }
        }

        [[nodiscard]] static auto normalize_capacity(std::size_t requested) -> std::size_t {
            if (requested == 0) {
                throw std::invalid_argument("RingBuffer capacity must be > 0.");
            }

            const std::size_t capacity(std::bit_ceil(requested));

            if (capacity > (std::numeric_limits<std::size_t>::max() >> 1)) {
                throw std::length_error("RingBuffer capacity is too large.");
            }

            return capacity;
        }

        [[nodiscard]] auto slot_for(std::size_t position) noexcept -> Slot& {
            return slots_[position & capacity_mask_];
        }

        [[nodiscard]] auto slot_for(std::size_t position) const noexcept -> const Slot& {
            return slots_[position & capacity_mask_];
        }

        [[nodiscard]] auto mirrored_slot_for(std::size_t mirrored_position
        ) const noexcept -> const Slot& {
            return *(mirrored_slots_[mirrored_position & mirrored_mask_]);
        }

        [[nodiscard]] auto reserve_enqueue_position() noexcept -> std::size_t {
            std::size_t position(enqueue_pos_.value.load(std::memory_order_relaxed));
            std::size_t spins{0};

            while (true) {
                Slot& slot(slot_for(position));
                const std::size_t sequence(slot.sequence.load(std::memory_order_acquire));
                const std::ptrdiff_t diff(
                        static_cast<std::ptrdiff_t>(sequence) -
                        static_cast<std::ptrdiff_t>(position)
                );

                if (diff == 0) {
                    if (enqueue_pos_.value.compare_exchange_weak(
                                position,
                                position + 1,
                                std::memory_order_relaxed,
                                std::memory_order_relaxed
                        )) {
                        return position;
                    }

                    continue;
                }

                batched_backoff(spins);
                position = enqueue_pos_.value.load(std::memory_order_relaxed);
            }
        }

        void publish_enqueue(std::size_t position, Slot& slot) noexcept {
            slot.sequence.store(position + 1, std::memory_order_release);
            size_.value.fetch_add(1, std::memory_order_relaxed);
        }

        [[nodiscard]] auto try_copy_slot_value(const Slot& slot, std::size_t expected_sequence)
                const -> std::optional<T> {
            std::size_t retries{0};

            while (true) {
                if (slot.sequence.load(std::memory_order_acquire) != expected_sequence) {
                    return std::nullopt;
                }

                slot.readers.fetch_add(1, std::memory_order_acq_rel);
                ReaderGuard guard{&slot};

                if (slot.sequence.load(std::memory_order_acquire) == expected_sequence) {
                    if constexpr (std::is_copy_constructible_v<T>) {
                        return slot.value;
                    }

                    return std::nullopt;
                }

                if (++retries >= k_backoff_batch) {
                    return std::nullopt;
                }

                batched_backoff(retries);
            }
        }

        std::size_t capacity_{0};
        std::size_t capacity_mask_{0};
        std::size_t mirrored_mask_{0};
        std::vector<Slot> slots_{};
        std::vector<Slot*> mirrored_slots_{};

        Cursor enqueue_pos_{};
        Cursor dequeue_pos_{};
        Cursor size_{};

      public:
        explicit RingBuffer(std::size_t data_size)
            : capacity_(normalize_capacity(data_size)), capacity_mask_(capacity_ - 1),
              mirrored_mask_((capacity_ << 1) - 1), slots_(capacity_),
              mirrored_slots_(capacity_ << 1) {

            for (std::size_t iii{0}; iii < capacity_; ++iii) {
                slots_[iii].sequence.store(iii, std::memory_order_relaxed);
                mirrored_slots_[iii] = &slots_[iii];
                mirrored_slots_[iii + capacity_] = &slots_[iii];
            }
        }

        ~RingBuffer() = default;

        RingBuffer(const RingBuffer&) = delete;
        auto operator=(const RingBuffer&) -> RingBuffer& = delete;
        RingBuffer(RingBuffer&&) = delete;
        auto operator=(RingBuffer&&) -> RingBuffer& = delete;

        void push(const T& value) {
            emplace(value);
        }

        void push(T&& value) {
            emplace(std::move(value));
        }

        template <typename... Args> void emplace(Args&&... args) {
            if constexpr (std::is_nothrow_constructible_v<T, Args&&...>) {
                const std::size_t position(reserve_enqueue_position());
                Slot& slot(slot_for(position));
                slot.value.emplace(std::forward<Args>(args)...);
                publish_enqueue(position, slot);
                return;
            }

            T staged(std::forward<Args>(args)...);
            const std::size_t position(reserve_enqueue_position());
            Slot& slot(slot_for(position));
            slot.value.emplace(std::move(staged));
            publish_enqueue(position, slot);
        }

        [[nodiscard]] auto pop() -> std::optional<T> {
            std::size_t position(dequeue_pos_.value.load(std::memory_order_relaxed));
            std::size_t spins{0};

            while (true) {
                Slot& slot(slot_for(position));
                const std::size_t sequence(slot.sequence.load(std::memory_order_acquire));
                const std::ptrdiff_t diff(
                        static_cast<std::ptrdiff_t>(sequence) -
                        static_cast<std::ptrdiff_t>(position + 1)
                );

                if (diff == 0) {
                    if (dequeue_pos_.value.compare_exchange_weak(
                                position,
                                position + 1,
                                std::memory_order_relaxed,
                                std::memory_order_relaxed
                        )) {
                        // Block peeks on this slot while payload is being moved/reset.
                        slot.sequence.store(position + (capacity_ << 1), std::memory_order_release);

                        std::size_t reader_spins{0};
                        while (slot.readers.load(std::memory_order_acquire) != 0) {
                            batched_backoff(reader_spins);
                        }

                        std::optional<T> result;
                        if (slot.value.has_value()) {
                            result.emplace(std::move(*(slot.value)));
                            slot.value.reset();
                        }

                        slot.sequence.store(position + capacity_, std::memory_order_release);
                        size_.value.fetch_sub(1, std::memory_order_relaxed);

                        return result;
                    }

                    continue;
                }

                if (diff < 0) {
                    return std::nullopt;
                }

                batched_backoff(spins);
                position = dequeue_pos_.value.load(std::memory_order_relaxed);
            }
        }

        [[nodiscard]] auto front() const -> std::optional<T> {
            const std::size_t head(dequeue_pos_.value.load(std::memory_order_acquire));
            const std::size_t tail(enqueue_pos_.value.load(std::memory_order_acquire));

            if (head >= tail) {
                return std::nullopt;
            }

            const Slot& slot(slot_for(head));
            return try_copy_slot_value(slot, head + 1);
        }

        [[nodiscard]] auto back() const -> std::optional<T> {
            const std::size_t head(dequeue_pos_.value.load(std::memory_order_acquire));
            const std::size_t tail(enqueue_pos_.value.load(std::memory_order_acquire));

            if (head >= tail) {
                return std::nullopt;
            }

            const std::size_t span(((tail - head) < capacity_) ? (tail - head) : capacity_);
            const std::size_t start_position(tail - 1);

            // Mirrored view provides a linear walk over wrap-around without branchy index fixes.
            const std::size_t mirrored_start((start_position & capacity_mask_) + capacity_);

            for (std::size_t offset{0}; offset < span; ++offset) {
                const std::size_t position(start_position - offset);
                const Slot& slot(mirrored_slot_for(mirrored_start - offset));

                if (std::optional<T> value(try_copy_slot_value(slot, position + 1));
                    value.has_value()) {
                    return value;
                }
            }

            return std::nullopt;
        }

        [[nodiscard]] auto empty() const noexcept -> bool {
            return size_.value.load(std::memory_order_acquire) == 0;
        }

        [[nodiscard]] auto size() const noexcept -> std::size_t {
            return size_.value.load(std::memory_order_acquire);
        }
    };
} // namespace seraph
