#pragma once

#include <thread>
#include <future>
#include <vector>
#include <atomic>
#include <cstdint>
#include <immintrin.h> // Required for _mm_pause
#include <concepts>
#include "SPMCqueue.h"

// Define a C++20 concept to enforce your library's interface at compile-time
template<typename T>
concept ConsumePolicy = requires(T policy, float payload, float& sum ) {
    { policy.Consume(payload, sum) } -> std::same_as<void>;
};


 /**
 * @brief Thread-pool manager that coordinates concurrent worker threads to pull and process items from an SPMCQueue.
 *
 * @tparam Policy The compile-time user policy enforcing the ConsumePolicy concept. Must implement a 
 *                thread-isolated `.Consume(T payload, AccumulatorType& thread_local_state)` method.
 *
 * The WorkerPool spans and manages a group of independent, hardware-isolated execution threads.
 * Each worker thread runs an internal hot-path loop pinned to a specific CPU core (starting 
 * at Core 3) to process items via the compile-time `Policy` object, ensuring zero-overhead data ingestion.
 *
 * @section Synchronization Strategy
 * Coordination with the concurrent producer thread is achieved entirely via deterministic ticket matching 
 * on the atomic `sequence` field embedded within each ring buffer `Slot`:
 * 
 * 1. **Zero-Contention Batch Reservation:** Instead of competing over a shared atomic read index, 
 *    each worker thread utilizes a completely deterministic mathematical formula (`batch_iteration * num_threads + worker_id`) 
 *    to map its next contiguous block of 128 slots. This entirely eliminates atomic bus-locking traffic 
 *    (such as `fetch_add` or CAS loops) across the consumer pool during block acquisition.
 * 
 * 2. **Generational Safety Check:** The worker maintains a strict tracking counter of its current 
 *    `reserved_ticket`. Before reading from a physical slot, it samples the slot's atomic sequence 
 *    using `memory_order_acquire`. If the sequence does not match the expected value (`reserved_ticket + 1`), 
 *    the worker loops in a low-latency spin-wait state (utilising `_mm_pause()`) until the producer 
 *    publishes the fresh payload.
 * 
 * 3. **Symmetric Handshake Rollover:** Immediately after the payload is extracted and passed to the 
 *    user's `Policy.Consume()` routine, the worker stores `reserved_ticket + RingBufferLen` back into the 
 *    slot's sequence tracker using `memory_order_release`. This signals to the producer that the slot has 
 *    been cleanly read and is available to be overwritten on its next lap around the ring buffer.
 *
 * @sa SPMCQueue
 * @sa SPMCProducer
 */

template <ConsumePolicy Policy, typename T>
class WorkerPool {
private:
    SPMCQueue<T>* queue;
    std::vector<std::thread> workers;
    std::vector<std::promise<std::uint32_t>> worker_promise;
    std::vector<std::future<std::uint32_t>> worker_future;
    std::size_t num_threads;
    Policy policy;
    
    // Aligned monitoring accumulator to aggregate local worker throughput
    alignas(64) std::atomic<float> global_sum{0.0f};

    // The core hot-path execution loop run by every thread in the pool
    void WorkerLoop(int worker_id, std::uint32_t num_threads, std::promise<std::uint32_t> output_promise, Policy& policy) {
        std::uint32_t count = 0;
        // 1. PIN THE THREAD
        // For performance determinism, pin each worker to its own isolated CPU core
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(3 + worker_id, &cpuset); // Pins to Cores 3, 4, 5, etc.
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

        float thread_local_sum = 0.0f;
        std::uint32_t block = 0;

        /*while (true) {
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
        }*/
        
        constexpr std::uint32_t LocalBatchSize = 128; // Reserve N slots at a time

        while (true) {
            // 1. BATCH RESERVATION: Fetch 64 slots at once, cutting atomic traffic by 64x
            //std::uint32_t base_ticket = queue->readIdx.fetch_add(LocalBatchSize, std::memory_order_acq_rel);
            std::uint32_t base_ticket = (block * num_threads + worker_id) * LocalBatchSize;
            
            // 2. Process the local batch sequentially
            for (std::uint32_t b = 0; b < LocalBatchSize; ++b) {
                std::uint32_t reserved_ticket = base_ticket + b;
                std::uint32_t slot_index = reserved_ticket % RingBufferLen;
                Slot<T>& target_slot = queue->store[slot_index];

                // 3. Generational Spin Wait (Stays local to this slot's cache line)
                std::uint32_t expected_sequence = reserved_ticket + 1;
                while (target_slot.sequence.load(std::memory_order_acquire) != expected_sequence) {
                    // Exit logic
                    if (queue->done.load(std::memory_order_acquire) && 
                        queue->writeIdx.load(std::memory_order_acquire) <= reserved_ticket) {
                        global_sum.fetch_add(thread_local_sum, std::memory_order_relaxed);
                        output_promise.set_value(count);
                        return;
                    }
                    _mm_pause();
                }

                // 4. Read data and release slot generation back to Producer
                //thread_local_sum += target_slot;
                policy.Consume(target_slot, thread_local_sum);
                count++;
                target_slot.sequence.store(reserved_ticket + RingBufferLen, std::memory_order_release);
            }
            block++;
        }
    }

public:
    WorkerPool(SPMCQueue<T>* q, std::size_t threads) 
        : queue(q)
        , num_threads(threads)
        , policy() 
    {
        workers.reserve(num_threads);
        worker_promise.reserve(num_threads);
        worker_future.reserve(num_threads);
        for(std::size_t i = 0; i < num_threads; ++i)
        {
            std::promise<std::uint32_t> p;
            worker_future.push_back(p.get_future());
            worker_promise.push_back(std::move(p));
        }
    }

    ~WorkerPool() {
        Shutdown();
    }

    // Spawns the worker threads and kicks off execution loops
    void Start() {
        for (std::size_t i = 0; i < num_threads; ++i) {
            workers.emplace_back(&WorkerPool::WorkerLoop, this, static_cast<int>(i), static_cast<std::uint32_t>(num_threads), std::move(worker_promise[i]), std::ref(policy));
        }
    }

    // Synchronizes the pool threads during teardown
    std::uint32_t Shutdown() {
        std::uint32_t ret = 0;
        int i = 0;
        for (auto& th : workers) {
            if (th.joinable()) {
                th.join();
                ret += worker_future[i].get();
                i++;
            }
        }
        workers.clear();
        worker_future.clear();
        return ret;
    }

    [[nodiscard]] float GetTotalSum() const noexcept {
        return global_sum.load(std::memory_order_relaxed);
    }
};
