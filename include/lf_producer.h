#pragma once

#include <thread>
#include <random>
#include <immintrin.h> // Required for _mm_pause

#include "lf_queue.h"

class Producer
{
    private: 
    Queue* queue;
    std::uint32_t count;
    static constexpr std::uint32_t rounds = 10000;
    static constexpr size_t delay = 100;


    public:
    Producer(Queue* q)
    : queue(q)
    , count(0)
    {}

    void Run()
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<float> dis(0.0f, 1.0f);

        constexpr std::uint32_t totalCount = RingBufferLen * rounds;
        std::uint32_t cachedReadIdx = 0;
        
        for(std::uint32_t i = 0; i < totalCount; ++i)
        {
            float random_value = dis(gen);
            count++;
            // SPIN WAIT: Re-load the atomic readIdx inside the loop condition
            // This ensures the producer actively pulls the consumer's cache line
            std::int32_t signed_distance = static_cast<std::int32_t>(count - cachedReadIdx);
            // Correct HFT Loop: Only stall the pipeline if the queue is strictly full (distance >= 1024)
            while (signed_distance >= static_cast<std::int32_t>(RingBufferLen))
            {
                cachedReadIdx = queue->readIdx.load(std::memory_order_acquire);
                signed_distance = static_cast<std::int32_t>(count - cachedReadIdx);
                
                if (signed_distance >= static_cast<std::int32_t>(RingBufferLen)) {
                    _mm_pause(); 
                }
            }

            // Guaranteed safe to write now: data race/corruption is physically impossible
            queue->store[count % RingBufferLen] = random_value;

            // Release order guarantees 'store' is updated before writeIdx becomes visible
            queue->writeIdx.store(count, std::memory_order_release);
            
            //producer_delay(delay);
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