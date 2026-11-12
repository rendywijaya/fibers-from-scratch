# Learning guide: own this project

This is the "small second project" of the HFT roadmap (Module 7, lesson 13:
fibers and coroutines, a context switch by hand). It is small on purpose:
about 150 lines of assembly and 300 of C++. The point is that you can explain
every instruction of the switch, say what the kernel does that it does not,
and defend the measurement.

## Why it matters for low-latency roles

Trading systems are threads passing data to each other, and the cost of
moving control between them is on the critical path. Engineers who work on
those systems are expected to know, concretely, what a context switch is,
what it costs in user space and in the kernel, why, and when to use fibers,
coroutines, or neither. This project is the smallest piece of code that lets
you answer all of that from your own measurements.

## Reading order (about five sessions)

| # | Read | You can answer… |
|---|---|---|
| 1 | `docs/DESIGN.md` §§ "The one idea", "What must be saved" | Why only callee-saved registers? Why is a switch "a function call that returns on a different stack"? |
| 2 | `src/switch_aarch64.S` and `src/switch_x86_64.S`, one instruction at a time | What does each line do? Why `stp d8, d9` and not `q8`? Why save MXCSR? Why 16-byte alignment, and where is it enforced? |
| 3 | `include/fib/context.hpp`, `src/context.cpp` | How does a fresh stack get its first frame? What is the trampoline for? What does the guard page protect against? |
| 4 | `include/fib/fiber.hpp`, `src/fiber.cpp`, `tests/test_fiber.cpp` | Why two switches per yield? Where do exceptions go? How do stacks get recycled? |
| 5 | `include/fib/coro.hpp`, `bench/switch_bench.cpp`, `docs/BENCHMARKS.md` | Why can a coroutine be 6× cheaper? Why did the first coroutine measurement read 0.0 ns? What is the spin row a control for? |

Then do the ASan part of session 3 again: `src/asan.hpp` and the two hooks
in `context.cpp`. Being able to explain why a stack switch confuses a
sanitizer, and how the handshake fixes it, is a senior-engineer signal.

## Exercises

1. **Symmetric switching.** Let a fiber switch directly to the next ready
   fiber instead of going through the scheduler. One switch per yield instead
   of two. Measure; expect the yield row to fall toward the raw-switch row.
2. **Inline the switch.** Rewrite `fib_switch` as GCC/Clang inline assembly
   inside `switch_to`. Does it get faster? What did you have to tell the
   compiler about clobbers, and why is that list the same as the register
   table?
3. **A fiber-aware channel.** A bounded SPSC queue where `push` on a full
   queue and `pop` on an empty one call `yield()` instead of spinning or
   blocking. Then a producer-consumer pipeline of five fibers. This is
   Module 7 lesson 9's ring buffer, used from fibers.
4. **Sleep.** `Scheduler::sleep_for(ns)`: park the fiber in a timer heap,
   and have `run()` check the heap before the ready queue. Measure how late
   wake-ups are.
5. **Break it.** Remove one `stp`/`ldp` pair from the AArch64 switch (or one
   `push`/`pop` from x86-64) and run `test_context`. Which register did it
   catch, and how? Then remove the MXCSR save, set a different rounding mode
   in one fiber, and show the leak with a test.
6. **Windows x64.** Write `switch_win_x64.asm`: more callee-saved registers
   (`rdi`, `rsi`, `xmm6`–`xmm15`), and the TIB's stack limits have to be
   swapped too. Why does Windows need that and System V does not?
7. **Linux, pinned.** Run `switch_bench` on Linux with `taskset -c 3`, then
   with the two threads of the hand-off rows pinned to two cores on the same
   L2, then on different sockets if you have them. Explain each change.
8. **Preemption.** A `SIGALRM`-driven timer that forces a switch from a
   signal handler (on an alternate signal stack). What is now unsafe that
   was safe before, and why do most fiber libraries refuse to do this?

## Interview questions, with answers to say out loud

**"What is a context switch, exactly?"**
> Saving the registers that define one flow of control and loading another's,
> then continuing on the other's stack. In the kernel it saves everything,
> because the interrupted code didn't agree to stop. In user space, at a
> point the code chose, it's a function call that returns on a different
> stack, so only the callee-saved registers need saving. That's why mine is
> about twenty instructions and 11.6 ns on an M2, and a kernel hand-off
> through a condition variable is 1.3 µs on the same machine.

