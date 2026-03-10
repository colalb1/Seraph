#include "seraph/queue.hpp"
#include "seraph/ringbuffer.hpp"
#include "seraph/stack.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
    enum class OpKind : std::uint8_t {
        push,
        pop,
    };

    struct PlannedOp {
        OpKind kind{OpKind::push};
        int value{0};
    };

    struct OpRecord {
        int id{0};
        int thread_id{0};
        OpKind kind{OpKind::push};
        int push_value{0};
        std::optional<int> pop_result{};
        std::uint64_t start_tick{0};
        std::uint64_t end_tick{0};
    };

    struct QueueSpec {
        using State = std::deque<int>;

        [[nodiscard]] static auto initial() -> State {
            return {};
        }

        [[nodiscard]] static auto apply(const OpRecord& operation, State& state) -> bool {
            if (operation.kind == OpKind::push) {
                state.push_back(operation.push_value);
                return true;
            }

            std::optional<int> expected;
            if (!state.empty()) {
                expected = state.front();
                state.pop_front();
            }
            return expected == operation.pop_result;
        }
    };

    struct StackSpec {
        using State = std::vector<int>;

        [[nodiscard]] static auto initial() -> State {
            return {};
        }

        [[nodiscard]] static auto apply(const OpRecord& operation, State& state) -> bool {
            if (operation.kind == OpKind::push) {
                state.push_back(operation.push_value);
                return true;
            }

            std::optional<int> expected;
            if (!state.empty()) {
                expected = state.back();
                state.pop_back();
            }
            return expected == operation.pop_result;
        }
    };

    template <typename Spec>
    [[nodiscard]] auto check_linearizable(const std::vector<OpRecord>& records) -> bool {
        const size_t n = records.size();
        if (n > 63) {
            return false;
        }

        std::vector<std::uint64_t> predecessors(n, 0);
        for (size_t i = 0; i < n; ++i) {
            for (size_t j = 0; j < n; ++j) {
                if (i == j) {
                    continue;
                }
                if (records[i].end_tick < records[j].start_tick) {
                    predecessors[j] |= (1ULL << i);
                }
            }
        }

        const std::uint64_t full_mask = (n == 64) ? ~0ULL : ((1ULL << n) - 1ULL);

        const auto search = [&](auto&& self, std::uint64_t placed, typename Spec::State state
                            ) -> bool {
            if (placed == full_mask) {
                return true;
            }

            for (size_t idx = 0; idx < n; ++idx) {
                const std::uint64_t bit = (1ULL << idx);
                if ((placed & bit) != 0) {
                    continue;
                }
                if ((predecessors[idx] & ~placed) != 0) {
                    continue;
                }

                typename Spec::State next_state(state);
                if (!Spec::apply(records[idx], next_state)) {
                    continue;
                }

                if (self(self, placed | bit, std::move(next_state))) {
                    return true;
                }
            }

            return false;
        };

        return search(search, 0ULL, Spec::initial());
    }

    template <typename Adapter, typename Spec>
    [[nodiscard]] auto run_linearizability_suite(
            std::string_view suite_name,
            std::uint64_t seed_base,
            int trials,
            int thread_count,
            int ops_per_thread
    ) -> bool {
        const int total_ops = thread_count * ops_per_thread;

        for (int trial = 0; trial < trials; ++trial) {
            std::mt19937_64 rng(seed_base + static_cast<std::uint64_t>(trial));
            std::bernoulli_distribution push_choice(0.60);

            std::vector<std::vector<PlannedOp>> plan(static_cast<size_t>(thread_count));
            for (auto& per_thread : plan) {
                per_thread.reserve(static_cast<size_t>(ops_per_thread));
            }

            int next_push_value = trial * 1000;
            for (int tid = 0; tid < thread_count; ++tid) {
                for (int operation = 0; operation < ops_per_thread; ++operation) {
                    bool is_push = push_choice(rng);
                    if (tid == 0 && operation == 0) {
                        is_push = true;
                    }

                    PlannedOp planned;
                    planned.kind = is_push ? OpKind::push : OpKind::pop;
                    planned.value = is_push ? next_push_value++ : 0;
                    plan[static_cast<size_t>(tid)].push_back(planned);
                }
            }

            Adapter data_structure;
            std::barrier sync_start(thread_count + 1);
            std::atomic<std::uint64_t> tick{0};
            std::vector<OpRecord> records(static_cast<size_t>(total_ops));
            std::vector<std::thread> workers;
            workers.reserve(static_cast<size_t>(thread_count));

            for (int tid = 0; tid < thread_count; ++tid) {
                workers.emplace_back([&, tid]() {
                    sync_start.arrive_and_wait();

                    const auto& ops = plan[static_cast<size_t>(tid)];
                    for (int operation = 0; operation < ops_per_thread; ++operation) {
                        const PlannedOp& planned = ops[static_cast<size_t>(operation)];
                        const int index = tid * ops_per_thread + operation;

                        OpRecord rec;
                        rec.id = index;
                        rec.thread_id = tid;
                        rec.kind = planned.kind;
                        rec.push_value = planned.value;
                        rec.start_tick = tick.fetch_add(1, std::memory_order_relaxed);

                        if (planned.kind == OpKind::push) {
                            data_structure.push(planned.value);
                        }
                        else {
                            rec.pop_result = data_structure.pop();
                        }

                        rec.end_tick = tick.fetch_add(1, std::memory_order_relaxed);
                        records[static_cast<size_t>(index)] = rec;
                    }
                });
            }

            sync_start.arrive_and_wait();
            for (auto& worker : workers) {
                worker.join();
            }

            if (!check_linearizable<Spec>(records)) {
                std::cerr << "Linearizability failure in " << suite_name << " trial " << trial
                          << ".\n";
                std::vector<OpRecord> ordered(records);
                std::sort(
                        ordered.begin(),
                        ordered.end(),
                        [](const OpRecord& lhs, const OpRecord& rhs) {
                            return lhs.start_tick < rhs.start_tick;
                        }
                );
                for (const OpRecord& rec : ordered) {
                    std::cerr << "  t" << rec.thread_id << " [" << rec.start_tick << ", "
                              << rec.end_tick << "] ";
                    if (rec.kind == OpKind::push) {
                        std::cerr << "push(" << rec.push_value << ")";
                    }
                    else if (rec.pop_result.has_value()) {
                        std::cerr << "pop() -> " << *rec.pop_result;
                    }
                    else {
                        std::cerr << "pop() -> nullopt";
                    }
                    std::cerr << "\n";
                }
                return false;
            }
        }

        std::cout << "[PASS] " << suite_name << " linearizability (" << trials << " histories)\n";
        return true;
    }

    class QueueAdapter {
      public:
        void push(int value) {
            queue_.push(value);
        }

        [[nodiscard]] auto pop() -> std::optional<int> {
            return queue_.pop();
        }

      private:
        seraph::queue<int> queue_;
    };

    class StackAdapter {
      public:
        void push(int value) {
            stack_.push(value);
        }

        [[nodiscard]] auto pop() -> std::optional<int> {
            return stack_.pop();
        }

      private:
        seraph::stack<int> stack_;
    };

    class RingBufferAdapter {
      public:
        RingBufferAdapter() : ring_(128) {}

        void push(int value) {
            ring_.push(value);
        }

        [[nodiscard]] auto pop() -> std::optional<int> {
            return ring_.pop();
        }

      private:
        seraph::RingBuffer<int> ring_;
    };
} // namespace

