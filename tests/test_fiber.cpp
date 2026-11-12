// test_fiber.cpp — the scheduler: order, interleaving, errors, scale.
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "fib/fiber.hpp"

using namespace fib;

TEST(Fiber, RunsEachFiberToCompletionInSpawnOrder) {
    Scheduler s;
    std::vector<int> order;
    for (int i = 1; i <= 3; ++i) s.spawn([&, i] { order.push_back(i); });
    EXPECT_EQ(s.ready_count(), 3u);
    s.run();
    EXPECT_EQ(order, (std::vector{1, 2, 3}));
    EXPECT_EQ(s.ready_count(), 0u);
    EXPECT_EQ(s.spawned(), 3u);
}

TEST(Fiber, YieldInterleavesRoundRobin) {
    Scheduler s;
    std::vector<std::string> log;
    s.spawn([&] { log.push_back("A1"); Scheduler::yield(); log.push_back("A2"); Scheduler::yield(); log.push_back("A3"); });
    s.spawn([&] { log.push_back("B1"); Scheduler::yield(); log.push_back("B2"); });
    s.run();
    EXPECT_EQ(log, (std::vector<std::string>{"A1", "B1", "A2", "B2", "A3"}));
}

TEST(Fiber, StepRunsOneSliceAtATime) {
    Scheduler s;
    int a = 0, b = 0;
    s.spawn([&] { a = 1; Scheduler::yield(); a = 2; });
    s.spawn([&] { b = 1; });
    EXPECT_TRUE(s.step());
    EXPECT_EQ(a, 1); EXPECT_EQ(b, 0);
    EXPECT_TRUE(s.step());
    EXPECT_EQ(b, 1);
    EXPECT_TRUE(s.step());
    EXPECT_EQ(a, 2);
    EXPECT_FALSE(s.step());
}

TEST(Fiber, CurrentAndYieldOutsideAFiber) {
    EXPECT_EQ(Scheduler::current(), nullptr);
    Scheduler::yield();  // a no-op, not a crash
    Scheduler s;
    Fiber* seen = nullptr;
    Fiber& f = s.spawn([&] { seen = Scheduler::current(); });
    s.run();
    EXPECT_EQ(seen, &f);
    EXPECT_EQ(f.state(), Fiber::State::Done);
    EXPECT_EQ(Scheduler::current(), nullptr);
}

TEST(Fiber, SpawnFromInsideAFiber) {
    Scheduler s;
    std::vector<int> order;
    s.spawn([&] {
        order.push_back(1);
        s.spawn([&] { order.push_back(3); });
        Scheduler::yield();
        order.push_back(4);
    });
    s.spawn([&] { order.push_back(2); });
    s.run();
    EXPECT_EQ(order, (std::vector{1, 2, 3, 4}));
}

TEST(Fiber, ExceptionIsRethrownByRunAfterOthersFinish) {
    Scheduler s;
    bool other_done = false;
    s.spawn([] { throw std::runtime_error("boom"); });
    s.spawn([&] { Scheduler::yield(); other_done = true; });
    EXPECT_THROW(s.run(), std::runtime_error);
    EXPECT_TRUE(other_done);
    EXPECT_FALSE(s.step());  // nothing is left half-run
}

TEST(Fiber, ValuesSurviveYields) {
    Scheduler s;
    std::uint64_t result = 0;
    double        dresult = 0;
    s.spawn([&] {
        std::uint64_t a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8;
        double x = 0.5, y = 1.5, z = 2.5, w = 3.5;
        for (int i = 0; i < 500; ++i) {
            a = a * 3 + b; b ^= c; c += d; d = d * 5 + e; e ^= f; f += g; g = g * 7 + h; h ^= a;
            x = x * 1.001 + y; y += z * 0.5; z = z * 0.999 + w; w += x * 1e-3;
            Scheduler::yield();
        }
        result = a ^ b ^ c ^ d ^ e ^ f ^ g ^ h;
        dresult = x + y + z + w;
    });
    s.spawn([&] { for (int i = 0; i < 500; ++i) { volatile double k = 3.14159 * i; (void)k; Scheduler::yield(); } });
    s.run();
    std::uint64_t a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8;
    double x = 0.5, y = 1.5, z = 2.5, w = 3.5;
    for (int i = 0; i < 500; ++i) {
        a = a * 3 + b; b ^= c; c += d; d = d * 5 + e; e ^= f; f += g; g = g * 7 + h; h ^= a;
        x = x * 1.001 + y; y += z * 0.5; z = z * 0.999 + w; w += x * 1e-3;
    }
    EXPECT_EQ(result, a ^ b ^ c ^ d ^ e ^ f ^ g ^ h);
    EXPECT_DOUBLE_EQ(dresult, x + y + z + w);
}

TEST(Fiber, TenThousandFibersWithSmallStacks) {
    Scheduler s;
    std::uint64_t sum = 0;
    for (int i = 0; i < 10'000; ++i)
        s.spawn([&, i] { Scheduler::yield(); sum += std::uint64_t(i); Scheduler::yield(); }, 16 * 1024);
    s.run();
    EXPECT_EQ(sum, 10'000ull * 9'999 / 2);
    EXPECT_EQ(s.switches(), 10'000u * 3u + 10'000u * 2u);  // 3 resumes + 2 yields each
}

namespace {
int depth(int n) {
    volatile char pad[64];  // make each frame cost something
    pad[0] = char(n);
    return n == 0 ? pad[0] : depth(n - 1) + 1;
}
}  // namespace

TEST(Fiber, RecursionUsesTheFibersOwnStack) {
    Scheduler s;
    int got = -1;
    s.spawn([&] { got = depth(2000); }, 512 * 1024);
    s.run();
    EXPECT_EQ(got, 2000);
}

TEST(Fiber, SchedulersAreIndependent) {
    Scheduler a, b;
    std::vector<char> log;
    a.spawn([&] { log.push_back('a'); Scheduler::yield(); log.push_back('A'); });
    b.spawn([&] { log.push_back('b'); Scheduler::yield(); log.push_back('B'); });
    a.step(); b.step(); a.step(); b.step();
    EXPECT_EQ(log, (std::vector<char>{'a', 'b', 'A', 'B'}));
}

TEST(Fiber, FinishedFibersStacksAreRecycled) {
    Scheduler s;
    s.spawn([] {}, 32 * 1024);
    s.run();
    EXPECT_EQ(s.pooled_stacks(), 1u);
    s.spawn([] {}, 16 * 1024);  // smaller request: the 32 KB stack is reused
    EXPECT_EQ(s.pooled_stacks(), 0u);
    s.run();
    EXPECT_EQ(s.pooled_stacks(), 1u);
    s.spawn([] {}, 64 * 1024);  // bigger than anything pooled: a new one
    EXPECT_EQ(s.pooled_stacks(), 1u);
    s.run();
    EXPECT_EQ(s.pooled_stacks(), 2u);
}
