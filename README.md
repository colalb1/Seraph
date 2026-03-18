# Seraph

Seraph is a header-only C++ data-structure library for Apple ARM64.

## Why does this exist?

After reading [*The Art of Writing Efficient Programs*](https://www.amazon.com/Art-Writing-Efficient-Programs-optimizations/dp/1800208111) and [*C++ Templates: The Complete Guide*](https://www.amazon.com/C-Templates-Complete-Guide-2nd/dp/0321714121), I wanted to put the lessons into practice by building (basic) concurrent data structures and exploring the difficulty of outperforming a [portable high-performance package](https://www.boost.org/doc/libs/latest/doc/html/heap/data_structures.html) when targeting a particular architecture.

Thus, these data structures are not portable and are proprietary to [Apple ARM64](https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms).

## Design Notes

The specs of the machine (Macbook M4 Pro) optimized for are as follows:

- **L1 instruction cache size**: 128 KB
- **L1 data cache size**: 64 KB
- **L2 cache size**: 4 MB

The structures are tuned for 4-thread workloads. I also tested 2 and 8-thread workloads out of curiosity.

Cache‑line alignment reduces false sharing; per‑slot reader counts create a bounded critical section for peeks so `front/back` remain wait‑free with respect to `pop`’s move/reset.

### `stack`

A two‑phase system: a mutexed spinlock vector for low contention, then a [Treiber‑style](https://en.wikipedia.org/wiki/Treiber_stack) CAS list after surpassing a contention threshold.

### `queue`

A [Michael–Scott lock‑free queue](https://people.csail.mit.edu/shanir/publications/FIFO_Queues.pdf). Linearizability is enforced via compare-and-swap (CAS) on the `head/tail`. Hazard pointers provide a safe memory‑reclamation scheme under the C++ atomics model.

### `ringbuffer`

Uses a power‑of‑two capacity $N$, so the enqueue/dequeue positions advance monotonically in $\mathbb{Z}$, and each slot’s state is determined by the position’s congruence class modulo $N$. A mirrored index array of length $2N$ maps $[0, ..., 2N)$ to the same $N$ slots, so `back()` can scan linearly across a wrapped region without any branch-modular wrap‑around fixes.

## Correctness & Safety

- [Testing strategy or invariants]
- [Sanitizers / tooling]
- [Known constraints]

## Performance Highlights

I compared Seraph to [`Boost`](https://www.boost.org/)’s lock‑free containers because they are commonly used in performance‑critical C++ code.

Specialized multithread throughput (ops/sec), Release build, 2/4/8 threads, 5 repeats (`queue`: 200k ops/thread, `stack`: 150k ops/thread):

![Queue push-only throughput](tests/perf_results/queue_specialized_mt_push_only_ops_per_sec.svg)
![Queue pop-only throughput](tests/perf_results/queue_specialized_mt_pop_only_ops_per_sec.svg)
![Stack specialized throughput](tests/perf_results/stack_specialized_mt_ops_per_sec.svg)

## Performance Summary

Queue: Seraph `queue` leads pop‑only throughput across 2/4/8 threads (~23.8M / 9.9M / 4.16M ops/sec), while push‑only is led by `ringbuffer` at 2 threads (~14.7M ops/sec) and by `queue` at 4/8 threads (~8.4M / 4.8M ops/sec).

Stack: Seraph is competitive with Boost—`stack` leads at 4 threads for both push/pop (~9.1M / 8.9M ops/sec), while Boost edges at 2 threads and is slightly ahead at 8 threads.

## Benchmark Methodology

- Build: Release (`-DCMAKE_BUILD_TYPE=Release`)
- Threads: 2, 4, 8
- Repeats: 5
- Queue ops/thread: 200k
- Stack ops/thread: 150k
- Warm-up: none (each repeat runs without an explicit warm-up phase)
- Boost comparisons: enabled when Boost headers are found (CMake `find_package(Boost)` or `/opt/homebrew/include/boost`)

## Reproduce Results

- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
- `cmake --build build --target seraph_queue_perf seraph_stack_perf`
- `./build/seraph_queue_perf`
- `./build/seraph_stack_perf`

## Limitations

- [Portability or architecture constraints]
- [Performance caveats]
- [Concurrency or workload assumptions]

## Usage Examples

Queue:

```cpp
// [Minimal queue example]
```

Stack:

```cpp
// [Minimal stack example]
```


## Build Locally

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## Consumer Usage

### Option 1: add_subdirectory (local checkout)

```cmake
add_subdirectory(path/to/Seraph)
target_link_libraries(my_app PRIVATE seraph::seraph)
```

### Option 2: FetchContent (remote source)

```cmake
include(FetchContent)
FetchContent_Declare(
  seraph
  GIT_REPOSITORY https://github.com/<you>/Seraph.git
  GIT_TAG main
)
FetchContent_MakeAvailable(seraph)
target_link_libraries(my_app PRIVATE seraph::seraph)
```

### Option 3: Installed package (find_package)

Install Seraph:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix "$HOME/.local"
```

Use in another project:

```cmake
find_package(seraph CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE seraph::seraph)
```

## Formatting

```bash
cmake --build build --target format
```
