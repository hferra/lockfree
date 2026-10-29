#include <atomic>
#include <cstdint>
#include <immintrin.h>

/*class SpinWaitBarrier
{
    private:
            alignas(64) std::atomic<std::uint32_t> arrivedCount{0};
            std::uint32_t waitForCount;
            alignas(64) std::atomic<std::uint32_t> finishedCount{0};

            // Explicit Gate Flag: 
            // false = Locked (Workers must wait here before starting a new phase)
            // true  = Open (Workers are free to register and enter)
            alignas(64) std::atomic<bool> gateOpen{true};

    public:
        SpinWaitBarrier(std::uint32_t count)
        : waitForCount(count)
        {
        }

        void wait()
        {
            // STEP 1: Explicit Pre-Check Gate
            // If a fast worker loops around too early, it spins here until the coordinator opens the gate
            while (!gateOpen.load(std::memory_order_acquire))
            {
                _mm_pause();
            }

            // STEP 2: Signal Arrival Atomically & Catch the Return Value
            // fetch_add returns the old value. If it was (waitForCount - 1), 
            // this thread is the one that pushes it to waitForCount.
            std::uint32_t previousArrivals = arrivedCount.fetch_add(1, std::memory_order_acq_rel);
                
            // If we are the final thread to arrive, close the entry gate behind us 
            if (previousArrivals + 1 == waitForCount)             
            {                 
                gateOpen.store(false, std::memory_order_release); // Use release to match the acquire gate
            }

            while (arrivedCount.load(std::memory_order_acquire) < waitForCount)
            {
                _mm_pause(); // Protects CPU pipeline and saves power
            }
            
            finishedCount.fetch_add(1, std::memory_order_release);
        }

        void reset()
        {
            std::uint32_t finished = finishedCount.load(std::memory_order_acquire);
            while(finished != waitForCount)
            {
                _mm_pause();
                finished = finishedCount.load(std::memory_order_acquire);
            }
            arrivedCount.store(0, std::memory_order_relaxed);
            finishedCount.store(0, std::memory_order_relaxed);

            // 3. Explicitly flip the gate flag back to true to unlock the next phase
            gateOpen.store(true, std::memory_order_release);
        }
};*/

#include <atomic>
#include <cstdint>
#include <immintrin.h>

class SpinWaitBarrier
{
    private:
        alignas(64) std::atomic<std::uint32_t> arrivedCount{0};
        std::uint32_t waitForCount;

        alignas(64) std::atomic<std::uint32_t> generation{0};

    public:
        SpinWaitBarrier(std::uint32_t count)
            : waitForCount(count)
        {
        }

        void wait()
        {
            // STEP 1: Capture the exact generation we are entering
            std::uint32_t currentGen = generation.load(std::memory_order_relaxed);

            // STEP 2: Register arrival and get our atomic arrival ticket
            std::uint32_t previousArrivals = arrivedCount.fetch_add(1, std::memory_order_acq_rel);
            
            // STEP 3: Check if WE are the final thread to arrive
            if (previousArrivals + 1 == waitForCount)             
            {                 
                // We are the final thread! We implicitly handle the reset duties:
                // 1. Reset the arrival counter for the NEXT generation
                arrivedCount.store(0, std::memory_order_relaxed);
                
                // 2. Increment the generation index to release all waiting threads.
                // A release fence ensures the store to arrivedCount happens before the generation flip.
                generation.fetch_add(1, std::memory_order_release);
            }
            else
            {
                // STEP 4: If we aren't the final thread, spin until the generation advances
                // This means the final thread has arrived and cleared the phase.
                while (generation.load(std::memory_order_acquire) == currentGen)
                {
                    _mm_pause();   
                }
            }
        }

        /*void reset()
        {
            // 1. Wait until all workers have completely checked out of the barrier phase
            while(finishedCount.load(std::memory_order_acquire) != waitForCount)
            {
                _mm_pause();
            }

            // 2. Safely reset counts while workers are safely trapped in Step 5 of wait()
            arrivedCount.store(0, std::memory_order_relaxed);
            finishedCount.store(0, std::memory_order_relaxed);

            // 3. Increment the generation. This releases the workers trapped in Step 5
            // and allows them to safely proceed to their next iteration.
            generation.fetch_add(1, std::memory_order_release);
        }*/    
};

