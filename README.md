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

Publication uses release stores and consumption uses acquire loads on the per‑node/per‑slot state (`next`/`sequence`), ensuring payload visibility once a state transition is observed. Cursor/size counters are updated with relaxed atomics because they do not carry payload visibility. Correctness relies on the release/acquire edges. Hazard pointers and per‑slot reader counts prevent reclamation or mutation during a user read.

### `stack`
In vector mode, a spinlock serializes operations. Promotion to CAS mode is guarded by a `shared_mutex` so no operation straddles both modes, and all elements are transferred exactly once.

In CAS mode, `push` and `pop` are Treiber‑style and linearize at successful CAS on `cas_head_`. Hazard pointers prevent use‑after‑free during concurrent pops/peeks.

### `queue`

`push` linearizes when the new node is linked via CAS on `tail->next`, and `pop` linearizes when `head_` is advanced via CAS.

`front`/`back` are best‑effort peeks under hazard protection, and `size()` is an approximate counter under concurrency.

### `ringbuffer`
A slot is producer‑ready when `sequence == p`, consumer‑ready at `sequence == p + 1`, and recycled at `sequence == p + N`.

`push` publishes by writing the payload then storing `sequence = p + 1`, while `pop` claims via CAS on `dequeue_pos_` and marks the slot busy during move/reset.

`front`/`back` are observational helpers that attempt a stable copy; they are not linearizable snapshots.

## Performance Highlights

I compared Seraph to [`Boost`](https://www.boost.org/)’s lock‑free containers because they are commonly used in performance‑critical C++ code.

Specialized multithread throughput (ops/sec), Release build, 2/4/8 threads, 5 repeats (`queue`: 200k ops/thread, `stack`: 150k ops/thread):

![Queue push-only throughput](tests/perf_results/queue_specialized_mt_push_only_ops_per_sec.svg)
![Queue pop-only throughput](tests/perf_results/queue_specialized_mt_pop_only_ops_per_sec.svg)
![Stack specialized throughput](tests/perf_results/stack_specialized_mt_ops_per_sec.svg)

## Performance Summary

**Queue**: Seraph `queue` leads "pop‑only" throughput across 2/4/8 threads (~23.8M / 9.9M / 4.16M ops/sec). "push‑only" is led by `ringbuffer` at 2 threads (~14.7M ops/sec) and by `queue` at 4 and 8 threads (~8.4M / 4.8M ops/sec, respectively).

**Stack**: Seraph is competitive with Boost: `stack` leads at 4 threads for both push/pop (~9.1M / 8.9M ops/sec), while Boost wins at 2 threads and is slightly ahead at 8 threads.

## Reproduce Results

- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
- `cmake --build build --target seraph_queue_perf seraph_stack_perf`
- `./build/seraph_queue_perf`
- `./build/seraph_stack_perf`

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