int main(int argc, char** argv) {
    int trials = 80;
    int thread_count = 3;
    int ops_per_thread = 3;

    for (int argi = 1; argi < argc; ++argi) {
        const std::string arg(argv[argi]);
        if (arg == "--quick") {
            trials = 20;
            continue;
        }
        if (arg.rfind("--trials=", 0) == 0) {
            trials = std::stoi(arg.substr(9));
            continue;
        }
        if (arg.rfind("--threads=", 0) == 0) {
            thread_count = std::stoi(arg.substr(10));
            continue;
        }
        if (arg.rfind("--ops=", 0) == 0) {
            ops_per_thread = std::stoi(arg.substr(6));
            continue;
        }
    }

    if (trials <= 0 || thread_count <= 0 || ops_per_thread <= 0) {
        std::cerr << "Invalid arguments: trials/threads/ops must be positive.\n";
        return 2;
    }

    const int total_ops = thread_count * ops_per_thread;
    if (total_ops > 63) {
        std::cerr << "This checker supports at most 63 operations per history.\n";
        return 2;
    }

    std::cout << "Running linearizability model-checking histories: trials=" << trials
              << ", threads=" << thread_count << ", ops/thread=" << ops_per_thread << "\n";

    if (!run_linearizability_suite<StackAdapter, StackSpec>(
                "stack",
                0xA11CE000ULL,
                trials,
                thread_count,
                ops_per_thread
        )) {
        return 1;
    }

    if (!run_linearizability_suite<QueueAdapter, QueueSpec>(
                "queue",
                0xBEEFA000ULL,
                trials,
                thread_count,
                ops_per_thread
        )) {
        return 1;
    }

    if (!run_linearizability_suite<RingBufferAdapter, QueueSpec>(
                "ringbuffer",
                0xC0FFEE00ULL,
                trials,
                thread_count,
                ops_per_thread
        )) {
        return 1;
    }

    return 0;
}
