#include "seraph/stack.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__has_include)
#if __has_include(<boost/lockfree/stack.hpp>)
#include <boost/lockfree/stack.hpp>
#define SERAPH_HAS_BOOST_LOCKFREE_STACK 1
#endif
#endif

#ifndef SERAPH_HAS_BOOST_LOCKFREE_STACK
#define SERAPH_HAS_BOOST_LOCKFREE_STACK 0
#endif

namespace {
    using Clock = std::chrono::steady_clock;

    struct BenchmarkSample {
        std::string implementation;
        std::string operation;
        size_t iterations;
        int repeat_index;
        double total_ns;
        double nanoseconds_per_op;
        double ops_per_second;
    };

    struct BenchmarkAggregate {
        std::string implementation;
        std::string operation;
        size_t iterations;
        int repeats;
        double avg_nanoseconds_per_op;
        double avg_ops_per_second;
        double min_nanoseconds_per_op;
        double max_nanoseconds_per_op;
    };

    volatile std::uint64_t g_sink = 0;

    class STLStackAdapter {
      public:
        void push(const int& value) {
            data_.push(value);
        }

        void push(int&& value) {
            data_.push(std::move(value));
        }

        template <typename... Args> void emplace(Args&&... args) {
            data_.emplace(std::forward<Args>(args)...);
        }

        std::optional<int> pop() {
            if (data_.empty()) {
                return std::nullopt;
            }
            int value = std::move(data_.top());
            data_.pop();
            return value;
        }

        std::optional<int> top() const {
            if (data_.empty()) {
                return std::nullopt;
            }
            return data_.top();
        }

        bool empty() const noexcept {
            return data_.empty();
        }

        size_t size() const noexcept {
            return data_.size();
        }

      private:
        std::stack<int, std::vector<int>> data_;
    };

    class ThreadSafeSTLStackAdapter {
      public:
        void push(const int& value) {
            std::lock_guard<std::mutex> guard(lock_);
            data_.push(value);
        }

        void push(int&& value) {
            std::lock_guard<std::mutex> guard(lock_);
            data_.push(std::move(value));
        }

        template <typename... Args> void emplace(Args&&... args) {
            std::lock_guard<std::mutex> guard(lock_);
            data_.emplace(std::forward<Args>(args)...);
        }

        std::optional<int> pop() {
            std::lock_guard<std::mutex> guard(lock_);
            if (data_.empty()) {
                return std::nullopt;
            }
            int value = std::move(data_.top());
            data_.pop();
            return value;
        }

        std::optional<int> top() const {
            std::lock_guard<std::mutex> guard(lock_);
            if (data_.empty()) {
                return std::nullopt;
            }
            return data_.top();
        }

        bool empty() const noexcept {
            std::lock_guard<std::mutex> guard(lock_);
            return data_.empty();
        }

        size_t size() const noexcept {
            std::lock_guard<std::mutex> guard(lock_);
            return data_.size();
        }

      private:
        mutable std::mutex lock_;
        std::stack<int, std::vector<int>> data_;
    };

#if SERAPH_HAS_BOOST_LOCKFREE_STACK
    class BoostLockfreeStackAdapter {
      public:
        BoostLockfreeStackAdapter() : data_(1024) {}

        void push(const int& value) {
            while (!data_.push(value)) {
                std::this_thread::yield();
            }
            size_.fetch_add(1, std::memory_order_relaxed);
        }

        void push(int&& value) {
            while (!data_.push(std::move(value))) {
                std::this_thread::yield();
            }
            size_.fetch_add(1, std::memory_order_relaxed);
        }

        template <typename... Args> void emplace(Args&&... args) {
            int value(std::forward<Args>(args)...);
            push(std::move(value));
        }

        std::optional<int> pop() {
            int value = 0;
            if (!data_.pop(value)) {
                return std::nullopt;
            }
            size_.fetch_sub(1, std::memory_order_relaxed);
            return value;
        }

        bool empty() const noexcept {
            return data_.empty();
        }

        size_t size() const noexcept {
            return size_.load(std::memory_order_relaxed);
        }

      private:
        boost::lockfree::stack<int> data_;
        std::atomic<size_t> size_{0};
    };
#endif

    std::filesystem::path find_repo_root() {
        std::filesystem::path current = std::filesystem::current_path();

        while (!current.empty()) {
            const auto marker = current / "include" / "seraph" / "stack.hpp";
            const auto cmake = current / "CMakeLists.txt";
            if (std::filesystem::exists(marker) && std::filesystem::exists(cmake)) {
                return current;
            }

            if (current == current.root_path()) {
                break;
            }
            current = current.parent_path();
        }

        throw std::runtime_error("Unable to find repository root from current working directory.");
    }

    template <typename Fn>
    std::vector<BenchmarkSample> run_samples(
            std::string_view impl_name,
            std::string_view operation,
            size_t iterations,
            int repeats,
            Fn&& fn
    ) {
        std::vector<BenchmarkSample> samples;
        samples.reserve(static_cast<size_t>(repeats));

        for (int repeat = 0; repeat < repeats; ++repeat) {
            const auto start = Clock::now();
            fn();
            const auto stop = Clock::now();
            const double measured_ns =
                    std::chrono::duration<double, std::nano>(stop - start).count();
            const double total_ns = std::max(1.0, measured_ns);
            const double ns_per_op = total_ns / static_cast<double>(iterations);
            const double ops_per_sec = 1e9 / ns_per_op;

            samples.push_back(BenchmarkSample{
                    .implementation = std::string(impl_name),
                    .operation = std::string(operation),
                    .iterations = iterations,
                    .repeat_index = repeat,
                    .total_ns = total_ns,
                    .nanoseconds_per_op = ns_per_op,
                    .ops_per_second = ops_per_sec,
            });
        }

        return samples;
    }

