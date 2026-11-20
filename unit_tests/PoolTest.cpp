// Copyright 2026 Herman L. Ferra
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <unordered_set>
#include <vector>
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

// Payload for the contention test: each lessee writes a token unique to it and checks, after
// yielding, that nobody else touched the object while it was held.
struct TokenObj
{
    long token = 0;
    void Clear() { token = 0; }
};

// A sanity check under real contention, not the guard for the ABA bug (PoolTest.ABA is that).
// It is most useful when built with ThreadSanitizer (-DENABLE_TSAN=ON).
TEST_F(PoolTest, ExclusiveOwnershipUnderContention)
{
    constexpr int kThreads = 8;
    constexpr int kIterations = 50'000;
    LockFreeObjectPool<TokenObj, 4> pool;
    std::atomic<long> violations{0}, leases{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&pool, &violations, &leases, t] {
            for (int i = 0; i < kIterations; ++i)
            {
                auto obj = pool.Lease();
                if (!obj) continue;               // pool momentarily empty: more threads than nodes
                const long token = (static_cast<long>(t) << 32) | i;
                obj->token = token;
                std::this_thread::yield();        // widen the window in which an overlap would show
                if (obj->token != token) ++violations;
                ++leases;
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(violations.load(), 0);
    EXPECT_GT(leases.load(), 0);

    // all four nodes must still be recoverable, and distinct
    using P = decltype(pool)::ObjectPtr;
    std::vector<P> held;
    std::unordered_set<const TokenObj*> distinct;
    for (int i = 0; i < 4; ++i)
    {
        held.push_back(pool.Lease());
        ASSERT_NE(held.back().get(), nullptr);
        distinct.insert(held.back().get());
    }
    EXPECT_EQ(distinct.size(), 4u);
    auto extra = pool.Lease();
    EXPECT_EQ(extra.get(), nullptr);              // pool is empty again
}
