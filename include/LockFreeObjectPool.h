#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <cstddef>
#include <concepts>

// Define the contract: Type T must have a public Clear() method that returns void
template<typename T>
concept Clearable = requires(T a) {
    { a.Clear() } -> std::same_as<void>;
};


template <Clearable T, std::size_t PoolSize>
class LockFreeObjectPool {
private:
    // Kept private: The client will never see this type
    struct UnalignedNode {
        T storage;             
        std::size_t next;      
    };

    // If the node is already larger than or equal to a cache line (64 bytes),
    // we align it to its natural alignment. If it's smaller, we pad it to 64 bytes
    // to strictly prevent false sharing.
    static constexpr std::size_t Alignment = (sizeof(UnalignedNode) >= 64) 
                                             ? alignof(UnalignedNode) 
                                             : 64;

    struct alignas(Alignment) Node {
        T storage;             
        std::size_t next;      
    };

    std::array<Node, PoolSize> pool;
    std::atomic<std::size_t> free_head{0};

    // Internal raw Return that works with the index cached in 'next'
    void ReturnNode(Node* node_ptr) {
        std::size_t node_idx = node_ptr->next; 

        std::size_t current_head = free_head.load(std::memory_order_acquire);
        do {
            node_ptr->next = current_head;
        } while (!free_head.compare_exchange_weak(current_head, node_idx, 
                                                  std::memory_order_release, 
                                                  std::memory_order_acquire));
    }

public:
    // Custom deleter that the unique_ptr will use automatically
    /*class Deleter {
    private:
        LockFreeObjectPool* pool_ptr;
        Node* node_ptr;

    public:
        Deleter() : pool_ptr(nullptr), node_ptr(nullptr) {}
        Deleter(LockFreeObjectPool* pool, Node* node) : pool_ptr(pool), node_ptr(node) {}

        // Triggered automatically when unique_ptr goes out of scope
        void operator()(T*) const {
            if (pool_ptr && node_ptr) {
                // You could optionally call the destructor of T here if needed:
                // ptr->~T();
                pool_ptr->ReturnNode(node_ptr);
            }
        }
    };*/
    class Deleter {
    private:
        LockFreeObjectPool* pool_ptr;
        Node* node_ptr;

    public:
        Deleter() : pool_ptr(nullptr), node_ptr(nullptr) {}
        Deleter(LockFreeObjectPool* pool, Node* node) : pool_ptr(pool), node_ptr(node) {}

        void operator()(T* ptr) const {
            if (pool_ptr && node_ptr) {
                // Secure Sanitisation Phase
                ptr->Clear(); 

                // Recycle back into the lock-free array
                pool_ptr->ReturnNode(node_ptr);
            }
        }
    };

    // Clean alias for the client: behaves exactly like a standard unique_ptr<T>
    using ObjectPtr = std::unique_ptr<T, Deleter>;

    LockFreeObjectPool() {
        for (std::size_t i = 0; i < PoolSize - 1; ++i) {
            pool[i].next = i + 1;
        }
        pool[PoolSize - 1].next = std::numeric_limits<std::size_t>::max();
    }

    ObjectPtr Lease() {
        std::size_t current_head = free_head.load(std::memory_order_acquire);
        while (current_head != std::numeric_limits<std::size_t>::max()) {
            std::size_t next_free = pool[current_head].next;
            
            if (free_head.compare_exchange_weak(current_head, next_free, 
                                                std::memory_order_release, 
                                                std::memory_order_acquire)) {
                Node* leased_node = &pool[current_head];
                
                // Lock in its own index before handing it out
                leased_node->next = current_head; 

                // Return the smart pointer wrapping the internal storage payload
                return ObjectPtr(&(leased_node->storage), Deleter(this, leased_node));
            }
        }
        return nullptr; // Pool exhausted
    }
};