    template <typename StackType>
    std::vector<BenchmarkSample>
    bench_push_copy(std::string_view impl_name, size_t iterations, int repeats) {
        return run_samples(impl_name, "push_copy", iterations, repeats, [iterations]() {
            StackType stack;
            const int value = 42;
            for (size_t iii = 0; iii < iterations; ++iii) {
                stack.push(value);
            }
            g_sink += stack.size();
        });
    }

    template <typename StackType>
    std::vector<BenchmarkSample>
    bench_push_move(std::string_view impl_name, size_t iterations, int repeats) {
        return run_samples(impl_name, "push_move", iterations, repeats, [iterations]() {
            StackType stack;
            for (size_t iii = 0; iii < iterations; ++iii) {
                int value = static_cast<int>(iii);
                stack.push(std::move(value));
            }
            g_sink += stack.size();
        });
    }

    template <typename StackType>
    std::vector<BenchmarkSample>
    bench_emplace(std::string_view impl_name, size_t iterations, int repeats) {
        return run_samples(impl_name, "emplace", iterations, repeats, [iterations]() {
            StackType stack;
            for (size_t iii = 0; iii < iterations; ++iii) {
                stack.emplace(static_cast<int>(iii));
            }
            g_sink += stack.size();
        });
    }

    template <typename StackType>
    std::vector<BenchmarkSample>
    bench_pop(std::string_view impl_name, size_t iterations, int repeats) {
        return run_samples(impl_name, "pop", iterations, repeats, [iterations]() {
            StackType stack;
            for (size_t iii = 0; iii < iterations; ++iii) {
                stack.emplace(static_cast<int>(iii));
            }

            std::uint64_t local_sum = 0;
            for (size_t iii = 0; iii < iterations; ++iii) {
                auto value = stack.pop();
                if (value.has_value()) {
                    local_sum += static_cast<std::uint64_t>(*value);
                }
            }
            g_sink += local_sum;
        });
    }

    template <typename StackType>
    std::vector<BenchmarkSample>
    bench_size(std::string_view impl_name, size_t iterations, int repeats) {
        return run_samples(impl_name, "size", iterations, repeats, [iterations]() {
            StackType stack;
            for (size_t iii = 0; iii < 1024; ++iii) {
                stack.emplace(static_cast<int>(iii));
            }

            std::uint64_t local_sum = 0;
            for (size_t iii = 0; iii < iterations; ++iii) {
                local_sum += stack.size();
            }
            g_sink += local_sum;
        });
    }

    template <typename StackType>
    std::vector<BenchmarkSample>
    bench_empty(std::string_view impl_name, size_t iterations, int repeats) {
        return run_samples(impl_name, "empty", iterations, repeats, [iterations]() {
            StackType stack;
            stack.emplace(1);
            std::uint64_t local_sum = 0;
            for (size_t iii = 0; iii < iterations; ++iii) {
                local_sum += static_cast<std::uint64_t>(stack.empty());
            }
            g_sink += local_sum;
        });
    }

    template <typename StackType>
    std::vector<BenchmarkSample>
    bench_top(std::string_view impl_name, size_t iterations, int repeats) {
        return run_samples(impl_name, "top", iterations, repeats, [iterations]() {
            StackType stack;
            stack.emplace(7);
            std::uint64_t local_sum = 0;
            for (size_t iii = 0; iii < iterations; ++iii) {
                auto value = stack.top();
                if (value.has_value()) {
                    local_sum += static_cast<std::uint64_t>(*value);
                }
            }
            g_sink += local_sum;
        });
    }

    std::vector<BenchmarkSample> bench_reserve_stack(size_t iterations, int repeats) {
        return run_samples("stack", "reserve", iterations, repeats, [iterations]() {
            seraph::stack<int> stack;
            for (size_t iii = 1; iii <= iterations; ++iii) {
                stack.reserve(iii);
            }
            g_sink += stack.size();
        });
    }

    std::string make_contention_operation_label(int thread_count, int push_percent) {
        const int pop_percent = 100 - push_percent;
        return "contention_t" + std::to_string(thread_count) + "_push" +
               std::to_string(push_percent) + "_pop" + std::to_string(pop_percent);
    }

