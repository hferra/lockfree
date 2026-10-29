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
        alignas(64) std::atomic<std::uint32_t> finishedCount{0};

        // Replace the boolean gate with an atomic generation index
        alignas(64) std::atomic<std::uint32_t> generation{0};

    public:
        SpinWaitBarrier(std::uint32_t count)
            : waitForCount(count)
        {
        }

        void wait()
        {
            // STEP 1: Capture the generation we are EXPECTING to process
            std::uint32_t currentGen = generation.load(std::memory_order_relaxed);

            // STEP 2: Register arrival
            arrivedCount.fetch_add(1, std::memory_order_acq_rel);
            
            // If we are the final thread to arrive, we do NOT change a gate flag.
            // We just wait for the master thread to tell us it's ready.
            
             // STEP 3: Spin with Oversubscription-Safe Yielding
            std::uint32_t spinCount = 0;
            while (arrivedCount.load(std::memory_order_acquire) < waitForCount)
            {
                if (spinCount < 2000) {
                    _mm_pause(); 
                }
                else {
                    // If we've spun 2000 times, someone else sharing our core is starving.
                    // Yielding here lets a starving thread hop onto a Core and register its arrival!
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    spinCount = 0;
                }
                spinCount++;
            }
            
            // STEP 4: Signal departure
            finishedCount.fetch_add(1, std::memory_order_release);

            // STEP 5: Prevention Gate
            // If a fast worker loops around too early, it will hit this loop.
            // Because 'generation' hasn't been incremented by the master yet,
            // the fast worker safely spins here, unable to touch arrivedCount prematurely.

            spinCount = 0;
            while (generation.load(std::memory_order_acquire) == currentGen)
            {
                if (spinCount < 2000) {
                    _mm_pause();
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    spinCount = 0;
                }
                spinCount++;
            }
        }

        void reset()
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
        }    
};

