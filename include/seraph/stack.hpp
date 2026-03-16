#pragma once

#include "locks.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <new>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <utility>
#include <vector>

namespace seraph {
    template <typename T> class stack {
      private:
        // Starts in a spinlock-protected vector mode and promotes once to lock-free CAS.

        // Fast promotion under practical workload contention.
        static constexpr size_t k_default_thread_threshold{3};
        static constexpr size_t k_default_streak_threshold{64};
#if defined(__cpp_lib_hardware_interference_size)
        static constexpr size_t k_destructive_interference_size{
                std::hardware_destructive_interference_size
        };
#else
        static constexpr size_t k_destructive_interference_size{128};
#endif

        struct alignas(k_destructive_interference_size) Node {
            T value;
            Node* next;

            template <typename... Args>
            Node(Node* n, Args&&... args) : value(std::forward<Args>(args)...), next(n) {}
        };

        struct alignas(k_destructive_interference_size) HazardRecord {
            std::atomic<std::thread::id> owner;
            std::atomic<Node*> pointer;
        };

        struct HazardReleaser {
            ~HazardReleaser() {
                if (local_hazard_) {
                    local_hazard_->pointer.store(nullptr, std::memory_order_release);
                    local_hazard_->owner.store(std::thread::id{}, std::memory_order_release);
                    local_hazard_ = nullptr;
                }
            }
        };

        // 16 used as this will run on 4-threads. Reduces hazard-table scan traffic.
        // 4 threads allows one hazard slot per active thread.
        static constexpr size_t k_max_hazard_pointers{16};
        static constexpr size_t k_retire_scan_threshold{64};
        static HazardRecord hazard_records_[k_max_hazard_pointers];
        static thread_local HazardRecord* local_hazard_;
        static thread_local HazardReleaser hazard_releaser_;
        static thread_local std::vector<Node*> retire_list_;

        class ActiveOperationScope {
          public:
            explicit ActiveOperationScope(stack& stack) : stack_(stack) {
                const size_t active_now(
                        stack_.active_ops_.fetch_add(1, std::memory_order_relaxed) + 1
                );
                stack_.observe_contention(active_now);
            }

            ~ActiveOperationScope() {
                stack_.active_ops_.fetch_sub(1, std::memory_order_relaxed);
            }

          private:
            stack& stack_;
        };

        static HazardRecord* acquire_hazard() {
            if (local_hazard_) {
                return local_hazard_;
            }

            for (size_t iii{0}; iii < k_max_hazard_pointers; ++iii) {
                std::thread::id empty;

                if (hazard_records_[iii].owner.compare_exchange_strong(
                            empty,
                            std::this_thread::get_id(),
                            std::memory_order_acq_rel
                    )) {
                    (void)hazard_releaser_;
                    local_hazard_ = &hazard_records_[iii];

                    return local_hazard_;
                }
            }

            std::terminate();
        }

        static void scan() {
            std::array<Node*, k_max_hazard_pointers> hazard_snapshot{};

            for (size_t iii{0}; iii < k_max_hazard_pointers; ++iii) {
                hazard_snapshot[iii] = hazard_records_[iii].pointer.load(std::memory_order_acquire);
            }

            size_t write_index{0};

            for (size_t read_index{0}; read_index < retire_list_.size(); ++read_index) {
                Node* retired_node(retire_list_[read_index]);
                bool keep_node{false};

                for (Node* hazard_node : hazard_snapshot) {
                    if (hazard_node == retired_node) {
                        keep_node = true;
                        break;
                    }
                }

                if (keep_node) {
                    retire_list_[write_index++] = retired_node;
                }
                else {
                    delete retired_node;
                }
            }

            retire_list_.resize(write_index);
        }

        static void retire(Node* node) {
            retire_list_.push_back(node);

            if (retire_list_.size() >= k_retire_scan_threshold) {
                scan();
            }
        }

        template <typename... Args> void cas_emplace_impl(Args&&... args) {
            Node* new_node(new Node(nullptr, std::forward<Args>(args)...));
            Node* old_head(cas_head_.load(std::memory_order_relaxed));

            do {
                new_node->next = old_head;
            } while (!cas_head_.compare_exchange_weak(
                    old_head,
                    new_node,
                    std::memory_order_release,
                    std::memory_order_relaxed
            ));

            cas_size_.fetch_add(1, std::memory_order_relaxed);
        }

        std::optional<T> cas_pop_impl() {
            HazardRecord* hazard(acquire_hazard());
            Node* old_head(cas_head_.load(std::memory_order_acquire));

            while (old_head) {
                hazard->pointer.store(old_head, std::memory_order_release);

                if (cas_head_.load(std::memory_order_acquire) != old_head) {
                    old_head = cas_head_.load(std::memory_order_acquire);
                    continue;
                }

                Node* next = old_head->next;

                if (cas_head_.compare_exchange_weak(
                            old_head,
                            next,
                            std::memory_order_acquire,
                            std::memory_order_relaxed
                    )) {
                    hazard->pointer.store(nullptr, std::memory_order_release);
                    cas_size_.fetch_sub(1, std::memory_order_relaxed);

                    std::optional<T> result(std::move(old_head->value));
                    retire(old_head);
                    return result;
                }
            }

            hazard->pointer.store(nullptr, std::memory_order_release);
            return std::nullopt;
        }

        std::optional<T> cas_top_impl() const {
            HazardRecord* hazard(acquire_hazard());
            Node* old_head(cas_head_.load(std::memory_order_acquire));

            while (old_head) {
                hazard->pointer.store(old_head, std::memory_order_release);

                if (cas_head_.load(std::memory_order_acquire) != old_head) {
                    old_head = cas_head_.load(std::memory_order_acquire);
                    continue;
                }

                std::optional<T> result(old_head->value);
                hazard->pointer.store(nullptr, std::memory_order_release);
                return result;
            }

            hazard->pointer.store(nullptr, std::memory_order_release);
            return std::nullopt;
        }

        bool cas_empty_impl() const noexcept {
            return cas_head_.load(std::memory_order_acquire) == nullptr;
        }

        size_t cas_size_impl() const noexcept {
            return cas_size_.load(std::memory_order_relaxed);
        }

        void clear_cas_nodes() {
            Node* node(cas_head_.load(std::memory_order_relaxed));

            while (node) {
                Node* next = node->next;
                delete node;
                node = next;
            }

            cas_head_.store(nullptr, std::memory_order_relaxed);
            cas_size_.store(0, std::memory_order_relaxed);
        }

        void observe_contention(size_t active_now) {
            if (using_cas_.load(std::memory_order_relaxed)) {
                return;
            }

            if (active_now >= contention_thread_threshold_) {
                const size_t streak(contention_streak_.fetch_add(1, std::memory_order_relaxed) + 1);

                if (streak >= promotion_streak_threshold_) {
                    promotion_requested_.store(true, std::memory_order_relaxed);
                }
            }
            else {
                contention_streak_.store(0, std::memory_order_relaxed);
            }
        }

        void maybe_promote_to_cas() {
            if (using_cas_.load(std::memory_order_acquire) ||
                !promotion_requested_.load(std::memory_order_relaxed)) {
                return;
            }

            std::unique_lock mode_guard(mode_mutex_);

            if (using_cas_.load(std::memory_order_relaxed)) {
                return;
            }

            std::vector<T> transfer_buffer;
            {
                SpinlockGuard guard(spin_lock_);
                transfer_buffer = std::move(spin_data_);
                spin_data_.clear();
            }

            for (auto& value : transfer_buffer) {
                cas_emplace_impl(std::move(value));
            }

            using_cas_.store(true, std::memory_order_release);
        }

        mutable std::shared_mutex mode_mutex_;

        alignas(k_destructive_interference_size) mutable Spinlock spin_lock_;
        std::vector<T> spin_data_;

        alignas(k_destructive_interference_size) std::atomic<Node*> cas_head_{nullptr};
        alignas(k_destructive_interference_size) std::atomic<size_t> cas_size_{0};

        alignas(k_destructive_interference_size) std::atomic<size_t> active_ops_{0};
        alignas(k_destructive_interference_size) std::atomic<size_t> contention_streak_{0};

        alignas(k_destructive_interference_size) std::atomic<bool> using_cas_{false};
        std::atomic<bool> promotion_requested_{false};

        const size_t contention_thread_threshold_;

        std::atomic<size_t> active_ops_{0};
        std::atomic<size_t> contention_streak_{0};
        std::atomic<bool> promotion_requested_{false};

      public:
        stack()
            : contention_thread_threshold_(k_default_thread_threshold),
              promotion_streak_threshold_(k_default_streak_threshold) {}

        explicit stack(size_t reserve_hint)
            : contention_thread_threshold_(k_default_thread_threshold),
              promotion_streak_threshold_(k_default_streak_threshold) {
            spin_data_.reserve(reserve_hint);
        }

        stack(size_t reserve_hint, size_t contention_thread_threshold, size_t streak_threshold)
            : contention_thread_threshold_(std::max<size_t>(2, contention_thread_threshold)),
              promotion_streak_threshold_(std::max<size_t>(1, streak_threshold)) {
            spin_data_.reserve(reserve_hint);
        }

        ~stack() {
            if (using_cas_.load(std::memory_order_acquire)) {
                clear_cas_nodes();
            }
        }

        stack(const stack&) = delete;
        stack& operator=(const stack&) = delete;

        void reserve(size_t n) {
            ActiveOperationScope scope(*this);
            maybe_promote_to_cas();

            std::shared_lock mode_guard(mode_mutex_);
            if (using_cas_.load(std::memory_order_acquire)) {
                return;
            }

            SpinlockGuard guard(spin_lock_);
            spin_data_.reserve(n);
        }

        void push(const T& value) {
            ActiveOperationScope scope(*this);
            maybe_promote_to_cas();

            std::shared_lock mode_guard(mode_mutex_);

            if (using_cas_.load(std::memory_order_acquire)) {
                cas_emplace_impl(value);
            }
            else {
                T temp(value);
                SpinlockGuard guard(spin_lock_);
                spin_data_.push_back(std::move(temp));
            }
        }

        void push(T&& value) {
            ActiveOperationScope scope(*this);
            maybe_promote_to_cas();

            std::shared_lock mode_guard(mode_mutex_);

            if (using_cas_.load(std::memory_order_acquire)) {
                cas_emplace_impl(std::move(value));
            }
            else {
                SpinlockGuard guard(spin_lock_);
                spin_data_.push_back(std::move(value));
            }
        }

        template <typename... Args> void emplace(Args&&... args) {
            ActiveOperationScope scope(*this);
            maybe_promote_to_cas();

            std::shared_lock mode_guard(mode_mutex_);

            if (using_cas_.load(std::memory_order_acquire)) {
                cas_emplace_impl(std::forward<Args>(args)...);
            }
            else {
                T temp(std::forward<Args>(args)...);
                SpinlockGuard guard(spin_lock_);
                spin_data_.push_back(std::move(temp));
            }
        }

        std::optional<T> pop() {
            ActiveOperationScope scope(*this);
            maybe_promote_to_cas();

            std::shared_lock mode_guard(mode_mutex_);

            if (using_cas_.load(std::memory_order_acquire)) {
                return cas_pop_impl();
            }

            SpinlockGuard guard(spin_lock_);

            if (spin_data_.empty()) {
                return std::nullopt;
            }

            std::optional<T> result(std::move(spin_data_.back()));
            spin_data_.pop_back();
            return result;
        }

        std::optional<T> top() const {
            std::shared_lock mode_guard(mode_mutex_);
            if (using_cas_.load(std::memory_order_acquire)) {
                return cas_top_impl();
            }

            SpinlockGuard guard(spin_lock_);
            if (spin_data_.empty()) {
                return std::nullopt;
            }

            return spin_data_.back();
        }

        bool empty() const noexcept {
            std::shared_lock mode_guard(mode_mutex_);
            if (using_cas_.load(std::memory_order_acquire)) {
                return cas_empty_impl();
            }

            SpinlockGuard guard(spin_lock_);
            return spin_data_.empty();
        }

        size_t size() const noexcept {
            std::shared_lock mode_guard(mode_mutex_);
            if (using_cas_.load(std::memory_order_acquire)) {
                return cas_size_impl();
            }

            SpinlockGuard guard(spin_lock_);
            return spin_data_.size();
        }

        bool is_using_cas() const noexcept {
            return using_cas_.load(std::memory_order_acquire);
        }
    };

    template <typename T>
    typename stack<T>::HazardRecord stack<T>::hazard_records_[stack<T>::k_max_hazard_pointers];

    template <typename T>
    thread_local typename stack<T>::HazardRecord* stack<T>::local_hazard_ = nullptr;

    template <typename T> thread_local typename stack<T>::HazardReleaser stack<T>::hazard_releaser_;

    template <typename T> thread_local std::vector<typename stack<T>::Node*> stack<T>::retire_list_;

} // namespace seraph
