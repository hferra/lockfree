/*#pragma once

#include <atomic>
#include <vector>
#include <cstdint>

constexpr std::size_t RingBufferLen = 1024;
struct Queue
{
    alignas(64) std::atomic<std::uint32_t> writeIdx = 0;
    alignas(64) std::atomic<std::uint32_t> readIdx = 0;
    alignas(64) std::atomic<bool> done = false;
    std::vector<float> store = std::vector<float>(RingBufferLen, 0.0f);
};*/


#pragma once

#include <atomic>
#include <cstdint>
#include <array>
#include "scoped_memory_lock.h"

constexpr std::size_t CacheLineSize = 64;
constexpr std::size_t PageSize = 4096; // Standard Linux page size
constexpr std::size_t RingBufferLen = 1024;

// Cache-aligned slot guarantees zero cross-talk between sequential indices
struct alignas(CacheLineSize) Slot {
    float value{0.0f};

    // 1. Implicit constructor: Allows a Slot to be created/initialized from a float
    Slot(float v = 0.0f) : value(v) {}

    // 2. Assignment operator: Allows directly assigning a float to an existing Slot
    Slot& operator=(float v) {
        value = v;
        return *this;
    }

    // 3. Conversion operator (Optional but highly recommended): 
    // Allows a Slot object to be implicitly treated as a float when read (e.g., in the consumer)
    operator float() const { return value; }
};

struct alignas(PageSize) Queue
{
    alignas(CacheLineSize) std::atomic<std::uint32_t> writeIdx{0};
    alignas(CacheLineSize) std::atomic<std::uint32_t> readIdx{0};
    alignas(CacheLineSize) std::atomic<bool> done{false};
    
    // Flat, sequential, cache-isolated memory layout
    alignas(CacheLineSize) std::array<Slot, RingBufferLen> store{};
};