**"Why only the callee-saved registers?"**
> Because the caller of the switch already assumes every caller-saved
> register is destroyed by any call. The ABI contract is the contract. On
> AArch64 that's x19 to x28, the frame pointer, the link register and the low
> halves of v8 to v15. On x86-64 System V it's rbx, rbp, r12 to r15, plus the
> MXCSR control bits and the x87 control word, which people forget: a fiber
> that changes the rounding mode mustn't leak it.

**"How does a fiber start the first time, if it never called the switch?"**
> I build a frame on its new stack that looks exactly like one the switch
> would have pushed: the return-address slot points at a trampoline, and two
> callee-saved slots carry the context pointer and the entry function. The
> first switch into it pops that frame and `ret`s into the trampoline, which
> calls the entry. The entry never returns; its last act is to switch away.
> And the frame is placed so the stack is correctly aligned at the call,
> which is a hardware fault on ARM if you get it wrong.

**"Why are fibers faster than threads? Is it just the system call?"**
> The system call is part of it. A kernel hand-off also runs the scheduler,
> saves every register including the vector ones, may switch page tables,
> sends an interrupt to wake the other core, and then that core runs with
> cold caches. A fiber switch does none of that because both sides consented
> to it. The honest control is my spinning hand-off between two threads with
> no kernel involvement at all: 35 ns, which is just a cache line crossing
> cores. A fiber switch on one core is cheaper than any cross-core
> communication.

**"Stackful or stackless? Which would you use?"**
> A fiber has its own stack, so anything in the call chain can suspend and
> the code reads as ordinary blocking code; the price is a stack per fiber
> and 11 ns per switch. A C++20 coroutine has no stack; the compiler turns
> one function into a state machine with a heap frame; it resumes in 2 ns
> but only that function can suspend, and `co_await` spreads through every
> caller. I'd use fibers to run blocking-style strategy code on one thread
> and coroutines for new asynchronous paths where the small frames matter.

**"What's the catch with fibers?"**
> No preemption: a fiber that spins or makes a blocking system call stalls
> every fiber on that thread. Fixed stack sizes: too small faults, too big
> wastes memory. Thread-local storage is per kernel thread, not per fiber,
> so a fiber that migrates between threads sees the wrong TLS. Sanitizers
> and debuggers need to be told about the stack switch. And exceptions must
> never unwind through the switch.

**"How did you handle AddressSanitizer?"**
> ASan tracks the stack, so a hand-made switch needs a handshake:
> `start_switch_fiber` before leaving a stack, `finish_switch_fiber` on
> arrival. The subtle case is the first entry into a fresh fiber, which
> doesn't return from the switch; the finish call lives in the entry shim.
> That call also reports the bounds of the stack you came from, which is how
> the scheduler's own context learns its stack without asking pthread. I
> also had to build GoogleTest from source in sanitized builds; a prebuilt
> one gives false container-overflow reports.

**"Can you trust your benchmark?"**
> Each row is N operations between two clock reads, so the clock's
> resolution doesn't matter. Median of five, best shown beside it. The
> coroutine row first came out at 0.0 ns because the optimizer folded the
> whole loop; I made the coroutine `noinline` and put a barrier on each
> value. No core pinning on macOS, and the M2 has two kinds of cores, so I
> report that. The ratios are the robust result; the absolute values are
> for this machine.

**"What would you do next?"**
> Symmetric switching to halve the yield cost, a fiber-aware SPSC channel so
> fibers can form a pipeline, and parking fibers on an event loop so blocking
> I/O doesn't stall the thread. Then put it under the trading path from my
> other project and see whether the strategy thread wants fibers or
> coroutines.

## Words to know cold
callee-saved / caller-saved, ABI (AAPCS64, System V), frame pointer, link
register, stack alignment, trampoline, guard page, `mmap`/`mprotect`,
cooperative vs preemptive, stackful vs stackless, coroutine frame, HALO
(heap allocation elision), futex, condition variable, cache-line transfer,
inter-processor interrupt, TLB, unwind / CFI, `thread_local`,
`__sanitizer_start_switch_fiber`.
