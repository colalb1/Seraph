#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <exception>
#include <new>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace seraph {

    template <typename T> class queue {
      private:
#if defined(__cpp_lib_hardware_interference_size)
        static constexpr size_t k_destructive_interference_size{
                std::hardware_destructive_interference_size
        };
#else
        static constexpr size_t k_destructive_interference_size{64};
#endif

        struct Node {
            std::atomic<Node*> next;
            std::optional<T> value;

            Node() : next(nullptr), value(std::nullopt) {}

            template <typename... Args>
            explicit Node(Args&&... args)
                : next(nullptr), value(std::in_place, std::forward<Args>(args)...) {}
        };

        struct alignas(k_destructive_interference_size) HazardRecord {
            std::atomic<std::thread::id> owner;
            std::atomic<Node*> pointer;
        };

        struct HazardReleaser {
            ~HazardReleaser() {
                for (HazardRecord*& hazard : local_hazards_) {
                    if (!hazard) {
                        continue;
                    }

                    hazard->pointer.store(nullptr, std::memory_order_release);
                    hazard->owner.store(std::thread::id{}, std::memory_order_release);
                    hazard = nullptr;
                }
            }
        };

        static constexpr size_t k_max_hazard_pointers{32};
        static constexpr size_t k_local_hazard_slots{2};
        static constexpr size_t k_hazard_clear_interval{64};
        static constexpr size_t k_retire_scan_threshold{256};
        static constexpr size_t k_retire_scan_budget{16};

        static HazardRecord hazard_records_[k_max_hazard_pointers];
        static thread_local std::array<HazardRecord*, k_local_hazard_slots> local_hazards_;
        static thread_local HazardReleaser hazard_releaser_;
        static thread_local size_t hazard_ops_since_clear_;
        static thread_local std::vector<Node*> retire_list_;

        static auto acquire_hazard(size_t slot) -> HazardRecord* {
            HazardRecord* hazard(local_hazards_[slot]);

            if (hazard) {
                return hazard;
            }

            for (size_t iii{0}; iii < k_max_hazard_pointers; ++iii) {
                std::thread::id empty;

                if (hazard_records_[iii].owner.compare_exchange_strong(
                            empty,
                            std::this_thread::get_id(),
                            std::memory_order_acq_rel
                    )) {
                    (void)hazard_releaser_;
                    local_hazards_[slot] = &hazard_records_[iii];

                    return local_hazards_[slot];
                }
            }

            std::terminate();
        }

        static void clear_local_hazard_pointers() {
            for (HazardRecord* hazard : local_hazards_) {
                if (hazard) {
                    hazard->pointer.store(nullptr, std::memory_order_release);
                }
            }
        }

        static void maybe_clear_local_hazard_pointers(bool force = false) {
            if (!force) {
                ++hazard_ops_since_clear_;
                if (hazard_ops_since_clear_ < k_hazard_clear_interval) {
                    return;
                }
            }

            clear_local_hazard_pointers();
            hazard_ops_since_clear_ = 0;
        }

        static void scan_incremental(size_t scan_budget) {
            if (retire_list_.empty() || scan_budget == 0) {
                return;
            }

            std::array<Node*, k_max_hazard_pointers> hazard_snapshot{};
            size_t active_hazards{0};

            for (size_t iii{0}; iii < k_max_hazard_pointers; ++iii) {
                Node* hazard_ptr(hazard_records_[iii].pointer.load(std::memory_order_acquire));

                if (hazard_ptr) {
                    hazard_snapshot[active_hazards++] = hazard_ptr;
                }
            }

            if (active_hazards == 0) {
                const size_t reclaim_count(
                        (retire_list_.size() < scan_budget) ? retire_list_.size() : scan_budget
                );

                for (size_t iii{0}; iii < reclaim_count; ++iii) {
                    delete retire_list_.back();
                    retire_list_.pop_back();
                }

                return;
            }

            size_t inspected{0};
            size_t read_index{0};

            while (inspected < scan_budget && read_index < retire_list_.size()) {
                Node* retired_node(retire_list_[read_index]);
                bool keep_node{false};

                for (size_t iii{0}; iii < active_hazards; ++iii) {
                    if (hazard_snapshot[iii] == retired_node) {
                        keep_node = true;
                        break;
                    }
                }
                ++inspected;

                if (keep_node) {
                    ++read_index;
                }
                else {
                    delete retired_node;
                    retire_list_[read_index] = retire_list_.back();
                    retire_list_.pop_back();
                }
            }
        }

        static void retire_node(Node* node) {
            if (retire_list_.capacity() < k_retire_scan_threshold) {
                retire_list_.reserve(k_retire_scan_threshold);
            }

            retire_list_.push_back(node);

            if (retire_list_.size() >= k_retire_scan_threshold) {
                scan_incremental(k_retire_scan_budget);
            }
        }

        void clear_live_nodes() noexcept {
            Node* node(head_.load(std::memory_order_relaxed));

            while (node) {
                Node* next(node->next.load(std::memory_order_relaxed));
                delete node;
                node = next;
            }

            head_.store(nullptr, std::memory_order_relaxed);
            tail_.store(nullptr, std::memory_order_relaxed);
            size_.store(0, std::memory_order_relaxed);
        }

        static void clear_local_retired_nodes() noexcept {
            for (Node* node : retire_list_) {
                delete node;
            }

            retire_list_.clear();
        }

        std::atomic<Node*> head_{nullptr};
        std::atomic<Node*> tail_{nullptr};
        std::atomic<size_t> size_{0};

      public:
        queue() {
            Node* dummy(new Node());
            head_.store(dummy, std::memory_order_relaxed);
            tail_.store(dummy, std::memory_order_relaxed);
        }

        ~queue() {
            clear_local_hazard_pointers();
            clear_live_nodes();
            clear_local_retired_nodes();
        }

        queue(const queue&) = delete;
        auto operator=(const queue&) -> queue& = delete;

        void push(const T& value) {
            emplace(value);
        }

        void push(T&& value) {
            emplace(std::move(value));
        }

        template <typename InputIt> void push_range(InputIt first, InputIt last) {
            for (; first != last; ++first) {
                emplace(*first);
            }
        }

        template <typename... Args> void emplace(Args&&... args) {
            Node* new_node(new Node(std::forward<Args>(args)...));
            HazardRecord* hazard_tail(acquire_hazard(0));

            while (true) {
                Node* tail(tail_.load(std::memory_order_acquire));
                hazard_tail->pointer.store(tail, std::memory_order_release);

                if (tail != tail_.load(std::memory_order_acquire)) {
                    continue;
                }

                Node* next(tail->next.load(std::memory_order_acquire));

                if (tail != tail_.load(std::memory_order_acquire)) {
                    continue;
                }

                if (next == nullptr) {
                    Node* expected = nullptr;

                    if (tail->next.compare_exchange_weak(
                                expected,
                                new_node,
                                std::memory_order_release,
                                std::memory_order_relaxed
                        )) {
                        tail_.compare_exchange_strong(
                                tail,
                                new_node,
                                std::memory_order_release,
                                std::memory_order_relaxed
                        );
                        size_.fetch_add(1, std::memory_order_relaxed);
                        maybe_clear_local_hazard_pointers();
                        return;
                    }
                }
                else {
                    tail_.compare_exchange_weak(
                            tail,
                            next,
                            std::memory_order_release,
                            std::memory_order_relaxed
                    );
                }
            }
        }

        [[nodiscard]] auto pop() -> std::optional<T> {
            HazardRecord* hazard_head(acquire_hazard(0));
            HazardRecord* hazard_next(acquire_hazard(1));

            while (true) {
                Node* head(head_.load(std::memory_order_acquire));
                hazard_head->pointer.store(head, std::memory_order_release);

                if (head != head_.load(std::memory_order_acquire)) {
                    continue;
                }

                Node* next(head->next.load(std::memory_order_acquire));
                hazard_next->pointer.store(next, std::memory_order_release);

                if (head != head_.load(std::memory_order_acquire)) {
                    continue;
                }

                if (next == nullptr) {
                    maybe_clear_local_hazard_pointers();
                    return std::nullopt;
                }

                Node* tail(tail_.load(std::memory_order_acquire));
                if (head == tail) {
                    tail_.compare_exchange_weak(
                            tail,
                            next,
                            std::memory_order_release,
                            std::memory_order_relaxed
                    );
                    continue;
                }

                if (head_.compare_exchange_weak(
                            head,
                            next,
                            std::memory_order_acq_rel,
                            std::memory_order_acquire
                    )) {
                    size_.fetch_sub(1, std::memory_order_relaxed);
                    std::optional<T> result(std::move(*(next->value)));
                    maybe_clear_local_hazard_pointers();

                    retire_node(head);

                    return result;
                }
            }
        }

        [[nodiscard]] auto front() const -> std::optional<T> {
            HazardRecord* hazard_head(acquire_hazard(0));
            HazardRecord* hazard_next(acquire_hazard(1));

            while (true) {
                Node* head(head_.load(std::memory_order_acquire));
                hazard_head->pointer.store(head, std::memory_order_release);

                if (head != head_.load(std::memory_order_acquire)) {
                    continue;
                }

                Node* next(head->next.load(std::memory_order_acquire));
                hazard_next->pointer.store(next, std::memory_order_release);

                if (head != head_.load(std::memory_order_acquire)) {
                    continue;
                }

                if (next == nullptr) {
                    maybe_clear_local_hazard_pointers();
                    return std::nullopt;
                }

                std::optional<T> result(*(next->value));
                maybe_clear_local_hazard_pointers();
                return result;
            }
        }

        [[nodiscard]] auto back() const -> std::optional<T> {
            HazardRecord* hazard_curr(acquire_hazard(0));
            HazardRecord* hazard_next(acquire_hazard(1));

            while (true) {
                Node* head(head_.load(std::memory_order_acquire));
                hazard_curr->pointer.store(head, std::memory_order_release);

                if (head != head_.load(std::memory_order_acquire)) {
                    continue;
                }

                Node* current(head->next.load(std::memory_order_acquire));
                hazard_next->pointer.store(current, std::memory_order_release);

                if (head != head_.load(std::memory_order_acquire)) {
                    continue;
                }

                if (current == nullptr) {
                    maybe_clear_local_hazard_pointers();
                    return std::nullopt;
                }

                hazard_curr->pointer.store(current, std::memory_order_release);
                hazard_next->pointer.store(nullptr, std::memory_order_release);

                while (true) {
                    Node* next(current->next.load(std::memory_order_acquire));

                    if (next == nullptr) {
                        std::optional<T> result(*(current->value));
                        maybe_clear_local_hazard_pointers();
                        return result;
                    }

                    hazard_next->pointer.store(next, std::memory_order_release);

                    if (current->next.load(std::memory_order_acquire) != next) {
                        continue;
                    }

                    current = next;
                    hazard_curr->pointer.store(current, std::memory_order_release);
                    hazard_next->pointer.store(nullptr, std::memory_order_release);
                }
            }
        }

        [[nodiscard]] auto empty() const noexcept -> bool {
            return size_.load(std::memory_order_acquire) == 0;
        }

        [[nodiscard]] auto size() const noexcept -> size_t {
            return size_.load(std::memory_order_acquire);
        }
    };

    template <typename T>
    typename queue<T>::HazardRecord queue<T>::hazard_records_[queue<T>::k_max_hazard_pointers];
    template <typename T>
    thread_local std::array<typename queue<T>::HazardRecord*, queue<T>::k_local_hazard_slots>
            queue<T>::local_hazards_{};
    template <typename T> thread_local typename queue<T>::HazardReleaser queue<T>::hazard_releaser_;
    template <typename T> thread_local size_t queue<T>::hazard_ops_since_clear_{0};
    template <typename T> thread_local std::vector<typename queue<T>::Node*> queue<T>::retire_list_;

} // namespace seraph
