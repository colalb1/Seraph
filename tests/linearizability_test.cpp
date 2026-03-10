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
        front,
        back,
        top,
        empty,
        size,
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
        std::optional<int> read_result{};
        std::optional<size_t> size_result{};
        std::optional<bool> empty_result{};
        std::uint64_t start_tick{0};
        std::uint64_t end_tick{0};
    };

    struct QueueSpec {
        using State = std::deque<int>;

        [[nodiscard]] static auto initial() -> State {
            return {};
        }

        [[nodiscard]] static auto apply(const OpRecord& operation, State& state) -> bool {
            switch (operation.kind) {
            case OpKind::push:
                state.push_back(operation.push_value);
                return true;
            case OpKind::pop: {
                std::optional<int> expected;
                if (!state.empty()) {
                    expected = state.front();
                    state.pop_front();
                }
                return expected == operation.pop_result;
            }
            case OpKind::front: {
                std::optional<int> expected;
                if (!state.empty()) {
                    expected = state.front();
                }
                return expected == operation.read_result;
            }
            case OpKind::back: {
                std::optional<int> expected;
                if (!state.empty()) {
                    expected = state.back();
                }
                return expected == operation.read_result;
            }
            case OpKind::empty:
                return operation.empty_result.has_value() &&
                       *operation.empty_result == state.empty();
            case OpKind::size:
                return operation.size_result.has_value() && *operation.size_result == state.size();
            case OpKind::top:
                return false;
            }
            return false;
        }
    };

    struct StackSpec {
        using State = std::vector<int>;

        [[nodiscard]] static auto initial() -> State {
            return {};
        }

        [[nodiscard]] static auto apply(const OpRecord& operation, State& state) -> bool {
            switch (operation.kind) {
            case OpKind::push:
                state.push_back(operation.push_value);
                return true;
            case OpKind::pop: {
                std::optional<int> expected;
                if (!state.empty()) {
                    expected = state.back();
                    state.pop_back();
                }
                return expected == operation.pop_result;
            }
            case OpKind::top: {
                std::optional<int> expected;
                if (!state.empty()) {
                    expected = state.back();
                }
                return expected == operation.read_result;
            }
            case OpKind::empty:
                return operation.empty_result.has_value() &&
                       *operation.empty_result == state.empty();
            case OpKind::size:
                return operation.size_result.has_value() && *operation.size_result == state.size();
            case OpKind::front:
            case OpKind::back:
                return false;
            }
            return false;
        }
    };

    struct RingBufferBestEffortSpec {
        using State = std::deque<int>;

        [[nodiscard]] static auto initial() -> State {
            return {};
        }

        // RingBuffer front/back are best-effort. Returning nullopt is always allowed.
        [[nodiscard]] static auto apply(const OpRecord& operation, State& state) -> bool {
            switch (operation.kind) {
            case OpKind::push:
                state.push_back(operation.push_value);
                return true;
            case OpKind::pop: {
                std::optional<int> expected;
                if (!state.empty()) {
                    expected = state.front();
                    state.pop_front();
                }
                return expected == operation.pop_result;
            }
            case OpKind::front: {
                if (!operation.read_result.has_value()) {
                    return true;
                }
                return !state.empty() && *operation.read_result == state.front();
            }
            case OpKind::back: {
                if (!operation.read_result.has_value()) {
                    return true;
                }
                return !state.empty() && *operation.read_result == state.back();
            }
            case OpKind::empty:
                return operation.empty_result.has_value() &&
                       *operation.empty_result == state.empty();
            case OpKind::size:
                return operation.size_result.has_value() && *operation.size_result == state.size();
            case OpKind::top:
                return false;
            }
            return false;
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

    template <typename Adapter, typename Spec, typename AdapterFactory>
    [[nodiscard]] auto run_linearizability_suite(
            std::string_view suite_name,
            std::uint64_t seed_base,
            int trials,
            int thread_count,
            int ops_per_thread,
            const std::vector<OpKind>& allowed_ops,
            AdapterFactory&& make_adapter
    ) -> bool {
        const int total_ops = thread_count * ops_per_thread;

        for (int trial = 0; trial < trials; ++trial) {
            std::mt19937_64 rng(seed_base + static_cast<std::uint64_t>(trial));
            std::vector<std::vector<PlannedOp>> plan(static_cast<size_t>(thread_count));
            for (auto& per_thread : plan) {
                per_thread.reserve(static_cast<size_t>(ops_per_thread));
            }

            int next_push_value = trial * 1000;
            std::uniform_int_distribution<size_t> op_picker(0, allowed_ops.size() - 1);
            for (int tid = 0; tid < thread_count; ++tid) {
                for (int operation = 0; operation < ops_per_thread; ++operation) {
                    PlannedOp planned;
                    planned.kind = allowed_ops[op_picker(rng)];
                    if (planned.kind == OpKind::push) {
                        planned.value = next_push_value++;
                    }
                    else {
                        planned.value = 0;
                    }
                    plan[static_cast<size_t>(tid)].push_back(planned);
                }
            }

            // Ensure at least one push exists so the history is not trivially empty.
            plan.front().front().kind = OpKind::push;
            plan.front().front().value = next_push_value++;

            Adapter data_structure = make_adapter();
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

                        switch (planned.kind) {
                        case OpKind::push:
                            data_structure.push(planned.value);
                            break;
                        case OpKind::pop:
                            rec.pop_result = data_structure.pop();
                            break;
                        case OpKind::front:
                            rec.read_result = data_structure.front();
                            break;
                        case OpKind::back:
                            rec.read_result = data_structure.back();
                            break;
                        case OpKind::top:
                            rec.read_result = data_structure.top();
                            break;
                        case OpKind::empty:
                            rec.empty_result = data_structure.empty();
                            break;
                        case OpKind::size:
                            rec.size_result = data_structure.size();
                            break;
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
                    switch (rec.kind) {
                    case OpKind::push:
                        std::cerr << "push(" << rec.push_value << ")";
                        break;
                    case OpKind::pop:
                        if (rec.pop_result.has_value()) {
                            std::cerr << "pop() -> " << *rec.pop_result;
                        }
                        else {
                            std::cerr << "pop() -> nullopt";
                        }
                        break;
                    case OpKind::front:
                        if (rec.read_result.has_value()) {
                            std::cerr << "front() -> " << *rec.read_result;
                        }
                        else {
                            std::cerr << "front() -> nullopt";
                        }
                        break;
                    case OpKind::back:
                        if (rec.read_result.has_value()) {
                            std::cerr << "back() -> " << *rec.read_result;
                        }
                        else {
                            std::cerr << "back() -> nullopt";
                        }
                        break;
                    case OpKind::top:
                        if (rec.read_result.has_value()) {
                            std::cerr << "top() -> " << *rec.read_result;
                        }
                        else {
                            std::cerr << "top() -> nullopt";
                        }
                        break;
                    case OpKind::empty:
                        std::cerr << "empty() -> "
                                  << (rec.empty_result.value_or(false) ? "true" : "false");
                        break;
                    case OpKind::size:
                        std::cerr << "size() -> " << rec.size_result.value_or(0);
                        break;
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

        [[nodiscard]] auto front() -> std::optional<int> {
            return queue_.front();
        }

        [[nodiscard]] auto back() -> std::optional<int> {
            return queue_.back();
        }

        // Queue has no top(); keep an explicit stub so shared test code compiles cleanly.
        [[nodiscard]] auto top() -> std::optional<int> {
            return std::nullopt;
        }

        [[nodiscard]] auto empty() -> bool {
            return queue_.empty();
        }

        [[nodiscard]] auto size() -> size_t {
            return queue_.size();
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

        [[nodiscard]] auto top() -> std::optional<int> {
            return stack_.top();
        }

        // Stack has no front/back; keep explicit stubs so shared test code compiles cleanly.
        [[nodiscard]] auto front() -> std::optional<int> {
            return std::nullopt;
        }

        [[nodiscard]] auto back() -> std::optional<int> {
            return std::nullopt;
        }

        [[nodiscard]] auto empty() -> bool {
            return stack_.empty();
        }

        [[nodiscard]] auto size() -> size_t {
            return stack_.size();
        }

      private:
        seraph::stack<int> stack_;
    };

    class RingBufferAdapter {
      public:
        explicit RingBufferAdapter(size_t capacity) : ring_(capacity) {}

        void push(int value) {
            ring_.push(value);
        }

        [[nodiscard]] auto pop() -> std::optional<int> {
            return ring_.pop();
        }

        [[nodiscard]] auto front() -> std::optional<int> {
            return ring_.front();
        }

        [[nodiscard]] auto back() -> std::optional<int> {
            return ring_.back();
        }

        // RingBuffer has no top(); keep an explicit stub so shared test code compiles cleanly.
        [[nodiscard]] auto top() -> std::optional<int> {
            return std::nullopt;
        }

        [[nodiscard]] auto empty() -> bool {
            return ring_.empty();
        }

        [[nodiscard]] auto size() -> size_t {
            return ring_.size();
        }

      private:
        seraph::RingBuffer<int> ring_;
    };

    bool run_sequential_sanity() {
        {
            QueueAdapter queue;
            if (!queue.empty() || queue.size() != 0) {
                return false;
            }
            if (queue.front().has_value() || queue.back().has_value()) {
                return false;
            }

            queue.push(1);
            queue.push(2);
            if (queue.front() != 1 || queue.back() != 2) {
                return false;
            }
            if (queue.size() != 2 || queue.empty()) {
                return false;
            }

            if (queue.pop() != 1) {
                return false;
            }
            if (queue.front() != 2 || queue.back() != 2) {
                return false;
            }
            if (queue.size() != 1) {
                return false;
            }

            if (queue.pop() != 2) {
                return false;
            }
            if (!queue.empty() || queue.size() != 0) {
                return false;
            }
        }

        {
            StackAdapter stack;
            if (!stack.empty() || stack.size() != 0) {
                return false;
            }
            if (stack.top().has_value()) {
                return false;
            }

            stack.push(7);
            stack.push(9);
            if (stack.top() != 9 || stack.size() != 2) {
                return false;
            }

            if (stack.pop() != 9) {
                return false;
            }
            if (stack.top() != 7 || stack.size() != 1) {
                return false;
            }

            if (stack.pop() != 7) {
                return false;
            }
            if (!stack.empty() || stack.size() != 0) {
                return false;
            }
        }

        {
            RingBufferAdapter ringbuffer(4);
            if (!ringbuffer.empty() || ringbuffer.size() != 0) {
                return false;
            }
            if (ringbuffer.front().has_value() || ringbuffer.back().has_value()) {
                return false;
            }

            ringbuffer.push(10);
            ringbuffer.push(11);
            if (ringbuffer.front() != 10 || ringbuffer.back() != 11) {
                return false;
            }
            if (ringbuffer.size() != 2 || ringbuffer.empty()) {
                return false;
            }

            if (ringbuffer.pop() != 10) {
                return false;
            }
            if (ringbuffer.front() != 11 || ringbuffer.back() != 11) {
                return false;
            }
            if (ringbuffer.size() != 1) {
                return false;
            }

            if (ringbuffer.pop() != 11) {
                return false;
            }
            if (!ringbuffer.empty() || ringbuffer.size() != 0) {
                return false;
            }
        }

        return true;
    }

    template <typename Adapter, typename Spec, typename AdapterFactory>
    [[nodiscard]] auto run_phased_history(
            std::string_view suite_name,
            const std::vector<std::vector<PlannedOp>>& plan,
            AdapterFactory&& make_adapter
    ) -> bool {
        const int thread_count = static_cast<int>(plan.size());
        if (thread_count == 0) {
            return false;
        }

        const int ops_per_thread = static_cast<int>(plan.front().size());
        const int total_ops = thread_count * ops_per_thread;
        if (total_ops > 63) {
            std::cerr << "Planned history too large for checker.\n";
            return false;
        }

        Adapter data_structure = make_adapter();
        std::barrier sync_start(thread_count + 1);
        std::barrier phase_sync(thread_count);
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

                    switch (planned.kind) {
                    case OpKind::push:
                        data_structure.push(planned.value);
                        break;
                    case OpKind::pop:
                        rec.pop_result = data_structure.pop();
                        break;
                    case OpKind::front:
                        rec.read_result = data_structure.front();
                        break;
                    case OpKind::back:
                        rec.read_result = data_structure.back();
                        break;
                    case OpKind::top:
                        rec.read_result = data_structure.top();
                        break;
                    case OpKind::empty:
                        rec.empty_result = data_structure.empty();
                        break;
                    case OpKind::size:
                        rec.size_result = data_structure.size();
                        break;
                    }

                    rec.end_tick = tick.fetch_add(1, std::memory_order_relaxed);
                    records[static_cast<size_t>(index)] = rec;

                    // Phase barrier keeps push/pop balanced so blocking pushes can progress.
                    phase_sync.arrive_and_wait();
                }
            });
        }

        sync_start.arrive_and_wait();
        for (auto& worker : workers) {
            worker.join();
        }

        if (!check_linearizable<Spec>(records)) {
            std::cerr << "Linearizability failure in " << suite_name << ".\n";
            return false;
        }

        std::cout << "[PASS] " << suite_name << " linearizability (phased history)\n";
        return true;
    }
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

    if (!run_sequential_sanity()) {
        std::cerr << "Sequential sanity checks failed.\n";
        return 1;
    }
    std::cout << "[PASS] sequential observer sanity checks\n";

    const std::vector<OpKind> stack_ops = {
            OpKind::push,
            OpKind::pop,
    };
    if (!run_linearizability_suite<StackAdapter, StackSpec>(
                "stack",
                0xA11CE000ULL,
                trials,
                thread_count,
                ops_per_thread,
                stack_ops,
                []() {
                    return StackAdapter();
                }
        )) {
        return 1;
    }

    const std::vector<OpKind> queue_ops = {
            OpKind::push,
            OpKind::pop,
    };
    if (!run_linearizability_suite<QueueAdapter, QueueSpec>(
                "queue",
                0xBEEFA000ULL,
                trials,
                thread_count,
                ops_per_thread,
                queue_ops,
                []() {
                    return QueueAdapter();
                }
        )) {
        return 1;
    }

    const std::vector<OpKind> ringbuffer_ops = {
            OpKind::push,
            OpKind::pop,
    };
    if (!run_linearizability_suite<RingBufferAdapter, RingBufferBestEffortSpec>(
                "ringbuffer",
                0xC0FFEE00ULL,
                trials,
                thread_count,
                ops_per_thread,
                ringbuffer_ops,
                []() {
                    return RingBufferAdapter(128);
                }
        )) {
        return 1;
    }

    // Targeted full/empty contention scenarios for RingBuffer with a tiny capacity.
    {
        std::vector<std::vector<PlannedOp>> plan = {
                {
                        {OpKind::push, 1},
                        {OpKind::push, 2},
                        {OpKind::push, 3},
                },
                {
                        {OpKind::pop, 0},
                        {OpKind::pop, 0},
                        {OpKind::pop, 0},
                },
        };

        if (!run_phased_history<RingBufferAdapter, RingBufferBestEffortSpec>(
                    "ringbuffer_full_then_drain",
                    plan,
                    []() {
                        return RingBufferAdapter(2);
                    }
            )) {
            return 1;
        }
    }

    {
        std::vector<std::vector<PlannedOp>> plan = {
                {
                        {OpKind::pop, 0},
                        {OpKind::pop, 0},
                        {OpKind::push, 10},
                },
                {
                        {OpKind::push, 20},
                        {OpKind::push, 30},
                        {OpKind::pop, 0},
                },
        };

        if (!run_phased_history<RingBufferAdapter, RingBufferBestEffortSpec>(
                    "ringbuffer_empty_then_fill",
                    plan,
                    []() {
                        return RingBufferAdapter(2);
                    }
            )) {
            return 1;
        }
    }

    {
        std::vector<std::vector<PlannedOp>> plan = {
                {
                        {OpKind::push, 100},
                        {OpKind::front, 0},
                        {OpKind::push, 200},
                },
                {
                        {OpKind::back, 0},
                        {OpKind::pop, 0},
                        {OpKind::pop, 0},
                },
        };

        if (!run_phased_history<RingBufferAdapter, RingBufferBestEffortSpec>(
                    "ringbuffer_peek_under_contention",
                    plan,
                    []() {
                        return RingBufferAdapter(2);
                    }
            )) {
            return 1;
        }
    }

    return 0;
}
