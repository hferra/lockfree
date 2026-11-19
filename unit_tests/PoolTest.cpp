#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include "LockFreeObjectPool.h"


class PoolTest : public ::testing::Test
{
    public:
    struct PauseAPolicy {
        mutable std::atomic<bool> a_paused{false}, b_done{false};
        static inline thread_local bool is_thread_a = false;

        void OnPopSnapshot() noexcept {
            if (!is_thread_a) return;      // B's own leases pass straight through
            is_thread_a = false;           // pause only once
            a_paused.store(true);
            while (!b_done.load()) std::this_thread::yield();
        }
        void OnCasRetry()  noexcept {}
        void OnExhausted() noexcept {}
    };
};

struct Obj
{
    int id = 0;
    void Clear() {id = 0;}
};



TEST_F(PoolTest, ABA)
{
    LockFreeObjectPool<Obj, 4, PauseAPolicy> pool;
    using P = decltype(pool)::ObjectPtr;
    P a, b0, b1;

    std::thread tA([&pool, &a]{ pool.policy().is_thread_a = true; a = pool.Lease();});
    std::thread tB([&pool, &b0, &b1] {
        while (!pool.policy().a_paused.load()) std::this_thread::yield();
        b0 = pool.Lease();
        b1 = pool.Lease();
        b0.reset();
        pool.policy().b_done.store(true);
    });
    tA.join();
    tB.join();
    P c = pool.Lease();

    ASSERT_NE(a.get(), nullptr);
    ASSERT_NE(b1.get(), nullptr);
    ASSERT_NE(c.get(), nullptr);

    ASSERT_NE(a.get(), b1.get());
    ASSERT_NE(a.get(), c.get());
    ASSERT_NE(b1.get(), c.get());
}

