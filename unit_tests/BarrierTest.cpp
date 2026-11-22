// Data-passing test for SpinWaitBarrier.
//
// Every worker writes a private data packet, waits at the barrier, then checks the packet written by
// its left-hand neighbour. If the barrier lets a thread through early, or fails to order memory, a
// check fails and the program exits non-zero. The checks are explicit rather than assert(), so they
// still run in release builds.
//
// Also run this test under ThreadSanitizer. On x86 a missing acquire/release ordering can still pass
// the value checks, but TSan reports the unsynchronised neighbour read as a data race.
//
// Usage: barrier_test [iterations]      (default 10000)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>
#include <pthread.h>
#include <sched.h>

#include "SpinWaitBarrier.h"

namespace {

constexpr int kSlots = 7;                    // mock payload words per packet
constexpr int kChecksPerIteration = 1 + kSlots;

// Each worker owns one cache line, so the workers do not false-share.
struct alignas(64) WorkerPacket {
    std::uint64_t generation_stamp = 0;
    std::uint64_t calculations[kSlots] = {};
};

// Per-worker tallies, also one cache line each.
struct alignas(64) WorkerResult {
    std::uint64_t checks = 0;
    std::uint64_t mismatches = 0;
};

bool pin_thread_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
}

// The value worker `worker` publishes in iteration `iter` at slot `i`. The writer and the checker
// both use this, so they cannot drift apart.
std::uint64_t payload(int iter, std::uint32_t worker, int i) {
    return static_cast<std::uint64_t>(iter) * 100 + worker + static_cast<std::uint64_t>(i);
}

} // namespace

int main(int argc, char** argv) {
    const int total_iterations = (argc > 1) ? std::max(1, std::atoi(argv[1])) : 10000;

    // Prefer 5 workers on cores 1-5, leaving core 0 to the OS. On smaller machines use fewer
    // workers, so a spinning barrier never has more threads than cores.
    constexpr std::uint32_t kPreferredThreads = 5;
    const std::uint32_t hw = std::thread::hardware_concurrency();
    const std::uint32_t thread_count =
        std::max<std::uint32_t>(2, std::min<std::uint32_t>(kPreferredThreads, hw > 1 ? hw - 1 : 1));
    const bool can_pin = hw > thread_count;  // cores 1..thread_count must exist

    SpinWaitBarrier barrier(thread_count);
    std::atomic<bool> master_start{false};
    std::vector<WorkerPacket> shared_data_grid(thread_count);
    std::vector<WorkerResult> results(thread_count);
    std::atomic<std::uint32_t> pin_failures{0};
    std::vector<std::thread> workers;

    for (std::uint32_t t = 0; t < thread_count; ++t) {
        workers.emplace_back([&, t] {
            if (can_pin && !pin_thread_to_core(static_cast<int>(t) + 1)) {
                ++pin_failures;
            }
            while (!master_start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            std::uint64_t checks = 0, mismatches = 0;
            const std::uint32_t neighbour = (t == 0) ? thread_count - 1 : t - 1;

            for (int iter = 0; iter < total_iterations; ++iter) {
                // Phase 1: write this worker's private packet.
                shared_data_grid[t].generation_stamp = static_cast<std::uint64_t>(iter);
                for (int i = 0; i < kSlots; ++i) {
                    shared_data_grid[t].calculations[i] = payload(iter, t, i);
                }

                barrier.wait();

                // Phase 2: after the barrier it is safe to read the neighbour's packet.
                const WorkerPacket& n = shared_data_grid[neighbour];
                ++checks;
                if (n.generation_stamp != static_cast<std::uint64_t>(iter)) ++mismatches;
                for (int i = 0; i < kSlots; ++i) {
                    ++checks;
                    if (n.calculations[i] != payload(iter, neighbour, i)) ++mismatches;
                }

                // Second barrier: stops a fast thread from looping round and overwriting its packet
                // before a slow neighbour has finished reading it.
                barrier.wait();
            }
            results[t].checks = checks;
            results[t].mismatches = mismatches;
        });
    }

    std::cout << "Barrier data-passing test: " << thread_count << " workers, " << total_iterations
              << " iterations" << (can_pin ? ", pinned to cores 1-" + std::to_string(thread_count) : ", not pinned")
              << std::endl;
    const auto start_time = std::chrono::high_resolution_clock::now();
    master_start.store(true, std::memory_order_release);
    for (auto& w : workers) w.join();
    const auto end_time = std::chrono::high_resolution_clock::now();
    const std::chrono::duration<double, std::milli> elapsed = end_time - start_time;

    std::uint64_t checks = 0, mismatches = 0;
    for (const auto& r : results) {
        checks += r.checks;
        mismatches += r.mismatches;
    }
    const std::uint64_t expected_checks =
        static_cast<std::uint64_t>(total_iterations) * thread_count * kChecksPerIteration;

    std::cout << "\n================ BARRIER TEST RESULTS ================\n"
              << "Total run time:        " << elapsed.count() << " ms\n"
              << "Barrier crossings:     " << static_cast<std::uint64_t>(total_iterations) * 2 << "\n"
              << "Average per crossing:  " << (elapsed.count() * 1e6) / (total_iterations * 2.0) << " ns\n"
              << "Checks performed:      " << checks << " (expected " << expected_checks << ")\n"
              << "Mismatches:            " << mismatches << "\n";
    if (pin_failures.load() != 0) {
        std::cout << "Warning: " << pin_failures.load() << " thread(s) could not be pinned\n";
    }
    std::cout << "======================================================\n";

    if (mismatches != 0 || checks != expected_checks) {
        std::cout << "Verification: FAILED\n";
        return 1;
    }
    std::cout << "Verification: PASSED (all " << checks << " checks)\n";
    return 0;
}
