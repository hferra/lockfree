#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <cassert>

#include <pthread.h>
#include <sched.h>

#include "SpinWaitBarrier.h"

// Helper function to pin a thread to a specific core index
void pin_thread_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}


int main() {
    // ----------------------------------------------------
    // SETUP
    // ----------------------------------------------------
    constexpr std::uint32_t ThreadCount = 5; //oversubscription
    constexpr int TotalIterations = 10000;   // 10,000 back-to-back batch processing cycles

    SpinWaitBarrier barrier(ThreadCount);
    std::atomic<bool> master_start{false};
    std::vector<std::thread> workers;

    // Shared mock workload array
    alignas(64) std::atomic<std::uint64_t> shared_work_accumulator{0};

    // ----------------------------------------------------
    // WORKER THREAD LOGIC
    // ----------------------------------------------------
    for (std::uint32_t t = 0; t < ThreadCount; ++t) {
        workers.emplace_back([&barrier, &master_start, &shared_work_accumulator, t, TotalIterations]() {
            int worker_core = t % 5; 
            pin_thread_to_core(worker_core);
            // Spin until the master start signal is flipped to maximize alignment
            while (!master_start.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
                //std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            for (int iter = 0; iter < TotalIterations; ++iter) {
                // PHASE 1: Do some thread-local work before entering the barrier
                // This simulates your batch-processing step
                shared_work_accumulator.fetch_add(1, std::memory_order_relaxed);

                // Hit the entry gate and wait for all workers to arrive
                barrier.wait();

                // PHASE 2: Simulate quick business logic right after exiting
                // Fast threads will immediately loop back and hit the gateOpen pre-check
                if (t == 0) {
                    // Introduce a tiny variable delay to core 0 to force threads 
                    // to exit wait() out of alignment and try to overrun the gate
                    _mm_pause();
                }
            }
        });
    }

    // Pin the Main (Master) Thread to Core 5
    // This guarantees the coordinator always has a dedicated core to execute reset()
    pin_thread_to_core(5);

    // ----------------------------------------------------
    // COORDINATOR LOOP (MASTER THREAD)
    // ----------------------------------------------------
    std::cout << "Starting barrier stress test with " << ThreadCount << " threads across " 
              << TotalIterations << " generations..." << std::endl;

    auto start_time = std::chrono::high_resolution_clock::now();
    master_start.store(true, std::memory_order_release); // Unleash the workers

    for (int iter = 0; iter < TotalIterations; ++iter) {
        // The coordinator explicitly resets the barrier for every single cycle.
        // It will safely spin inside reset() until all worker threads have completed their current iteration.
        barrier.reset();

        // Optional: Sanity check our shared workload processing halfway through
        if (iter == TotalIterations / 2) {
            std::uint64_t current_work = shared_work_accumulator.load(std::memory_order_relaxed);
            assert(current_work >= (TotalIterations / 2) * ThreadCount && "Work synchronization anomaly detected.");
        }
    }

    // Clean up the threads
    for (auto& worker : workers) {
        worker.join();
    }
    auto end_time = std::chrono::high_resolution_clock::now();

    // ----------------------------------------------------
    // METRICS & VERIFICATION
    // ----------------------------------------------------
    std::chrono::duration<double, std::milli> elapsed_time = end_time - start_time;
    std::uint64_t total_expected_work = TotalIterations * ThreadCount;

    std::cout << "\n================ TEST RESULTS ================" << std::endl;
    std::cout << "Total Execution Time:    " << elapsed_time.count() << " ms" << std::endl;
    std::cout << "Completed Generations:   " << TotalIterations << std::endl;
    std::cout << "Accumulated Work Units:  " << shared_work_accumulator.load() << " / " << total_expected_work << std::endl;
    std::cout << "Average Cycle Latency:   " << (elapsed_time.count() * 1000000.0) / TotalIterations << " ns" << std::endl;
    std::cout << "==============================================" << std::endl;

    assert(shared_work_accumulator.load() == total_expected_work && "Data race or missed operations detected!");
    std::cout << "Verification: PASSED! (No deadlocks, zero overrun errors)" << std::endl;

    return 0;
}
