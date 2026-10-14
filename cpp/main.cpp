#include <iostream>
#include <thread>
#include <chrono>
#include <pthread.h> // For thread affinity on Linux

#include "SPMCqueue.h"
#include "SPMCproducer.h"
#include "worker_pool.h"

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
    // 1. Instantiate the shared queue infrastructure
    SPMCQueue shared_queue;

    {
        // 2. Pin the queue memory using RAII
        // The lock remains active for the entire duration of this execution block
        ScopedMemoryLock queue_lock(&shared_queue, sizeof(shared_queue));

        // 3. Warm up the buffer while securely pinned in physical RAM
        for (size_t i = 0; i < RingBufferLen; ++i) {
            shared_queue.store[i] = 0.0f; 
        }

        // 4. Instantiate workloads
        SPMCProducer producer(&shared_queue);
        WorkerPool consumers(&shared_queue, 3);

        std::cout << "Starting benchmark with RAII-managed memory pinning..." << std::endl;
        auto start_time = std::chrono::high_resolution_clock::now();

        // 5. Run execution pathways on isolated cores
        consumers.Start();
        std::thread producer_thread(&SPMCProducer::Run, &producer);

        pin_thread_to_core(producer_thread, 2);

        // 6. Synchronize and join threads
        producer_thread.join();
        shared_queue.done.store(true, std::memory_order_release);
        consumers.Shutdown();

        auto end_time = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

        std::cout << "Benchmark complete! Elapsed time: " << elapsed << " ms" << std::endl;
        
    } // <-- queue_lock automatically goes out of scope here and calls munlock()!

    return 0;
}