    template <typename StackType>
    std::vector<BenchmarkSample> bench_contention_mix(
            std::string_view impl_name,
            int thread_count,
            int push_percent,
            size_t ops_per_thread,
            int repeats
    ) {
        const size_t total_ops = static_cast<size_t>(thread_count) * ops_per_thread;
        const std::string op_label = make_contention_operation_label(thread_count, push_percent);
        return run_samples(
                impl_name,
                op_label,
                total_ops,
                repeats,
                [thread_count, push_percent, ops_per_thread]() {
                    StackType stack;
                    for (size_t iii = 0; iii < static_cast<size_t>(thread_count) * ops_per_thread;
                         ++iii) {
                        stack.emplace(static_cast<int>(iii));
                    }

                    std::barrier sync_start(thread_count + 1);
                    std::atomic<std::uint64_t> pop_sum{0};

                    std::vector<std::thread> workers;
                    workers.reserve(static_cast<size_t>(thread_count));

                    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
                        workers.emplace_back([&, thread_index]() {
                            std::uint64_t seed = 0x9e3779b97f4a7c15ULL ^
                                                 static_cast<std::uint64_t>(thread_index + 1);
                            std::uint64_t local_sum = 0;

                            sync_start.arrive_and_wait();
                            for (size_t iii = 0; iii < ops_per_thread; ++iii) {
                                seed ^= seed << 13;
                                seed ^= seed >> 7;
                                seed ^= seed << 17;

                                const int roll = static_cast<int>(seed % 100ULL);
                                if (roll < push_percent) {
                                    stack.push(static_cast<int>(
                                            iii ^ static_cast<size_t>(thread_index)
                                    ));
                                }
                                else {
                                    auto value = stack.pop();
                                    if (value.has_value()) {
                                        local_sum += static_cast<std::uint64_t>(*value);
                                    }
                                }
                            }

                            pop_sum.fetch_add(local_sum, std::memory_order_relaxed);
                        });
                    }

                    sync_start.arrive_and_wait();

                    for (auto& worker : workers) {
                        worker.join();
                    }

                    g_sink += pop_sum.load(std::memory_order_relaxed);
                }
        );
    }

    std::string make_mt_simple_operation_label(std::string_view mode, int thread_count) {
        return "mt_" + std::string(mode) + "_t" + std::to_string(thread_count);
    }

    template <typename StackType>
    std::vector<BenchmarkSample> bench_mt_push_only(
            std::string_view impl_name,
            int thread_count,
            size_t ops_per_thread,
            int repeats
    ) {
        const size_t total_ops = static_cast<size_t>(thread_count) * ops_per_thread;
        const std::string op_label = make_mt_simple_operation_label("push_only", thread_count);
        return run_samples(
                impl_name,
                op_label,
                total_ops,
                repeats,
                [thread_count, ops_per_thread]() {
                    StackType stack;
                    std::barrier sync_start(thread_count + 1);
                    std::vector<std::thread> workers;
                    workers.reserve(static_cast<size_t>(thread_count));

                    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
                        workers.emplace_back([&, thread_index]() {
                            sync_start.arrive_and_wait();
                            for (size_t iii = 0; iii < ops_per_thread; ++iii) {
                                stack.push(static_cast<int>(iii + static_cast<size_t>(thread_index))
                                );
                            }
                        });
                    }

                    sync_start.arrive_and_wait();
                    for (auto& worker : workers) {
                        worker.join();
                    }
                    g_sink += stack.size();
                }
        );
    }

    template <typename StackType>
    std::vector<BenchmarkSample> bench_mt_pop_only(
            std::string_view impl_name,
            int thread_count,
            size_t ops_per_thread,
            int repeats
    ) {
        const size_t total_ops = static_cast<size_t>(thread_count) * ops_per_thread;
        const std::string op_label = make_mt_simple_operation_label("pop_only", thread_count);
        return run_samples(
                impl_name,
                op_label,
                total_ops,
                repeats,
                [thread_count, ops_per_thread, total_ops]() {
                    StackType stack;
                    for (size_t iii = 0; iii < total_ops; ++iii) {
                        stack.emplace(static_cast<int>(iii));
                    }

                    std::barrier sync_start(thread_count + 1);
                    std::atomic<std::uint64_t> pop_sum{0};
                    std::vector<std::thread> workers;
                    workers.reserve(static_cast<size_t>(thread_count));

                    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
                        workers.emplace_back([&, thread_index]() {
                            std::uint64_t local_sum = 0;
                            sync_start.arrive_and_wait();
                            for (size_t iii = 0; iii < ops_per_thread; ++iii) {
                                auto value = stack.pop();
                                if (value.has_value()) {
                                    local_sum += static_cast<std::uint64_t>(*value);
                                }
                            }
                            pop_sum.fetch_add(local_sum, std::memory_order_relaxed);
                        });
                    }

                    sync_start.arrive_and_wait();
                    for (auto& worker : workers) {
                        worker.join();
                    }
                    g_sink += pop_sum.load(std::memory_order_relaxed);
                }
        );
    }

    std::vector<BenchmarkAggregate> build_aggregates(const std::vector<BenchmarkSample>& samples) {
        std::vector<BenchmarkAggregate> aggregates;
        std::map<std::pair<std::string, std::string>, std::vector<const BenchmarkSample*>> grouped;

        for (const auto& sample : samples) {
            grouped[{sample.implementation, sample.operation}].push_back(&sample);
        }

        for (const auto& [key, group] : grouped) {
            const auto& impl = key.first;
            const auto& op = key.second;

            double sum_ns_per_op = 0.0;
            double sum_ops_per_sec = 0.0;
            double min_ns_per_op = group.front()->nanoseconds_per_op;
            double max_ns_per_op = group.front()->nanoseconds_per_op;

            for (const auto* sample : group) {
                sum_ns_per_op += sample->nanoseconds_per_op;
                sum_ops_per_sec += sample->ops_per_second;
                min_ns_per_op = std::min(min_ns_per_op, sample->nanoseconds_per_op);
                max_ns_per_op = std::max(max_ns_per_op, sample->nanoseconds_per_op);
            }

            const double count = static_cast<double>(group.size());
            aggregates.push_back(BenchmarkAggregate{
                    .implementation = impl,
                    .operation = op,
                    .iterations = group.front()->iterations,
                    .repeats = static_cast<int>(group.size()),
                    .avg_nanoseconds_per_op = sum_ns_per_op / count,
                    .avg_ops_per_second = sum_ops_per_sec / count,
                    .min_nanoseconds_per_op = min_ns_per_op,
                    .max_nanoseconds_per_op = max_ns_per_op,
            });
        }

        return aggregates;
    }

    void write_results_csv(
            const std::vector<BenchmarkSample>& samples,
            const std::vector<BenchmarkAggregate>& aggregates,
            int repeats,
            const std::filesystem::path& output_path
    ) {
        std::ofstream out(output_path);
        out << "record_type,implementation,operation,iterations,repeats,repeat_index,total_ns,ns_"
               "per_op,ops_per_sec,min_ns_per_op,max_ns_per_op,avg_ns_per_op,avg_ops_per_sec\n";

        for (const auto& sample : samples) {
            out << "sample," << sample.implementation << "," << sample.operation << ","
                << sample.iterations << "," << repeats << "," << sample.repeat_index << ","
                << sample.total_ns << "," << sample.nanoseconds_per_op << ","
                << sample.ops_per_second << ",,,\n";
        }

        for (const auto& aggregate : aggregates) {
            out << "average," << aggregate.implementation << "," << aggregate.operation << ","
                << aggregate.iterations << "," << aggregate.repeats << ",,,,"
                << aggregate.min_nanoseconds_per_op << "," << aggregate.max_nanoseconds_per_op
                << "," << aggregate.avg_nanoseconds_per_op << "," << aggregate.avg_ops_per_second
                << "\n";
        }
    }

    std::string color_for_impl(std::string_view impl) {
        if (impl == "stack") {
            return "#2a9d8f";
        }
        if (impl == "BoostStack") {
            return "#e76f51";
        }
        return "#264653";
    }

    std::string format_metric(double value) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(value >= 100.0 ? 1 : 2) << value;
        return ss.str();
    }

    void write_svg_grouped_bars(
            const std::vector<BenchmarkAggregate>& aggregates,
            const std::filesystem::path& output_path,
            bool use_ns_metric
    ) {
        std::vector<std::string> operations;
        for (const auto& result : aggregates) {
            if (std::find(operations.begin(), operations.end(), result.operation) ==
                operations.end()) {
                operations.push_back(result.operation);
            }
        }

        std::vector<std::string> impls = {"stack"};
#if SERAPH_HAS_BOOST_LOCKFREE_STACK
        impls.push_back("BoostStack");
#endif

        std::map<std::string, std::map<std::string, double>> metric_by_op_impl;
        double max_metric = 0.0;
        for (const auto& result : aggregates) {
            const double metric =
                    use_ns_metric ? result.avg_nanoseconds_per_op : result.avg_ops_per_second;
            metric_by_op_impl[result.operation][result.implementation] = metric;
            max_metric = std::max(max_metric, metric);
        }

        const int width = 1280;
        const int height = 720;
        const int margin_left = 90;
        const int margin_right = 40;
        const int margin_top = 80;
        const int margin_bottom = 170;
        const double plot_w = static_cast<double>(width - margin_left - margin_right);
        const double plot_h = static_cast<double>(height - margin_top - margin_bottom);
        const double group_w = plot_w / static_cast<double>(operations.size());
        const double bar_w = group_w / 5.0;

        std::ofstream out(output_path);
        out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width << "\" height=\""
            << height << "\" viewBox=\"0 0 " << width << " " << height << "\">\n";
        out << "<rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height
            << "\" fill=\"#ffffff\"/>\n";
        out << "<text x=\"" << width / 2
            << "\" y=\"40\" text-anchor=\"middle\" font-size=\"26\" font-family=\"Menlo, "
               "monospace\" fill=\"#111111\">stack Performance Average: "
            << (use_ns_metric ? "ns/op (lower is better)" : "ops/sec (higher is better)")
            << "</text>\n";
        out << "<text x=\"28\" y=\"" << (margin_top + plot_h / 2.0)
            << "\" text-anchor=\"middle\" font-size=\"13\" font-family=\"Menlo, monospace\" "
               "fill=\"#222222\" transform=\"rotate(-90 28 "
            << (margin_top + plot_h / 2.0) << ")\">" << (use_ns_metric ? "ns/op" : "ops/sec")
            << "</text>\n";
        out << "<text x=\"" << (margin_left + plot_w / 2.0) << "\" y=\"" << (height - 18)
            << "\" text-anchor=\"middle\" font-size=\"13\" font-family=\"Menlo, monospace\" "
               "fill=\"#222222\">operation</text>\n";

        for (int tick = 0; tick <= 5; ++tick) {
            const double ratio = static_cast<double>(tick) / 5.0;
            const double y = margin_top + plot_h - ratio * plot_h;
            const double value = ratio * max_metric;
            out << "<line x1=\"" << margin_left << "\" y1=\"" << y << "\" x2=\""
                << (width - margin_right) << "\" y2=\"" << y
                << "\" stroke=\"#e0e0e0\" stroke-width=\"1\"/>\n";
            out << "<text x=\"" << (margin_left - 10) << "\" y=\"" << (y + 4)
                << "\" text-anchor=\"end\" font-size=\"12\" font-family=\"Menlo, monospace\" "
                   "fill=\"#444444\">"
                << format_metric(value) << "</text>\n";
        }

        out << "<line x1=\"" << margin_left << "\" y1=\"" << margin_top << "\" x2=\"" << margin_left
            << "\" y2=\"" << (height - margin_bottom)
            << "\" stroke=\"#222222\" stroke-width=\"2\"/>\n";
        out << "<line x1=\"" << margin_left << "\" y1=\"" << (height - margin_bottom) << "\" x2=\""
            << (width - margin_right) << "\" y2=\"" << (height - margin_bottom)
            << "\" stroke=\"#222222\" stroke-width=\"2\"/>\n";

        for (size_t op_idx = 0; op_idx < operations.size(); ++op_idx) {
            const std::string& op = operations[op_idx];
            const double group_start = margin_left + static_cast<double>(op_idx) * group_w;
            const double center = group_start + group_w / 2.0;

            const auto op_it = metric_by_op_impl.find(op);
            if (op_it == metric_by_op_impl.end()) {
                continue;
            }

            std::vector<std::string> present_impls;
            for (const auto& impl : impls) {
                if (op_it->second.contains(impl)) {
                    present_impls.push_back(impl);
                }
            }

            for (size_t impl_idx = 0; impl_idx < present_impls.size(); ++impl_idx) {
                const std::string& impl = present_impls[impl_idx];
                const auto impl_it = op_it->second.find(impl);
                const double metric = impl_it->second;
                const double ratio = (max_metric > 0.0) ? (metric / max_metric) : 0.0;
                const double bar_h = ratio * plot_h;
                const double offset =
                        (static_cast<double>(impl_idx) - (present_impls.size() - 1) / 2.0) *
                        (bar_w + 8.0);
                const double x = center + offset - bar_w / 2.0;
                const double y = margin_top + plot_h - bar_h;

                out << "<rect x=\"" << x << "\" y=\"" << y << "\" width=\"" << bar_w
                    << "\" height=\"" << bar_h << "\" fill=\"" << color_for_impl(impl) << "\"/>\n";
                out << "<text x=\"" << (x + bar_w / 2.0) << "\" y=\"" << (y - 6)
                    << "\" text-anchor=\"middle\" font-size=\"10\" font-family=\"Menlo, "
                       "monospace\" fill=\"#222222\">"
                    << format_metric(metric) << "</text>\n";
            }

            const double label_y = height - margin_bottom + 20;
            out << "<text x=\"" << center << "\" y=\"" << label_y
                << "\" text-anchor=\"middle\" font-size=\"12\" font-family=\"Menlo, "
                   "monospace\" fill=\"#222222\" transform=\"rotate(28 "
                << center << " " << label_y << ")\">" << op << "</text>\n";
        }

        const int legend_y = 60;
        int legend_x = 720;
        for (const auto& impl : impls) {
            out << "<rect x=\"" << legend_x << "\" y=\"" << (legend_y - 12)
                << "\" width=\"16\" height=\"16\" fill=\"" << color_for_impl(impl) << "\"/>\n";
            out << "<text x=\"" << (legend_x + 24) << "\" y=\"" << legend_y
                << "\" font-size=\"14\" font-family=\"Menlo, monospace\" fill=\"#222222\">" << impl
                << "</text>\n";
            legend_x += 170;
        }
        out << "</svg>\n";
    }

    struct ContentionSeriesPoint {
        int thread_count;
        double avg_ops_per_second;
    };

    bool parse_contention_op(
            std::string_view operation,
            int& thread_count,
            int& push_percent,
            int& pop_percent
    ) {
        const std::string prefix = "contention_t";
        if (!operation.starts_with(prefix)) {
            return false;
        }

        const size_t push_pos = operation.find("_push");
        const size_t pop_pos = operation.find("_pop");
        if (push_pos == std::string::npos || pop_pos == std::string::npos || pop_pos <= push_pos) {
            return false;
        }

        try {
            thread_count =
                    std::stoi(std::string(operation.substr(prefix.size(), push_pos - prefix.size()))
                    );
            push_percent =
                    std::stoi(std::string(operation.substr(push_pos + 5, pop_pos - (push_pos + 5)))
                    );
            pop_percent = std::stoi(std::string(operation.substr(pop_pos + 4)));
        }
        catch (...) {
            return false;
        }
        return true;
    }

    std::string color_for_series_index(size_t index) {
        static const std::vector<std::string> palette = {
                "#1d3557",
                "#e76f51",
                "#2a9d8f",
                "#f4a261",
                "#6a4c93",
                "#1982c4",
                "#8ac926",
                "#ff595e",
        };
        return palette[index % palette.size()];
    }

    void write_contention_svg(
            const std::vector<BenchmarkAggregate>& aggregates,
            const std::filesystem::path& output_path
    ) {
        std::map<std::string, std::vector<ContentionSeriesPoint>> series;
        std::vector<int> thread_counts;
        double max_ops = 0.0;

        for (const auto& aggregate : aggregates) {
            int thread_count = 0;
            int push_percent = 0;
            int pop_percent = 0;
            if (!parse_contention_op(
                        aggregate.operation,
                        thread_count,
                        push_percent,
                        pop_percent
                )) {
                continue;
            }

            const std::string key = aggregate.implementation + " " + std::to_string(push_percent) +
                                    "/" + std::to_string(pop_percent);
            series[key].push_back(ContentionSeriesPoint{
                    .thread_count = thread_count,
                    .avg_ops_per_second = aggregate.avg_ops_per_second,
            });
            if (std::find(thread_counts.begin(), thread_counts.end(), thread_count) ==
                thread_counts.end()) {
                thread_counts.push_back(thread_count);
            }
            max_ops = std::max(max_ops, aggregate.avg_ops_per_second);
        }

        std::sort(thread_counts.begin(), thread_counts.end());
        for (auto& [_, points] : series) {
            std::sort(points.begin(), points.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.thread_count < rhs.thread_count;
            });
        }

        const int width = 1280;
        const int height = 720;
        const int margin_left = 90;
        const int margin_right = 240;
        const int margin_top = 80;
        const int margin_bottom = 90;
        const double plot_w = static_cast<double>(width - margin_left - margin_right);
        const double plot_h = static_cast<double>(height - margin_top - margin_bottom);

        std::ofstream out(output_path);
        out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width << "\" height=\""
            << height << "\" viewBox=\"0 0 " << width << " " << height << "\">\n";
        out << "<rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height
            << "\" fill=\"#ffffff\"/>\n";
        out << "<text x=\"" << width / 2
            << "\" y=\"40\" text-anchor=\"middle\" font-size=\"26\" font-family=\"Menlo, "
               "monospace\" "
               "fill=\"#111111\">Multithreaded Contention Throughput (average ops/sec)</text>\n";

        for (int tick = 0; tick <= 5; ++tick) {
            const double ratio = static_cast<double>(tick) / 5.0;
            const double y = margin_top + plot_h - ratio * plot_h;
            const double value = ratio * max_ops;
            out << "<line x1=\"" << margin_left << "\" y1=\"" << y << "\" x2=\""
                << (width - margin_right) << "\" y2=\"" << y
                << "\" stroke=\"#e0e0e0\" stroke-width=\"1\"/>\n";
            out << "<text x=\"" << (margin_left - 10) << "\" y=\"" << (y + 4)
                << "\" text-anchor=\"end\" font-size=\"12\" font-family=\"Menlo, monospace\" "
                   "fill=\"#444444\">"
                << format_metric(value) << "</text>\n";
        }

        out << "<line x1=\"" << margin_left << "\" y1=\"" << margin_top << "\" x2=\"" << margin_left
            << "\" y2=\"" << (height - margin_bottom)
            << "\" stroke=\"#222222\" stroke-width=\"2\"/>\n";
        out << "<line x1=\"" << margin_left << "\" y1=\"" << (height - margin_bottom) << "\" x2=\""
            << (width - margin_right) << "\" y2=\"" << (height - margin_bottom)
            << "\" stroke=\"#222222\" stroke-width=\"2\"/>\n";

        auto x_for_threads = [&](int threads) {
            const auto it = std::find(thread_counts.begin(), thread_counts.end(), threads);
            const size_t idx = static_cast<size_t>(std::distance(thread_counts.begin(), it));
            const double frac = thread_counts.size() == 1
                                        ? 0.0
                                        : static_cast<double>(idx) /
                                                  static_cast<double>(thread_counts.size() - 1);
            return margin_left + frac * plot_w;
        };

        for (const int threads : thread_counts) {
            const double x = x_for_threads(threads);
            out << "<text x=\"" << x << "\" y=\"" << (height - margin_bottom + 20)
                << "\" text-anchor=\"middle\" font-size=\"12\" font-family=\"Menlo, monospace\" "
                   "fill=\"#222222\">"
                << threads << "t</text>\n";
        }

        int legend_y = 90;
        size_t series_index = 0;
        for (const auto& [key, points] : series) {
            const std::string color = color_for_series_index(series_index);

            std::string polyline_points;
            for (const auto& point : points) {
                const double x = x_for_threads(point.thread_count);
                const double ratio = (max_ops > 0.0) ? (point.avg_ops_per_second / max_ops) : 0.0;
                const double y = margin_top + plot_h - ratio * plot_h;
                polyline_points += std::to_string(x) + "," + std::to_string(y) + " ";
                out << "<circle cx=\"" << x << "\" cy=\"" << y << "\" r=\"3.5\" fill=\"" << color
                    << "\"/>\n";
            }
            out << "<polyline points=\"" << polyline_points << "\" fill=\"none\" stroke=\"" << color
                << "\" stroke-width=\"2.5\"/>\n";

            out << "<rect x=\"" << (width - margin_right + 20) << "\" y=\"" << (legend_y - 10)
                << "\" width=\"14\" height=\"14\" fill=\"" << color << "\"/>\n";
            out << "<text x=\"" << (width - margin_right + 40) << "\" y=\"" << legend_y
                << "\" font-size=\"12\" font-family=\"Menlo, monospace\" fill=\"#222222\">" << key
                << "</text>\n";
            legend_y += 24;
            ++series_index;
        }

        out << "</svg>\n";
    }

    bool
    parse_mt_simple_op(std::string_view operation, std::string_view prefix, int& thread_count) {
        if (!operation.starts_with(prefix)) {
            return false;
        }

        try {
            thread_count = std::stoi(std::string(operation.substr(prefix.size())));
        }
        catch (...) {
            return false;
        }
        return true;
    }

    void write_mt_specialized_svg(
            const std::vector<BenchmarkAggregate>& aggregates,
            const std::filesystem::path& output_path
    ) {
        std::map<std::string, std::vector<ContentionSeriesPoint>> series;
        std::vector<int> thread_counts;
        double max_ops = 0.0;

        for (const auto& aggregate : aggregates) {
            int thread_count = 0;
            std::string key;
            if (parse_mt_simple_op(aggregate.operation, "mt_push_only_t", thread_count)) {
                key = aggregate.implementation + " push_only";
            }
            else if (parse_mt_simple_op(aggregate.operation, "mt_pop_only_t", thread_count)) {
                key = aggregate.implementation + " pop_only";
            }
            else {
                continue;
            }

            series[key].push_back(ContentionSeriesPoint{
                    .thread_count = thread_count,
                    .avg_ops_per_second = aggregate.avg_ops_per_second,
            });

            if (std::find(thread_counts.begin(), thread_counts.end(), thread_count) ==
                thread_counts.end()) {
                thread_counts.push_back(thread_count);
            }
            max_ops = std::max(max_ops, aggregate.avg_ops_per_second);
        }

        std::sort(thread_counts.begin(), thread_counts.end());
        for (auto& [_, points] : series) {
            std::sort(points.begin(), points.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.thread_count < rhs.thread_count;
            });
        }

        const int width = 1280;
        const int height = 720;
        const int margin_left = 90;
        const int margin_right = 260;
        const int margin_top = 80;
        const int margin_bottom = 90;
        const double plot_w = static_cast<double>(width - margin_left - margin_right);
        const double plot_h = static_cast<double>(height - margin_top - margin_bottom);

        std::ofstream out(output_path);
        out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width << "\" height=\""
            << height << "\" viewBox=\"0 0 " << width << " " << height << "\">\n";
        out << "<rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height
            << "\" fill=\"#ffffff\"/>\n";
        out << "<text x=\"" << width / 2
            << "\" y=\"40\" text-anchor=\"middle\" font-size=\"26\" font-family=\"Menlo, "
               "monospace\" "
               "fill=\"#111111\">Multithreaded Specialized Throughput (average ops/sec)</text>\n";
        out << "<text x=\"28\" y=\"" << (margin_top + plot_h / 2.0)
            << "\" text-anchor=\"middle\" font-size=\"13\" font-family=\"Menlo, monospace\" "
               "fill=\"#222222\" transform=\"rotate(-90 28 "
            << (margin_top + plot_h / 2.0) << ")\">ops/sec</text>\n";
        out << "<text x=\"" << (margin_left + plot_w / 2.0) << "\" y=\"" << (height - 12)
            << "\" text-anchor=\"middle\" font-size=\"13\" font-family=\"Menlo, monospace\" "
               "fill=\"#222222\">threads</text>\n";

        for (int tick = 0; tick <= 5; ++tick) {
            const double ratio = static_cast<double>(tick) / 5.0;
            const double y = margin_top + plot_h - ratio * plot_h;
            const double value = ratio * max_ops;
            out << "<line x1=\"" << margin_left << "\" y1=\"" << y << "\" x2=\""
                << (width - margin_right) << "\" y2=\"" << y
                << "\" stroke=\"#e0e0e0\" stroke-width=\"1\"/>\n";
            out << "<text x=\"" << (margin_left - 10) << "\" y=\"" << (y + 4)
                << "\" text-anchor=\"end\" font-size=\"12\" font-family=\"Menlo, monospace\" "
                   "fill=\"#444444\">"
                << format_metric(value) << "</text>\n";
        }

        out << "<line x1=\"" << margin_left << "\" y1=\"" << margin_top << "\" x2=\"" << margin_left
            << "\" y2=\"" << (height - margin_bottom)
            << "\" stroke=\"#222222\" stroke-width=\"2\"/>\n";
        out << "<line x1=\"" << margin_left << "\" y1=\"" << (height - margin_bottom) << "\" x2=\""
            << (width - margin_right) << "\" y2=\"" << (height - margin_bottom)
            << "\" stroke=\"#222222\" stroke-width=\"2\"/>\n";

        auto x_for_threads = [&](int threads) {
            const auto it = std::find(thread_counts.begin(), thread_counts.end(), threads);
            const size_t idx = static_cast<size_t>(std::distance(thread_counts.begin(), it));
            const double frac = thread_counts.size() == 1
                                        ? 0.0
                                        : static_cast<double>(idx) /
                                                  static_cast<double>(thread_counts.size() - 1);
            return margin_left + frac * plot_w;
        };

        for (const int threads : thread_counts) {
            const double x = x_for_threads(threads);
            out << "<text x=\"" << x << "\" y=\"" << (height - margin_bottom + 20)
                << "\" text-anchor=\"middle\" font-size=\"12\" font-family=\"Menlo, monospace\" "
                   "fill=\"#222222\">"
                << threads << "t</text>\n";
        }

        int legend_y = 90;
        size_t series_index = 0;
        for (const auto& [key, points] : series) {
            const std::string color = color_for_series_index(series_index);

            std::string polyline_points;
            for (const auto& point : points) {
                const double x = x_for_threads(point.thread_count);
                const double ratio = (max_ops > 0.0) ? (point.avg_ops_per_second / max_ops) : 0.0;
                const double y = margin_top + plot_h - ratio * plot_h;
                polyline_points += std::to_string(x) + "," + std::to_string(y) + " ";
                out << "<circle cx=\"" << x << "\" cy=\"" << y << "\" r=\"3.5\" fill=\"" << color
                    << "\"/>\n";
            }
            out << "<polyline points=\"" << polyline_points << "\" fill=\"none\" stroke=\"" << color
                << "\" stroke-width=\"2.5\"/>\n";

            out << "<rect x=\"" << (width - margin_right + 20) << "\" y=\"" << (legend_y - 10)
                << "\" width=\"14\" height=\"14\" fill=\"" << color << "\"/>\n";
            out << "<text x=\"" << (width - margin_right + 40) << "\" y=\"" << legend_y
                << "\" font-size=\"12\" font-family=\"Menlo, monospace\" fill=\"#222222\">" << key
                << "</text>\n";
            legend_y += 24;
            ++series_index;
        }

        out << "</svg>\n";
    }

} // namespace

