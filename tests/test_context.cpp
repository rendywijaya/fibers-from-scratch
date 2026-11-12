// test_context.cpp — the assembly and the first frame, with no scheduler.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "fib/context.hpp"

using namespace fib;

namespace {

// A coroutine-like entry that bounces back to `main_ctx` a few times.
Context main_ctx, co_ctx;
int     steps = 0;
bool    finished = false;

void bouncer(void* arg) {
    EXPECT_EQ(arg, reinterpret_cast<void*>(0x1234));
    for (int i = 0; i < 3; ++i) {
        ++steps;
        co_ctx.switch_to(main_ctx);
    }
    finished = true;
    co_ctx.switch_to(main_ctx, /*exiting=*/true);
}

}  // namespace

TEST(Context, SwitchBackAndForth) {
    Stack st(64 * 1024);
    steps = 0;
    finished = false;
    co_ctx.make(st, bouncer, reinterpret_cast<void*>(0x1234));
    for (int i = 1; i <= 3; ++i) {
        main_ctx.switch_to(co_ctx);
        EXPECT_EQ(steps, i);
        EXPECT_FALSE(finished);
    }
    main_ctx.switch_to(co_ctx);
    EXPECT_TRUE(finished);
}

// Each ABI has its own alignment rule, and the forged frame must satisfy it
// at the moment the trampoline starts:
//   AArch64: sp is a multiple of 16 at all times, so the saved sp is too.
//   x86-64:  rsp % 16 == 8 at a function's entry (the call pushed 8 bytes
//            onto an aligned stack). The return-address slot sits 56 bytes
//            above the saved rsp and must itself be 16-aligned, hence 8.
TEST(Context, FirstFrameIsAligned) {
    Stack st(16 * 1024);
    Context c;
    c.make(st, bouncer, nullptr);
    const auto sp = reinterpret_cast<std::uintptr_t>(c.sp);
#if defined(__aarch64__)
    EXPECT_EQ(sp % 16, 0u);
#elif defined(__x86_64__)
    EXPECT_EQ(sp % 16, 8u);
    EXPECT_EQ((sp + 56) % 16, 0u);  // the return-address slot
#endif
    EXPECT_LT(c.sp, st.top());
    EXPECT_GE(c.sp, st.bottom());
}

TEST(Stack, SizesAreWholePagesPlusAGuard) {
    Stack st(1000);
    EXPECT_EQ(st.usable() % st.page_size(), 0u);
    EXPECT_GE(st.usable(), 1000u);
    EXPECT_EQ(static_cast<const char*>(st.top()) - static_cast<const char*>(st.bottom()),
              static_cast<std::ptrdiff_t>(st.usable()));
    // The usable region is writable end to end.
    std::memset(const_cast<void*>(st.bottom()), 0xAB, st.usable());
}

TEST(StackDeathTest, TouchingTheGuardPageFaults) {
    Stack st(16 * 1024);
    volatile char* below = const_cast<volatile char*>(static_cast<const char*>(st.bottom())) - 1;
    EXPECT_DEATH({ *below = 1; }, "");
}

