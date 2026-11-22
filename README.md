# lockfree

Small C++20 library and benchmarks for low-latency, lock-free building blocks on Linux/x86-64:
a bounded single-producer/multi-consumer (SPMC) queue, a fixed-size object pool and a spin barrier.

It is a learning and benchmarking project. Each component has a test, and the commit history records
the optimisation steps and the bugs found along the way (for example the ABA fix in the object pool).

## Components

| Header | What it provides |
|---|---|
| `SPMCQueue.h` | Bounded ring buffer (1024 slots). Each slot carries a sequence number that coordinates the producer and consumers; slots are padded to a cache line and the queue is page-aligned. |
| `SPMCProducer.h` | Producer loop driven by a compile-time `WorkPolicy` (C++20 concept). |
| `WorkerPool.h` | Consumer threads, each pinned to a core, driven by a compile-time `ConsumePolicy`. |
| `LockFreeObjectPool.h` | Fixed-capacity pool of reusable objects. `Lease()` returns a `std::unique_ptr` whose deleter returns the object to the pool. |
| `SpinWaitBarrier.h` | Generation-counting barrier; the last thread to arrive releases the others. |
| `ScopedMemoryLock.h` | RAII wrapper around `mlock`/`munlock`. |

## Building and testing

Requirements: Linux on x86-64 (the code uses `_mm_pause`, `pthread_setaffinity_np` and `mlock`),
a C++20 compiler (developed with GCC 15.3) and CMake (see `CMakeLists.txt` for the minimum
version). The first configure downloads GoogleTest, so it needs network access.

```bash
cmake -B build
cmake --build build -j
ctest --test-dir build        # pool unit tests and the barrier test
./build/queue_benchmark       # queue benchmark (1000 runs, about 2.5 minutes)
```

Options:
- `-DENABLE_TSAN=ON` builds everything with ThreadSanitizer (use a separate build directory, for
  example `cmake -B build-tsan -DENABLE_TSAN=ON`).
- `-DLOCKFREE_NATIVE_ARCH=OFF` drops `-march=native` for portable binaries.

Notes:
- The benchmark pins its producer to core 2 and its three consumers to cores 3-5, and the barrier
  test uses cores 1-5, so a machine with at least 6 logical CPUs is needed.
- The queue is locked into RAM with `mlock`. If `ulimit -l` is too low the benchmark aborts with an
  error message.

## Design notes

**SPMC queue.** Each slot has a sequence number. The producer waits until a slot's sequence equals
its own count, writes the payload, then publishes `count + 1` with a release store. A consumer waits for
`ticket + 1`, reads the payload, and hands the slot back by storing `ticket + 1024`. Consumers do not
contend on a shared read index: each computes its batch of 128 slots from its worker id and a local
block counter.

**Object pool.** An index-linked free list in an array, popped and pushed with compare-and-swap.
The head is a 64-bit word holding a 32-bit tag and a 32-bit index. The tag changes on every push and
pop, which prevents the ABA problem. `PoolTest.ABA` forces the problematic interleaving through a
`Policy` hook and fails if the tag updates are removed. The policy also has hooks for counting CAS
retries and pool exhaustion.

**Barrier.** Threads arrive with an atomic increment. The last arrival resets the counter and
advances a monotonic generation index, which releases the waiting threads.

## Benchmark results

Run in November 2026 on an Intel Core i5-8400 @ 2.80 GHz (6 cores, 6 threads, no SMT), SUSE Linux with
kernel 6.12.0-160000.41-default, GCC 15.3.0, CPU frequency governor `powersave`.

**Queue** (1 producer on core 2, 3 consumers on cores 3-5, batch size 128, 1000 runs,
10,240,000 items per run = 1024 slots x 10,000 rounds):

| Average | Std dev | Min | p99 | Max |
|---|---|---|---|---|
| 154.0 ms | 2.6 ms | 148 ms | 161 ms | 187 ms |

Two runs on the same machine agreed to within about 1% (averages of 154.9 ms and 154.0 ms).

**Barrier** (5 workers on cores 1-5, 10,000 generations, two barriers per cycle):
about 270-320 ns per barrier (268, 276 and 316 ns over three runs).

Results depend on the machine and on what else is running; treat them as indicative.

## Known limitations

- Sequence counters are 32-bit, so a single run of more than about 4.29 billion items is not supported.
- Batches are assigned to consumers statically, so a descheduled consumer stalls the pipeline.
  This is acceptable on isolated, pinned cores but not in general.
- x86-64 only (`_mm_pause`).

## Licence

Apache License 2.0. See `LICENSE`.
