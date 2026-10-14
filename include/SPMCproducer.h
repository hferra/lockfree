#pragma once

#include <thread>
#include <random>
#include <immintrin.h> // Required for _mm_pause

#include "SPMCqueue.h"

class SPMCProducer
{
    private: 
    SPMCQueue* queue;
    std::uint32_t count;
    static constexpr std::uint32_t rounds = 10000;
    static constexpr size_t delay = 100;


    public:
    SPMCProducer(SPMCQueue* q)
    : queue(q)
    , count(0)
    {}

    void Run()
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<float> dis(0.0f, 1.0f);

        constexpr std::uint32_t totalCount = RingBufferLen * rounds;
        
        for(std::uint32_t i = 0; i < totalCount; ++i)
        {
            float random_value = dis(gen);
            
            // 1. Map our absolute sequence number to the physical array slot
            std::uint32_t slot_index = count % RingBufferLen;
            Slot& target_slot = queue->store[slot_index];

            // 2. GENERATIONAL SAFETY BOUNDARY CHECK
            // Fetch the slot's current internal sequence ticket.
            std::uint32_t current_seq = target_slot.sequence.load(std::memory_order_acquire);

            // SPIN WAIT: Block if the slow worker thread has not updated it to our current turn target (current_seq != count).
            while (current_seq != count)
            {
                _mm_pause(); // Hint hardware pipelines during active polling loops
                current_seq = target_slot.sequence.load(std::memory_order_acquire);
            }

            // 3. SECURE PAYLOAD COMMIT
            // Direct implicit conversion via our zero-cost operator= wrapper
            target_slot = random_value; 

            // 4. TICKETING ADVANCEMENT HANDSHAKE
            // Advance the internal slot tracker to (count + 1). 
            // This signals to the spinning consumers that this specific slot data is fresh and ready.
            // memory_order_release guarantees the payload data is visible BEFORE the sequence shifts.
            target_slot.sequence.store(count + 1, std::memory_order_release);

            // 5. Publish global progress tracking boundary
            count++;
            queue->writeIdx.store(count, std::memory_order_release);
        }
    }

    private:
    // Forces the CPU to wait a specific number of loop iterations
    // without yielding control to the operating system.
    inline void producer_delay(size_t iterations) {
        for (size_t i = 0; i < iterations; ++i) {
            _mm_pause(); // Emits the hardware PAUSE instruction
        }
    }
};