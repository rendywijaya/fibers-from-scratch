# fibers-from-scratch: a context switch by hand, and what it costs

User-space threads (fibers) built on a context switch written in assembly
for **AArch64** and **x86-64**, with a round-robin scheduler, guard-paged
stacks, AddressSanitizer support, and a C++20 coroutine alongside for
comparison. Every claim about cost is measured against the kernel's own
thread hand-off on the same machine.

## Results (Apple M2 Pro, Apple clang 21, `-O3`, median of 5 runs)

| what | ns | what it is |
|---|---:|---|
| **raw context switch** (`fib_switch`) | **11.6** | save callee-saved registers, swap the stack pointer, restore, `ret` |
| fiber `yield()` through the scheduler | 34.4 | two raw switches plus the run queue |
| C++20 coroutine resume + suspend | 2.0 | a call into the frame and a jump to the saved state; no registers to save |
| thread hand-off, spinning on an atomic | 35.1 | no kernel: one cache line moving between two cores |
| **thread hand-off, mutex + condition variable** | **1,332** | the kernel puts one thread to sleep and wakes the other |
| thread hand-off, one byte through a pipe | 1,609 | two system calls and a kernel context switch |
| `std::thread` create + join | 13,457 | `pthread_create`, a kernel stack, scheduling |
| fiber spawn + run, fresh stack | 3,664 | `mmap` + `mprotect` for the guard page |
| **fiber spawn + run, recycled stack** | **93.5** | no kernel: frame set-up, queue, two switches |

A fiber switch is **115× cheaper** than a kernel hand-off through a condition
variable, and spawning a fiber from the stack pool is **144× cheaper** than
creating a thread. The coroutine is 6× cheaper again than the fiber, and the
[design notes](docs/DESIGN.md#stackful-versus-stackless) say what it gives up
for that. Method and caveats: [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

## How it works, in one paragraph

A context switch is a function call that returns on a different stack. The
caller of `fib_switch` already assumes every caller-saved register is
clobbered, so the switch needs to preserve only what the ABI says a callee
must preserve: on AArch64 `x19`–`x28`, the frame pointer, the link register
and `d8`–`d15`; on x86-64 `rbx`, `rbp`, `r12`–`r15` and the floating-point
control words. Those are pushed onto the current stack, the stack pointer is
stored, the other context's stack pointer is loaded, its registers are
popped, and `ret` continues wherever that context last called `fib_switch`.
A fresh fiber's stack is pre-built to look exactly like that, with the return
address pointing at a trampoline that calls the fiber's function. Everything
else (scheduler, stack pool, sanitizer handshake, exceptions) is ordinary C++
on top. [docs/DESIGN.md](docs/DESIGN.md) has the frame layouts.

```cpp
fib::Scheduler s;
s.spawn([&] { produce(); fib::Scheduler::yield(); produce(); });
s.spawn([&] { consume(); fib::Scheduler::yield(); consume(); });
s.run();   // returns when every fiber has finished
```

## What is tested

* **The switch itself**, with no scheduler: bounce between two contexts;
  first-frame alignment; and twelve integer plus eight floating-point
  accumulators kept live across 100 switches on both sides, compared with a
  switch-free computation. A missing register in the assembly fails this.
* **Stacks**: whole pages plus a guard; touching the guard page faults
  (a death test).
* **The scheduler**: completion order, round-robin interleaving, stepping,
  `current()`, spawning from inside a fiber, exceptions rethrown by `run()`
  after the other fibers finish, 10,000 fibers on 16 KB stacks, deep
  recursion on a fiber stack, independent schedulers, stack recycling.
* **The coroutine**: laziness, values, exceptions.
* **Sanitizers**: the whole suite under ASan + UBSan, which requires the
  fiber-switch handshake to be right.
* **Both architectures in CI**: Linux x86-64 (GCC 14 and Clang 18) and macOS
  arm64 (Apple clang), each in Release and Sanitize mode.

## Build and run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build -j                   # 19 tests
./build/switch_bench --runs 5               # the table above
cmake -S . -B build-asan -DFIB_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
```

Requires CMake ≥ 3.20, a C++20 compiler, and an AArch64 or x86-64 machine
running Linux or macOS.

## Layout

```
src/switch_aarch64.S     the switch and the trampoline, AArch64 (AAPCS64)
src/switch_x86_64.S      the same for x86-64 (System V)
include/fib/context.hpp  Stack (mmap + guard page) and Context (sp, first frame, ASan handshake)
include/fib/fiber.hpp    Fiber and Scheduler (round robin, yield, stack pool, exceptions)
include/fib/coro.hpp     Generator<T>: the C++20 stackless coroutine, for comparison
tests/                   context, fiber, coro
bench/switch_bench.cpp   the nine measurements
docs/                    DESIGN, BENCHMARKS, LEARNING
results/                 raw benchmark output
```

## What this is not

Not preemptive: a fiber runs until it yields or returns, and a blocking system
call blocks the whole thread. Not multi-threaded: one scheduler per thread,
no work stealing. No I/O integration: a real runtime parks a fiber on an
event loop. Stacks are fixed-size. None of these are hard to add; all of them
are left out so that the thing being measured stays small enough to read in
one sitting.

## Documents

* [DESIGN.md](docs/DESIGN.md): the ABI, the frame layouts, the trampoline, stacks, the scheduler, exceptions, ASan, stackful vs stackless.
* [BENCHMARKS.md](docs/BENCHMARKS.md): what each row measures, the method, the caveats.
* [LEARNING.md](docs/LEARNING.md): reading order, exercises, interview questions.
