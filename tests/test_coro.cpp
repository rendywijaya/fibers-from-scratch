// test_coro.cpp — the C++20 generator used for comparison.
#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

#include "fib/coro.hpp"

using namespace fib;

namespace {
Generator<int> count_to(int n) {
    for (int i = 1; i <= n; ++i) co_yield i;
}
Generator<int> throws_after(int n) {
    for (int i = 1; i <= n; ++i) co_yield i;
    throw std::runtime_error("done badly");
}
}  // namespace

TEST(Generator, YieldsEachValueThenEnds) {
    auto g = count_to(5);
    std::vector<int> got;
    while (g.next()) got.push_back(g.value());
    EXPECT_EQ(got, (std::vector{1, 2, 3, 4, 5}));
}

TEST(Generator, IsLazy) {
    bool started = false;
    auto make = [&]() -> Generator<int> { started = true; co_yield 1; };
    auto g = make();
    EXPECT_FALSE(started);
    g.next();
    EXPECT_TRUE(started);
}

TEST(Generator, ExceptionsSurfaceFromNext) {
    auto g = throws_after(2);
    EXPECT_TRUE(g.next());
    EXPECT_TRUE(g.next());
    EXPECT_THROW(g.next(), std::runtime_error);
}
