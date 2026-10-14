#pragma once

#include <thread>
#include <vector>
#include <atomic>
#include <cstdint>
#include <immintrin.h> // Required for _mm_pause
#include "SPMCqueue.h"

class WorkerPool {
private:
    SPMCQueue* queue;
    std::vector<std::thread> workers;
    std::size_t num_threads;
    
    // Aligned monitoring accumulator to aggregate local worker throughput
    alignas(64) std::atomic<float> global_sum{0.0f};

    // The core hot-path execution loop run by every thread in the pool
    void WorkerLoop(int worker_id) {
        // 1. PIN THE THREAD
        // For performance determinism, pin each worker to its own isolated CPU core
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(3 + worker_id, &cpuset); // Pins to Cores 3, 4, 5, etc.
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

        float thread_local_sum = 0.0f;

        while (true) {
            // 2. TICKET PRE-RESERVATION
            // Multi-thread atomic reservation handles index mapping concurrently.
            // acq_rel prevents reads/writes from reordering across the allocation frontier.
            std::uint32_t reserved_ticket = queue->readIdx.fetch_add(1, std::memory_order_acq_rel);
            
            // Map the monotonic ticket token to the physical ring buffer index
            std::uint32_t slot_index = reserved_ticket % RingBufferLen;
            Slot& target_slot = queue->store[slot_index];

            // 3. SPIN WAIT PHASE 1: Main global tracking fence
            while (queue->writeIdx.load(std::memory_order_acquire) <= reserved_ticket) {
                // Graceful loop exit: If the queue is finished and no new data is coming, shutdown
                if (queue->done.load(std::memory_order_acquire) && 
                    queue->writeIdx.load(std::memory_order_acquire) <= reserved_ticket) {
                    
                    // Commit our local math before exiting
                    global_sum.fetch_add(thread_local_sum, std::memory_order_relaxed);
                    return; 
                }
                _mm_pause(); // Hint the CPU pipeline during the spin
            }

            // 4. SPIN WAIT PHASE 2: Generational Slot Validation
            // Wait until the slot's internal sequence tracker matches our specific ticket turn.
            // Expected sequence is always (reserved_ticket + 1) once the producer commits the write.
            std::uint32_t expected_sequence = reserved_ticket + 1;
            while (target_slot.sequence.load(std::memory_order_acquire) != expected_sequence) {
                _mm_pause();
            }

            // 5. CRITICAL DATA READ
            // Safe payload extraction. Data race/corruption is physically impossible now.
            thread_local_sum += target_slot.value;

            // 6. GENERATIONAL INVALIDATION (The Clear Handshake)
            // Advance the slot's tracking sequence token forward by the RingBufferLen.
            // This signals to the producer that this memory slot is clear to be overwritten.
            target_slot.sequence.store(reserved_ticket + RingBufferLen, std::memory_order_release);
        }
    }

public:
    WorkerPool(SPMCQueue* q, std::size_t threads) 
        : queue(q)
        , num_threads(threads) 
    {
        workers.reserve(num_threads);
    }

    ~WorkerPool() {
        Shutdown();
    }

    // Spawns the worker threads and kicks off execution loops
    void Start() {
        for (std::size_t i = 0; i < num_threads; ++i) {
            workers.emplace_back(&WorkerPool::WorkerLoop, this, static_cast<int>(i));
        }
    }

    // Synchronizes the pool threads during teardown
    void Shutdown() {
        for (auto& th : workers) {
            if (th.joinable()) {
                th.join();
            }
        }
        workers.clear();
    }

    [[nodiscard]] float GetTotalSum() const noexcept {
        return global_sum.load(std::memory_order_relaxed);
    }
};
