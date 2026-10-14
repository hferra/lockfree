#pragma once

#include <thread>
#include <immintrin.h> // Required for _mm_pause


#include "lf_queue.h"

class Consumer
{
    private: 
    Queue* queue;
    std::uint32_t count;
    float sum;


    public:
    Consumer(Queue* q)
    : queue(q)
    , count(0)
    , sum(0.0f)
    {}

    void Run()
    {
    /*    while(true)
        {
            if(queue->writeIdx.load(std::memory_order_acquire) > count)
            {
                count++;
                auto val = queue->store[count % RingBufferLen];
                queue->readIdx.store(count, std::memory_order_release);
                sum += val;
                _mm_pause();
            }
            // If the queue is empty, check if the producer has shut down
            else if (queue->done.load(std::memory_order_acquire)) {
                break; // Exit loop naturally
            }
            else
            {
                // Optional: Hint the CPU pipeline while spinning empty 
                // to optimize power/core resources during the test
                _mm_pause(); 
            }
        }
    }*/

        // Batching threshold: stops cross-core cache invalidation spams
        constexpr std::uint32_t BatchSize = 32;
        std::uint32_t uncommittedReads = 0;

        while(true)
        {
            std::uint32_t currentWriteIdx = queue->writeIdx.load(std::memory_order_acquire);

            if(currentWriteIdx > count)
            {
                count++;
                sum += queue->store[count % RingBufferLen]; 
                uncommittedReads++;

                if (uncommittedReads >= BatchSize) {
                    queue->readIdx.store(count, std::memory_order_release);
                    uncommittedReads = 0;
                }
                _mm_pause();
            }
            else if (queue->done.load(std::memory_order_acquire)) {
                if (uncommittedReads > 0) {
                    queue->readIdx.store(count, std::memory_order_release);
                }
                break; 
            }
            else
            {
                if (uncommittedReads > 0) {
                    queue->readIdx.store(count, std::memory_order_release);
                    uncommittedReads = 0;
                }
                _mm_pause(); 
            }
        }
    }
};