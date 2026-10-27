#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <cassert>

#include "LockFreeObjectPool.h"

// Mock payload to simulate realistic object size and work
struct MockObject {
    int id = 0;
    int lease_count = 0;
    void Reset() { id = 0; }
    void Clear() { id = 0; lease_count = 0; }
};

int main() {
    // ----------------------------------------------------
    // SETUP
    // ----------------------------------------------------
    constexpr std::size_t PoolSize = 4;
    constexpr int OperationsPerThread = 50000;
    constexpr int ThreadCount = 8; // Adjust based on your CPU cores

    LockFreeObjectPool<MockObject, PoolSize> pool;
    std::atomic<bool> start_signal{false};
    std::atomic<std::size_t> total_successful_leases{0};
    std::atomic<std::size_t> total_failed_leases{0};

    // Tracks overall operation times across threads
    std::vector<std::chrono::nanoseconds> lease_times(ThreadCount, std::chrono::nanoseconds(0));
    std::vector<std::chrono::nanoseconds> return_times(ThreadCount, std::chrono::nanoseconds(0));

    std::vector<std::thread> workers;

    // ----------------------------------------------------
    // WORKER THREAD LOGIC
    // ----------------------------------------------------
    for (int t = 0; t < ThreadCount; ++t) {
        workers.emplace_back([&pool, &start_signal, &total_successful_leases, &total_failed_leases, &lease_times, &return_times, t]() {
            // Wait for the master start signal to maximize simultaneous contention
            while (!start_signal.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }

            std::chrono::nanoseconds local_lease_accum(0);
            std::chrono::nanoseconds local_return_accum(0);
            std::size_t local_success = 0;
            std::size_t local_fail = 0;

            for (int i = 0; i < OperationsPerThread; ++i) {
                // 1. Time the Lease operation
                auto t0 = std::chrono::high_resolution_clock::now();
                auto obj = pool.Lease();
                auto t1 = std::chrono::high_resolution_clock::now();
                local_lease_accum += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0);

                if (obj) {
                    local_success++;
                    
                    // Simulate minor business logic work with the object
                    obj->id = t * 10000 + i; 
                    std::this_thread::yield(); 

                    // 2. Time the Return operation (triggered when obj goes out of scope)
                    auto t2 = std::chrono::high_resolution_clock::now();
                    obj.reset(); // Manually invoke the unique_ptr custom deleter
                    auto t3 = std::chrono::high_resolution_clock::now();
                    local_return_accum += std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2);
                } else {
                    local_fail++;
                }
            }

            // Aggregate metrics
            total_successful_leases.fetch_add(local_success, std::memory_order_relaxed);
            total_failed_leases.fetch_add(local_fail, std::memory_order_relaxed);
            lease_times[t] = local_lease_accum;
            return_times[t] = local_return_accum;
        });
    }

    // ----------------------------------------------------
    // EXECUTION & TIMING
    // ----------------------------------------------------
    std::cout << "Starting stress test with " << ThreadCount << " threads (" 
              << (ThreadCount * OperationsPerThread) << " total ops)..." << std::endl;

    auto global_start = std::chrono::high_resolution_clock::now();
    start_signal.store(true, std::memory_order_release); // Unleash workers simultaneously

    for (auto& worker : workers) {
        worker.join();
    }
    auto global_end = std::chrono::high_resolution_clock::now();

    // ----------------------------------------------------
    // METRICS COMPILATION
    // ----------------------------------------------------
    std::chrono::duration<double, std::milli> total_wall_time = global_end - global_start;
    
    std::chrono::nanoseconds overall_lease_ns(0);
    std::chrono::nanoseconds overall_return_ns(0);
    for (int t = 0; t < ThreadCount; ++t) {
        overall_lease_ns += lease_times[t];
        overall_return_ns += return_times[t];
    }

    std::size_t success_ops = total_successful_leases.load();
    double avg_lease_ns = success_ops ? (double)overall_lease_ns.count() / success_ops : 0;
    double avg_return_ns = success_ops ? (double)overall_return_ns.count() / success_ops : 0;

    // ----------------------------------------------------
    // OUTPUT RESULTS
    // ----------------------------------------------------
    std::cout << "\n================ TEST RESULTS ================" << std::endl;
    std::cout << "Total Wall-Clock Time:   " << total_wall_time.count() << " ms" << std::endl;
    std::cout << "Successful Leases:       " << success_ops << std::endl;
    std::cout << "Failed Leases (Empty):   " << total_failed_leases.load() << std::endl;
    std::cout << "----------------------------------------------" << std::endl;
    std::cout << "Average Lease Latency:   " << avg_lease_ns << " ns" << std::endl;
    std::cout << "Average Return Latency:  " << avg_return_ns << " ns" << std::endl;
    std::cout << "==============================================" << std::endl;

    // Sanity check: Ensure that after all threads terminate, we can lease again 
    // confirming no memory leaks or broken nodes in the lock-free chain.
    auto integrity_check = pool.Lease();
    assert(integrity_check != nullptr && "Pool corruption detected: failed to lease after test execution.");
    std::cout << "Integrity Check: PASSED (Pool is stable)" << std::endl;

    return 0;
}
