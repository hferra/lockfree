// Copyright 2026 Herman L. Ferra
// SPDX-License-Identifier: Apache-2.0

#pragma once 
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <cstddef>
#include <concepts>

// Define the contract: Type T must have a public Clear() method that returns void
template<typename T>
concept Clearable = requires(T a) {
    { a.Clear() } -> std::same_as<void>;
};

template <typename P>
concept PoolPolicy = requires(P p) {
    { p.OnPopSnapshot() } noexcept;   // after reading next, before the CAS
    { p.OnCasRetry()    } noexcept;   // a CAS lost a race
    { p.OnExhausted()   } noexcept;   // Lease() found the pool empty
};

struct NoPolicy {
    void OnPopSnapshot() noexcept {}
    void OnCasRetry()    noexcept {}
    void OnExhausted()   noexcept {}
};


// Note the LockFreeObjectPool must outlive any object that it Leases 
template <Clearable T, std::size_t PoolSize, PoolPolicy Policy = NoPolicy>
class LockFreeObjectPool {
private:
    [[no_unique_address]] Policy policy_;
    static constexpr std::uint32_t kNull = 0xFFFFFFFFu;   // replaces size_t max

    static constexpr std::uint64_t Pack(std::uint32_t tag, std::uint32_t idx) {
        return (std::uint64_t{tag} << 32) | idx;           // tag in upper 32, index in lower 32
    }
    static constexpr std::uint32_t Index(std::uint64_t v) { return static_cast<std::uint32_t>(v & 0xFFFFFFFFu); }
    static constexpr std::uint32_t Tag(std::uint64_t v)   { return static_cast<std::uint32_t>(v >> 32); }

    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

    // Kept private: The client will never see this type
    struct UnalignedNode {
        T storage;             
        std::atomic<std::uint32_t> next;      
    };

    // If the node is already larger than or equal to a cache line (64 bytes),
    // we align it to its natural alignment. If it's smaller, we pad it to 64 bytes
    // to strictly prevent false sharing.
    static constexpr std::size_t Alignment = (sizeof(UnalignedNode) >= 64) 
                                             ? alignof(UnalignedNode) 
                                             : 64;

    struct alignas(Alignment) Node {
        T storage;             
        std::atomic<std::uint32_t> next;      
    };

    std::array<Node, PoolSize> pool;
    std::atomic<std::uint64_t> free_head{Pack(0, 0)};


    // Internal raw Return that works with the index cached in 'next'
    void ReturnNode(Node* node_ptr) {
        // Compute index via pointer arithmetic: avoids modifying node_ptr->next 
        // during Lease(), eliminating a concurrent read/write race on 'next'.
        const auto node_idx = static_cast<std::uint32_t>(node_ptr - pool.data());

        std::uint64_t current_head = free_head.load(std::memory_order_acquire);
        do {
            node_ptr->next.store(Index(current_head), std::memory_order_relaxed);
        } while (!free_head.compare_exchange_weak(current_head, Pack(Tag(current_head) + 1, node_idx), 
                                                  std::memory_order_release, 
                                                  std::memory_order_acquire));
    }

public:
    class Deleter {
    private:
        LockFreeObjectPool* pool_ptr;
        Node* node_ptr;

    public:
        Deleter() : pool_ptr(nullptr), node_ptr(nullptr) {}
        Deleter(LockFreeObjectPool* pool, Node* node) : pool_ptr(pool), node_ptr(node) {}

        void operator()(T* ptr) const noexcept {
            if (pool_ptr && node_ptr) {
                // Secure Sanitisation Phase
                ptr->Clear(); 

                // Recycle back into the lock-free array
                pool_ptr->ReturnNode(node_ptr);
            }
        }
    };

    const Policy& policy() const noexcept
    { return policy_; }

    // Clean alias for the client: behaves exactly like a standard unique_ptr<T>
    using ObjectPtr = std::unique_ptr<T, Deleter>;
    
    // Default constructor for default-initializable policies (including NoPolicy & PauseAPolicy)
    LockFreeObjectPool() requires std::default_initializable<Policy>
        : LockFreeObjectPool(std::in_place) {}


    // In-place constructor for non-movable/non-copyable policies with arguments
    template <typename... Args>
    explicit LockFreeObjectPool(std::in_place_t, Args&&... args)
        : policy_(std::forward<Args>(args)...)
    {
        static_assert(PoolSize > 0 && PoolSize < kNull);
        for (std::size_t i = 0; i < PoolSize - 1; ++i) {
            pool[i].next.store(static_cast<std::uint32_t>(i + 1), std::memory_order_relaxed);
        }
        pool[PoolSize - 1].next.store(kNull, std::memory_order_relaxed);
    }

    [[nodiscard]] ObjectPtr Lease() {
        std::uint64_t current_head = free_head.load(std::memory_order_acquire);
        while (Index(current_head) != kNull) {
            auto idx  = Index(current_head);
            auto next = pool[idx].next.load(std::memory_order_relaxed);

            policy_.OnPopSnapshot();
            
            if (free_head.compare_exchange_weak(current_head, Pack(Tag(current_head) + 1, next), 
                                                std::memory_order_acq_rel, 
                                                std::memory_order_acquire)) {
                Node* leased_node = &pool[idx];
                
                // Lock in its own index before handing it out
                leased_node->next.store(idx, std::memory_order_relaxed); 

                // Return the smart pointer wrapping the internal storage payload
                return ObjectPtr(&(leased_node->storage), Deleter(this, leased_node));
            }
            // note the CAS above can fail spuriously so if OnCasRetry implements a retry count
            // it could be an overestimate
            policy_.OnCasRetry(); 
        }
        policy_.OnExhausted();
        return nullptr; // Pool exhausted
    }
};