// Callee-saved registers, including the FP/SIMD ones, must survive a switch.
// Twelve integer and eight double accumulators are more than the caller-saved
// registers can hold, so some of them live in callee-saved registers across
// the switch, and a missing save/restore would corrupt the result.
namespace {
std::uint64_t int_result[12];
double        dbl_result[8];

void accumulator(void*) {
    std::uint64_t a0 = 1, a1 = 2, a2 = 3, a3 = 4, a4 = 5, a5 = 6, a6 = 7, a7 = 8, a8 = 9, a9 = 10, a10 = 11, a11 = 12;
    double d0 = 1.5, d1 = 2.5, d2 = 3.5, d3 = 4.5, d4 = 5.5, d5 = 6.5, d6 = 7.5, d7 = 8.5;
    for (int i = 0; i < 1000; ++i) {
        a0 = a0 * 3 + 1; a1 = a1 * 5 + 2; a2 ^= a1; a3 += a2; a4 = a4 * 7 + a3; a5 -= a4;
        a6 = a6 * 11 + a5; a7 += a6 ^ a0; a8 = a8 * 13 + a7; a9 ^= a8; a10 += a9; a11 = a11 * 17 + a10;
        d0 = d0 * 1.0001 + 0.5; d1 += d0 * 0.25; d2 = d2 * 0.999 + d1; d3 += d2; d4 = d4 * 1.001 - d3 * 1e-3;
        d5 += d4; d6 = d6 * 0.5 + d5; d7 += d6;
        if (i % 10 == 0) co_ctx.switch_to(main_ctx);  // 100 switches mid-computation
    }
    std::uint64_t ints[12] = {a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11};
    double        dbls[8] = {d0, d1, d2, d3, d4, d5, d6, d7};
    std::memcpy(int_result, ints, sizeof ints);
    std::memcpy(dbl_result, dbls, sizeof dbls);
    finished = true;
    co_ctx.switch_to(main_ctx, true);
}

// The same computation with no switches, for the expected values.
void reference(std::uint64_t* ints, double* dbls) {
    std::uint64_t a0 = 1, a1 = 2, a2 = 3, a3 = 4, a4 = 5, a5 = 6, a6 = 7, a7 = 8, a8 = 9, a9 = 10, a10 = 11, a11 = 12;
    double d0 = 1.5, d1 = 2.5, d2 = 3.5, d3 = 4.5, d4 = 5.5, d5 = 6.5, d6 = 7.5, d7 = 8.5;
    for (int i = 0; i < 1000; ++i) {
        a0 = a0 * 3 + 1; a1 = a1 * 5 + 2; a2 ^= a1; a3 += a2; a4 = a4 * 7 + a3; a5 -= a4;
        a6 = a6 * 11 + a5; a7 += a6 ^ a0; a8 = a8 * 13 + a7; a9 ^= a8; a10 += a9; a11 = a11 * 17 + a10;
        d0 = d0 * 1.0001 + 0.5; d1 += d0 * 0.25; d2 = d2 * 0.999 + d1; d3 += d2; d4 = d4 * 1.001 - d3 * 1e-3;
        d5 += d4; d6 = d6 * 0.5 + d5; d7 += d6;
    }
    std::uint64_t i2[12] = {a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11};
    double        d2v[8] = {d0, d1, d2, d3, d4, d5, d6, d7};
    std::memcpy(ints, i2, sizeof i2);
    std::memcpy(dbls, d2v, sizeof d2v);
}
}  // namespace

TEST(Context, CalleeSavedRegistersSurviveSwitches) {
    Stack st(64 * 1024);
    finished = false;
    co_ctx.make(st, accumulator, nullptr);
    // The main side also keeps live values across its switches.
    std::uint64_t m0 = 7, m1 = 11, m2 = 13, m3 = 17, m4 = 19, m5 = 23, m6 = 29, m7 = 31;
    double        f0 = 0.1, f1 = 0.2, f2 = 0.3, f3 = 0.4;
    int           n = 0;
    while (!finished) {
        main_ctx.switch_to(co_ctx);
        m0 += m1; m1 ^= m2; m2 += m3; m3 *= 3; m4 += m5; m5 ^= m6; m6 += m7; m7 *= 5;
        f0 += f1; f1 *= 1.01; f2 += f0; f3 -= f2 * 1e-4;
        ++n;
    }
    EXPECT_EQ(n, 101);  // 100 mid-loop switches + the final one
    std::uint64_t ei[12];
    double        ed[8];
    reference(ei, ed);
    for (int i = 0; i < 12; ++i) EXPECT_EQ(int_result[i], ei[i]) << "int acc " << i;
    for (int i = 0; i < 8; ++i) EXPECT_DOUBLE_EQ(dbl_result[i], ed[i]) << "double acc " << i;
    // And the main side's values: recompute them the same way.
    std::uint64_t r0 = 7, r1 = 11, r2 = 13, r3 = 17, r4 = 19, r5 = 23, r6 = 29, r7 = 31;
    double        g0 = 0.1, g1 = 0.2, g2 = 0.3, g3 = 0.4;
    for (int i = 0; i < 101; ++i) {
        r0 += r1; r1 ^= r2; r2 += r3; r3 *= 3; r4 += r5; r5 ^= r6; r6 += r7; r7 *= 5;
        g0 += g1; g1 *= 1.01; g2 += g0; g3 -= g2 * 1e-4;
    }
    EXPECT_EQ(m0, r0); EXPECT_EQ(m1, r1); EXPECT_EQ(m2, r2); EXPECT_EQ(m3, r3);
    EXPECT_EQ(m4, r4); EXPECT_EQ(m5, r5); EXPECT_EQ(m6, r6); EXPECT_EQ(m7, r7);
    EXPECT_DOUBLE_EQ(f0, g0); EXPECT_DOUBLE_EQ(f1, g1); EXPECT_DOUBLE_EQ(f2, g2); EXPECT_DOUBLE_EQ(f3, g3);
}
