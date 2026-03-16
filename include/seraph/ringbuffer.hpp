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
#if defined(__cpp_lib_hardware_interference_size)
        static constexpr size_t k_destructive_interference_size{
                std::hardware_destructive_interference_size
        };
#else
        static constexpr size_t k_destructive_interference_size{128};
#endif

        static constexpr size_t k_backoff_batch{32};
        static_assert((k_backoff_batch & (k_backoff_batch - 1)) == 0);

        struct alignas(k_destructive_interference_size) Slot {
            std::atomic<size_t> sequence{0};
            mutable std::atomic<std::uint32_t> readers{0};
            std::optional<T> value{};
        };

        struct alignas(k_destructive_interference_size) Cursor {
            std::atomic<size_t> value{0};
        };

        struct ReaderGuard {
            const Slot* slot{nullptr};

            ~ReaderGuard() {
                if (slot != nullptr) {
                    slot->readers.fetch_sub(1, std::memory_order_release);
                }
            }
        };

        static void cpu_relax_or_yield(size_t& spins) noexcept {
            ++spins;

            if ((spins & (k_backoff_batch - 1)) == 0) {
                std::this_thread::yield();
            }
            else {
                __asm__ __volatile__("yield");
            }
        }

        [[nodiscard]] static auto normalize_capacity(size_t requested) -> size_t {
            if (requested == 0) {
                throw std::invalid_argument("RingBuffer capacity must be > 0.");
            }

            const size_t capacity(std::bit_ceil(requested));

            if (capacity > (std::numeric_limits<size_t>::max() >> 1)) {
                throw std::length_error("RingBuffer capacity is too large.");
            }

            return capacity;
        }

        [[nodiscard]] auto slot_for(size_t position) noexcept -> Slot& {
            // With power-of-two capacity, masking is equivalent to modulo.
            return slots_[position & capacity_mask_];
        }

        [[nodiscard]] const Slot& slot_for(size_t position) const noexcept {
            return slots_[position & capacity_mask_];
        }

        [[nodiscard]] const Slot& mirrored_slot_for(size_t mirrored_position) const noexcept {
            return *(mirrored_slots_[mirrored_position & mirrored_mask_]);
        }

        [[nodiscard]] static bool
        slot_ready_for_enqueue(size_t sequence, size_t position) noexcept {
            return sequence == position;
        }

        [[nodiscard]] bool try_claim_enqueue_position(size_t& position) noexcept {
            return enqueue_pos_.value.compare_exchange_weak(
                    position,
                    position + 1,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed
            );
        }

        void refresh_enqueue_position(size_t& position) const noexcept {
            position = enqueue_pos_.value.load(std::memory_order_relaxed);
        }

        [[nodiscard]] size_t reserve_enqueue_position() noexcept {
            size_t position(enqueue_pos_.value.load(std::memory_order_relaxed));
            size_t spins{0};

            // Intentionally unbounded: blocking producer API retries until a slot claim succeeds.
            while (true) {
                Slot& slot(slot_for(position));
                const size_t sequence(slot.sequence.load(std::memory_order_acquire));

                if (slot_ready_for_enqueue(sequence, position)) {
                    if (try_claim_enqueue_position(position)) {
                        return position;
                    }

                    continue;
                }

                cpu_relax_or_yield(spins);
                refresh_enqueue_position(position);
            }
        }

        void publish_enqueue(size_t position, Slot& slot) noexcept {
            // `position + 1`: payload is now visible and eligible for pop/front/back.
            slot.sequence.store(position + 1, std::memory_order_release);
            size_.value.fetch_add(1, std::memory_order_relaxed);
        }

        [[nodiscard]] std::optional<T>
        try_copy_slot_value(const Slot& slot, size_t expected_sequence) const {
            size_t retries{0};

            while (retries < k_backoff_batch) {
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

                ++retries;
                cpu_relax_or_yield(retries);
            }

            return std::nullopt;
        }

        size_t capacity_{0};
        size_t capacity_mask_{0};
        size_t mirrored_mask_{0};
        std::vector<Slot> slots_{};
        std::vector<Slot*> mirrored_slots_{};

        alignas(k_destructive_interference_size) Cursor enqueue_pos_{};
        alignas(k_destructive_interference_size) Cursor dequeue_pos_{};
        alignas(k_destructive_interference_size) Cursor size_{};

      public:
        explicit RingBuffer(size_t data_size)
            : capacity_(normalize_capacity(data_size)), capacity_mask_(capacity_ - 1),
              mirrored_mask_((capacity_ << 1) - 1), slots_(capacity_),
              mirrored_slots_(capacity_ << 1) {

            for (size_t iii{0}; iii < capacity_; ++iii) {
                slots_[iii].sequence.store(iii, std::memory_order_relaxed);
                mirrored_slots_[iii] = &slots_[iii];
                mirrored_slots_[iii + capacity_] = &slots_[iii];
            }
        }

        ~RingBuffer() = default;

        RingBuffer(const RingBuffer&) = delete;
        RingBuffer& operator=(const RingBuffer&) = delete;
        RingBuffer(RingBuffer&&) = delete;
        RingBuffer& operator=(RingBuffer&&) = delete;

        void push(const T& value) {
            emplace(value);
        }

        void push(T&& value) {
            emplace(std::move(value));
        }

        template <typename... Args> void emplace(Args&&... args) {
            if constexpr (std::is_nothrow_constructible_v<T, Args&&...>) {
                const size_t position(reserve_enqueue_position());
                Slot& slot(slot_for(position));
                slot.value.emplace(std::forward<Args>(args)...);
                publish_enqueue(position, slot);
                return;
            }

            T staged(std::forward<Args>(args)...);

            const size_t position(reserve_enqueue_position());
            Slot& slot(slot_for(position));

            slot.value.emplace(std::move(staged));
            publish_enqueue(position, slot);
        }

        [[nodiscard]] std::optional<T> pop() {
            size_t position(dequeue_pos_.value.load(std::memory_order_relaxed));
            size_t spins{0};

            // Intentionally unbounded: retries under contention, exits on empty once no
            // in-flight enqueues remain, or on successful claim.
            while (true) {
                Slot& slot(slot_for(position));
                const size_t sequence(slot.sequence.load(std::memory_order_acquire));
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
                        // `position + (capacity_ << 1)`: transient busy state during pop.
                        slot.sequence.store(position + (capacity_ << 1), std::memory_order_release);

                        size_t reader_spins{0};
                        while (slot.readers.load(std::memory_order_acquire) != 0) {
                            cpu_relax_or_yield(reader_spins);
                        }

                        std::optional<T> result;
                        if (slot.value.has_value()) {
                            result.emplace(std::move(*(slot.value)));
                            slot.value.reset();
                        }

                        // `position + capacity_`: slot recycled and free for next enqueue cycle.
                        slot.sequence.store(position + capacity_, std::memory_order_release);
                        size_.value.fetch_sub(1, std::memory_order_relaxed);

                        return result;
                    }

                    continue;
                }

                if (diff < 0) {
                    const size_t tail(enqueue_pos_.value.load(std::memory_order_acquire));

                    if (tail == position) {
                        return std::nullopt;
                    }

                    cpu_relax_or_yield(spins);
                    position = dequeue_pos_.value.load(std::memory_order_relaxed);
                    continue;
                }

                cpu_relax_or_yield(spins);
                position = dequeue_pos_.value.load(std::memory_order_relaxed);
            }
        }

        [[nodiscard]] std::optional<T> front() const {
            const size_t head(dequeue_pos_.value.load(std::memory_order_acquire));
            const size_t tail(enqueue_pos_.value.load(std::memory_order_acquire));

            if (head >= tail) {
                return std::nullopt;
            }

            const Slot& slot(slot_for(head));
            return try_copy_slot_value(slot, head + 1);
        }

        [[nodiscard]] std::optional<T> back() const {
            const size_t head(dequeue_pos_.value.load(std::memory_order_acquire));
            const size_t tail(enqueue_pos_.value.load(std::memory_order_acquire));

            if (head >= tail) {
                return std::nullopt;
            }

            const size_t span(((tail - head) < capacity_) ? (tail - head) : capacity_);
            const size_t start_position(tail - 1);

            // Mirrored view provides a linear walk over wrap-around without branched index fixes.
            const size_t mirrored_start((start_position & capacity_mask_) + capacity_);

            for (size_t offset{0}; offset < span; ++offset) {
                const size_t position(start_position - offset);
                const Slot& slot(mirrored_slot_for(mirrored_start - offset));

                if (std::optional<T> value(try_copy_slot_value(slot, position + 1));
                    value.has_value()) {
                    return value;
                }
            }

            return std::nullopt;
        }

        [[nodiscard]] size_t size() const noexcept {
            const size_t enq(enqueue_pos_.value.load(std::memory_order_acquire));
            const size_t deq(dequeue_pos_.value.load(std::memory_order_acquire));

            return enq - deq;
        }

        [[nodiscard]] bool empty() const noexcept {
            return size() == 0;
        }
    };
} // namespace seraph
