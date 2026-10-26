#pragma once

#include <atomic>
#include <cstdint>
#include <array>

/**
 * @brief A lock-free, page-aligned data container structured as a bounded ring buffer.
 *
 * @tparam T The type of the payload element. Must be trivially copyable to preserve 
 *           the layout's flat, lock-free performance characteristics.
 *
 * The SPMCQueue represents a high-throughput, deterministic memory infrastructure. 
 * It functions as a passive, flat data layout consisting of sequential slots. 
 * The data structure itself does not dictate execution topology or thread constraints; 
 * synchronization and access semantics are entirely governed by how cooperating 
 * execution contexts manipulate the internal atomic sequence tokens.
 *
 * @section Memory Architecture & Cache Alignment
 * - Fixed ring buffer capacity of 1024 slots.
 * - Each Slot is strictly aligned to `CacheLineSize` (64 bytes). This completely 
 *   eliminates false sharing (cache line cross-talk) between adjacent array locations, 
 *   preventing reader-writer memory thrashing.
 * - The entire SPMCQueue structure is aligned to `PageSize` (4096 bytes). This 
 *   ensures the allocation aligns perfectly with OS hardware page boundaries, allowing 
 *   it to be pinned in physical RAM (e.g., via `mlock`) to completely eliminate 
 *   virtual memory page faults on the hot path.
 *
 * @section State Fields
 * - `writeIdx`: An atomic tracking index typically used to flag production progress.
 * - `done`: A global atomic flag to broadcast lifecycle tear-down signals.
 * - `store`: The flat, cache-isolated array holding the payload and sequence tickets.
 *
 * @sa SPMCProducer For the single-producer loop implementation details.
 * @sa WorkerPool   For the multi-consumer batch-reservation implementation details.
 */

constexpr std::size_t CacheLineSize = 64;
constexpr std::size_t PageSize = 4096; // Standard Linux page size
constexpr std::size_t RingBufferLen = 1024;

// Cache-aligned slot guarantees zero cross-talk between sequential indices
template <typename T>
struct alignas(CacheLineSize) Slot {
    T value{0.0f};
    std::atomic<std::uint32_t> sequence{0};

    // 1. Implicit constructor: Allows a Slot to be created/initialized from a passed in value
    Slot(T v = 0.0f) : value(v) {}

    // 2. Assignment operator: Allows directly assigning a value to an existing Slot
    Slot& operator=(T v) noexcept {
        value = v;
        return *this;
    }

    // 3. Conversion operator (Optional but highly recommended): 
    // Allows a Slot object to be implicitly treated as a T value when read (e.g., in the consumer)
    operator T() const noexcept { return value; }
};

template <typename T>
struct alignas(PageSize) SPMCQueue
{
    alignas(CacheLineSize) std::atomic<std::uint32_t> writeIdx{0};
    //alignas(CacheLineSize) std::atomic<std::uint32_t> readIdx{0};
    alignas(CacheLineSize) std::atomic<bool> done{false};
    
    // Flat, sequential, cache-isolated memory layout
    alignas(CacheLineSize) std::array<Slot<T>, RingBufferLen> store{};

    SPMCQueue()
    {
        for (std::uint32_t i = 0; i < RingBufferLen; ++i) {
            store[i].sequence.store(i, std::memory_order_relaxed);
        }
    }
};