int main(int argc, char** argv) {
    bool quick = false;
    bool allow_debug = false;

    for (int iii = 1; iii < argc; ++iii) {
        const std::string arg(argv[iii]);
        if (arg == "--quick") {
            quick = true;
        }
        else if (arg == "--allow-debug") {
            allow_debug = true;
        }
    }

#ifndef NDEBUG
    if (!allow_debug) {
        std::cerr << "Error: benchmark must run in a Release build. Reconfigure with "
                     "`-DCMAKE_BUILD_TYPE=Release`.\n";
        std::cerr << "Use `--allow-debug` only for smoke validation.\n";
        return 2;
    }
#endif

    const size_t iterations = quick ? 20'000 : 300'000;
    const int repeats = quick ? 2 : 5;
    const size_t contention_ops_per_thread = quick ? 10'000 : 100'000;
    const size_t specialized_ops_per_thread = quick ? 15'000 : 150'000;

    std::vector<BenchmarkSample> samples;
    samples.reserve(256);

    auto append_samples = [&samples](std::vector<BenchmarkSample> chunk) {
        samples.insert(
                samples.end(),
                std::make_move_iterator(chunk.begin()),
                std::make_move_iterator(chunk.end())
        );
    };

    using SeraphStack = seraph::stack<int>;
#if SERAPH_HAS_BOOST_LOCKFREE_STACK
    using BoostStack = BoostLockfreeStackAdapter;

    append_samples(bench_push_copy<SeraphStack>("stack", iterations, repeats));
    append_samples(bench_push_copy<BoostStack>("BoostStack", iterations, repeats));

    append_samples(bench_push_move<SeraphStack>("stack", iterations, repeats));
    append_samples(bench_push_move<BoostStack>("BoostStack", iterations, repeats));

    append_samples(bench_emplace<SeraphStack>("stack", iterations, repeats));
    append_samples(bench_emplace<BoostStack>("BoostStack", iterations, repeats));

    append_samples(bench_pop<SeraphStack>("stack", iterations, repeats));
    append_samples(bench_pop<BoostStack>("BoostStack", iterations, repeats));

    append_samples(bench_empty<SeraphStack>("stack", iterations, repeats));
    append_samples(bench_empty<BoostStack>("BoostStack", iterations, repeats));

    const std::vector<int> contention_threads = {2, 4, 8};
    const std::vector<int> push_percents = {10, 20, 50, 80, 100};
    for (const int thread_count : contention_threads) {
        for (const int push_percent : push_percents) {
            append_samples(bench_contention_mix<SeraphStack>(
                    "stack",
                    thread_count,
                    push_percent,
                    contention_ops_per_thread,
                    repeats
            ));
            append_samples(bench_contention_mix<BoostStack>(
                    "BoostStack",
                    thread_count,
                    push_percent,
                    contention_ops_per_thread,
                    repeats
            ));
        }
    }

    for (const int thread_count : contention_threads) {
        append_samples(bench_mt_push_only<SeraphStack>(
                "stack",
                thread_count,
                specialized_ops_per_thread,
                repeats
        ));
        append_samples(bench_mt_push_only<BoostStack>(
                "BoostStack",
                thread_count,
                specialized_ops_per_thread,
                repeats
        ));

        append_samples(bench_mt_pop_only<SeraphStack>(
                "stack",
                thread_count,
                specialized_ops_per_thread,
                repeats
        ));
        append_samples(bench_mt_pop_only<BoostStack>(
                "BoostStack",
                thread_count,
                specialized_ops_per_thread,
                repeats
        ));
    }
#else
    std::cerr << "Boost lockfree stack headers not found; cannot run Boost-only comparison.\n";
    return 3;
#endif

    const auto aggregates = build_aggregates(samples);

    const auto repo_root = find_repo_root();
    const auto output_dir = repo_root / "tests" / "perf_results";
    std::filesystem::create_directories(output_dir);

    const auto csv_path = output_dir / "stack_benchmark_results.csv";
    const auto ns_svg_path = output_dir / "stack_ns_per_op.svg";
    const auto ops_svg_path = output_dir / "stack_ops_per_sec.svg";
    const auto contention_svg_path = output_dir / "stack_contention_ops_per_sec.svg";
    const auto specialized_mt_svg_path = output_dir / "stack_specialized_mt_ops_per_sec.svg";

    write_results_csv(samples, aggregates, repeats, csv_path);
    write_svg_grouped_bars(aggregates, ns_svg_path, true);
    write_svg_grouped_bars(aggregates, ops_svg_path, false);
    write_contention_svg(aggregates, contention_svg_path);
    write_mt_specialized_svg(aggregates, specialized_mt_svg_path);

    std::cout << "stack performance benchmark complete.\n";
    std::cout << "Results CSV: " << csv_path << "\n";
    std::cout << "Graph (ns/op, averaged): " << ns_svg_path << "\n";
    std::cout << "Graph (ops/sec, averaged): " << ops_svg_path << "\n";
    std::cout << "Graph (contention ops/sec, averaged): " << contention_svg_path << "\n";
    std::cout << "Graph (specialized mt ops/sec, averaged): " << specialized_mt_svg_path << "\n";
    std::cout << "Sink: " << g_sink << "\n";

    return 0;
}
