// Copyright 2026 Herman L. Ferra
// SPDX-License-Identifier: Apache-2.0

#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <cassert>
#include <pthread.h>
#include <sched.h>

#include "SpinWaitBarrier.h"

// Helper function to pin a thread to a specific physical core index
void pin_thread_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

int main() {
    // ----------------------------------------------------
    // CONFIGURATION (5 Workers pinned to Cores 1-5)
    // ----------------------------------------------------
    constexpr std::uint32_t ThreadCount = 5; 
    constexpr int TotalIterations = 10000;   

    SpinWaitBarrier barrier(ThreadCount);
    std::atomic<bool> master_start{false};
    std::vector<std::thread> workers;

    // Shared Batch Data Packets
    // Each worker gets a dedicated 64-byte aligned cache line to prevent false sharing
    struct alignas(64) WorkerPacket {
        uint64_t generation_stamp;
        uint64_t calculations[7]; // Mock data payload
    };
    std::vector<WorkerPacket> shared_data_grid(ThreadCount);

    // ----------------------------------------------------
    // WORKER THREAD LOGIC
    // ----------------------------------------------------
    // Leave Core 0 unpinned to isolate OS kernel interrupts from the low-latency thread pool
    for (std::uint32_t t = 0; t < ThreadCount; ++t) {
        workers.emplace_back([&barrier, &master_start, &shared_data_grid, t, TotalIterations]() {
            // Pin strictly to Cores 1-5, leaving Core 0 entirely for the Linux OS
            pin_thread_to_core(t + 1);

            while (!master_start.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }

            for (int iter = 0; iter < TotalIterations; ++iter) {
                // ------------------------------------------------
                // PHASE 1: Write Private Data Packet
                // ------------------------------------------------
                // Worker 't' exclusively owns and writes to row 't'.
                // We use the current loop iteration as a unique mathematical signature.
                shared_data_grid[t].generation_stamp = iter;
                for (int i = 0; i < 7; ++i) {
                    shared_data_grid[t].calculations[i] = (iter * 100) + t + i;
                }

                // --- THE SYNCHRONIZATION BARRIER ---
                // This forces all memory writes to complete and flushes the 
                // modified cache lines across the CPU interconnect ring bus.
                barrier.wait();

                // ------------------------------------------------
                // PHASE 2: Cross-Thread Data Verification
                // ------------------------------------------------
                // Because the barrier passed, it is now legally safe to read 
                // data from ANY other thread's row. Let's check our left neighbor.
                uint32_t neighbor = (t == 0) ? (ThreadCount - 1) : (t - 1);
                
                // Assert that the neighbor has fully completed the exact same iteration
                assert(shared_data_grid[neighbor].generation_stamp == static_cast<uint64_t>(iter) && 
                       "CRITICAL ERROR: Memory reordering or race condition detected!");

                // Verify the mathematical payload written by the neighbor
                for (int i = 0; i < 7; ++i) {
                    uint64_t expected_val = (iter * 100) + neighbor + i;
                    assert(shared_data_grid[neighbor].calculations[i] == expected_val && 
                           "CRITICAL ERROR: Data corruption or dirty cache read!");
                }

                // --- ANTI-OVERRUN GATE ---
                // We wait again to prevent incredibly fast threads from looping back
                // and destroying their data before slow threads finish reading it.
                barrier.wait();
            }
        });
    }

    // ----------------------------------------------------
    // RUN BENCHMARK
    // ----------------------------------------------------
    std::cout << "Starting DECENTRALIZED Data-Passing Test (5 workers on Cores 1-5)..." << std::endl;
    auto start_time = std::chrono::high_resolution_clock::now();
    
    master_start.store(true, std::memory_order_release); // Release the pack

    for (auto& worker : workers) {
        worker.join();
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed_time = end_time - start_time;

    std::cout << "\n================ DATA TEST RESULTS ================" << std::endl;
    std::cout << "Total Run Time:        " << elapsed_time.count() << " ms" << std::endl;
    std::cout << "Completed Generations: " << TotalIterations << std::endl;
    std::cout << "Total Data Verifications: " << TotalIterations * ThreadCount * 8 << " successful passes" << std::endl;
    std::cout << "Average Step Latency:   " << (elapsed_time.count() * 1000000.0) / (TotalIterations * 2) << " ns (Includes 2 barriers per cycle)" << std::endl;
    std::cout << "===================================================" << std::endl;
    std::cout << "Verification: PASSED! (Perfect data integrity, zero races)" << std::endl;

    return 0;
}
