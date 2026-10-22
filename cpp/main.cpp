#include <iostream>
#include <thread>
#include <future>
#include <chrono>
#include <random>
#include <pthread.h> // For thread affinity on Linux
#include <cassert>
#include <algorithm>

#include "SPMCqueue.h"
#include "SPMCproducer.h"
#include "worker_pool.h"

// The user defines their custom business logic here
struct RandomGeneratorPolicy {
    std::mt19937 gen;
    std::uniform_real_distribution<float> dis;

    // Explicit constructor to ensure standard initialization works perfectly
    RandomGeneratorPolicy(std::uint32_t seed) 
        : gen(seed)
        , dis(0.0f, 1.0f) 
    {}

    RandomGeneratorPolicy(RandomGeneratorPolicy&&) noexcept = default;
    RandomGeneratorPolicy& operator=(RandomGeneratorPolicy&&) noexcept = default;

    // Must return EXACTLY float to satisfy std::same_as<float>
    [[nodiscard]] inline float Generate() noexcept {
        return dis(gen);
    }
};

// Helper function to pin a native thread to a specific CPU core
void pin_thread_to_core(std::thread& th, int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    
    pthread_t native_handle = th.native_handle();
    int rc = pthread_setaffinity_np(native_handle, sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << "Error pinning thread to core " << core_id << ": " << rc << std::endl;
    }
}

int main() {
    constexpr size_t total_bench_runs = 1000;
    std::vector<double> timings;
    timings.reserve(total_bench_runs);

    for (size_t run = 0; run < total_bench_runs; ++run) {
        // 1. Instantiate the shared queue infrastructure
        SPMCQueue shared_queue;
        std::promise<std::uint32_t> producer_promise;
        std::future<std::uint32_t> producer_future = producer_promise.get_future();
        std::random_device rd;

        RandomGeneratorPolicy policy(rd());
        {
            // 2. Pin the queue memory using RAII
            // The lock remains active for the entire duration of this execution block
            ScopedMemoryLock queue_lock(&shared_queue, sizeof(shared_queue));

            // 1. Instantiate the shared queue infrastructure
            SPMCQueue shared_queue;
            std::promise<std::uint32_t> producer_promise;
            std::future<std::uint32_t> producer_future = producer_promise.get_future();

            // 3. Warm up the buffer while securely pinned in physical RAM
            for (size_t i = 0; i < RingBufferLen; ++i) {
                shared_queue.store[i] = 0.0f; 
            }

            // 4. Instantiate workloads
            SPMCProducer<RandomGeneratorPolicy> producer(&shared_queue);
            WorkerPool consumers(&shared_queue, 3);


            // 5. Run execution pathways on isolated cores
            consumers.Start();
            std::thread producer_thread(&SPMCProducer<RandomGeneratorPolicy>::Run, &producer, std::move(producer_promise), std::ref(policy));
        

            std::cout << "Starting benchmark with RAII-managed memory pinning " << run << "..." << std::endl;
            auto start_time = std::chrono::high_resolution_clock::now();

            // 5. Run execution pathways on isolated cores
            pin_thread_to_core(producer_thread, 2);

            // 6. Synchronize and join threads
            producer_thread.join();
            std::uint32_t totalWork = producer_future.get();
            shared_queue.done.store(true, std::memory_order_release);
            std::uint32_t workDone = consumers.Shutdown();

            auto end_time = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
            timings.push_back(elapsed);

            //std::cout << "Benchmark complete! Elapsed time: " << elapsed << " ms" << std::endl;
            assert(totalWork == workDone);
            
        } // <-- queue_lock automatically goes out of scope here and calls munlock()!
    }

    // 1. Calculate Average
    double sum = std::accumulate(timings.begin(), timings.end(), 0.0);
    double avg = sum / timings.size();

    // 2. Calculate Standard Deviation
    double sq_sum = std::inner_product(timings.begin(), timings.end(), timings.begin(), 0.0);
    double stdev = std::sqrt(sq_sum / timings.size() - avg * avg);

    // 3. Extract Percentiles
    std::sort(timings.begin(), timings.end());
    double min_val = timings.front();
    double max_val = timings.back();
    double p99 = timings[static_cast<size_t>(total_bench_runs * 0.99)];

    std::cout << "--- Statistical Summary (" << total_bench_runs << " runs) ---" << std::endl;
    std::cout << "Avg: " << avg << " ms | StdDev: " << stdev << " ms" << std::endl;
    std::cout << "Min: " << min_val << " ms | p99: " << p99 << " ms | Max: " << max_val << " ms" << std::endl;

    return 0;
}