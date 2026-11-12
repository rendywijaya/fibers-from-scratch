# Design

## The one idea

A thread's state is its registers and its stack. A context switch saves one
thread's registers, points the CPU at another thread's stack, and restores
that thread's registers. The kernel does this for every thread on a timer
interrupt or a blocking system call, and it must save *everything*, because
the interrupted code did not expect to stop.

A fiber switch is cooperative: it happens only where the code calls
`fib_switch()`. From the caller's point of view, that is a function call.
Function calls already have a rule about registers: the **caller** assumes
every *caller-saved* register is destroyed by the call, and the **callee**
must preserve every *callee-saved* register. So a switch that looks like a
function call only has to save the callee-saved set. That is why the whole
thing is about twenty instructions.

## What must be saved

| | AArch64 (AAPCS64) | x86-64 (System V) |
|---|---|---|
| general registers | `x19`–`x28` | `rbx`, `rbp`, `r12`–`r15` |
| frame / link | `x29` (fp), `x30` (lr) | `rbp` is the frame pointer; the return address is on the stack |
| stack pointer | `sp` | `rsp` |
| floating point / SIMD | low 64 bits of `v8`–`v15` (`d8`–`d15`) | none of the vector registers; only the **MXCSR** control bits and the **x87 control word** |
| frame size in this project | 160 bytes | 64 bytes including the return address |

Two details are easy to get wrong and are the reason the register test
exists. On AArch64 only the *bottom half* of `v8`–`v15` is callee-saved, so
`stp d8, d9` is correct and `stp q8, q9` would be wasted work. On x86-64 the
control words are callee-saved even though the vector registers are not: a
fiber that changed the rounding mode must not leak it into another fiber.

## The switch

```
fib_switch(void** save_sp, void* load_sp):
    push callee-saved registers            (onto the current stack)
    *save_sp = sp
    sp = load_sp
    pop callee-saved registers             (from the other stack)
    ret                                    (to wherever that context called fib_switch)
```

`ret` is what makes it work: the other context is suspended *inside its own
call to `fib_switch`*, so returning from `fib_switch` on its stack continues it
exactly where it stopped. Neither side sees anything unusual; each thinks it
called a function that took a while.

### Frame layouts

AArch64, 160 bytes from the saved `sp` upward:

```
 +0  x19 x20   +16 x21 x22   +32 x23 x24   +48 x25 x26   +64 x27 x28
+80  x29 x30   +96 d8 d9    +112 d10 d11  +128 d12 d13  +144 d14 d15
```

x86-64, 64 bytes from the saved `rsp` upward:

```
 +0  mxcsr (4) | x87 cw (2) | pad     +8 r15   +16 r14   +24 r13   +32 r12
+40  rbx   +48 rbp   +56 return address
```

### Alignment

AArch64 requires `sp` to be 16-byte aligned at all times; the hardware
faults on a misaligned stack access. 160 is a multiple of 16, so a frame
pushed on an aligned stack stays aligned. x86-64 requires `rsp % 16 == 0` at
every `call` instruction, so `rsp % 16 == 8` at every function entry.
`Context::make()` therefore places a fresh frame's return-address slot at a
16-byte-aligned address, which makes the trampoline start like any normally
called function, and the trampoline subtracts 8 before its own `call` to keep
the rule.

## A fresh fiber: the first frame and the trampoline

A fiber that has never run has no "place it called `fib_switch` from". So
`Context::make()` writes a frame onto the new stack that looks exactly like
one `fib_switch` would have pushed:

* the return-address slot (`x30` on AArch64, the slot above the frame on
  x86-64) holds the address of **`fib_trampoline`**;
* one callee-saved register slot (`x19` / `r12`) holds the `Context*`;
* another (`x20` / `r13`) holds the address of **`fib_context_entry`**;
* everything else is zero, and the x86-64 control words hold their defaults
  (`mxcsr = 0x1F80`, `fcw = 0x037F`).

The first `fib_switch` into this context pops those values and `ret`s into
the trampoline, which moves the `Context*` into the first-argument register
and calls `fib_context_entry`. That function completes the sanitizer
handshake (below) and calls the user's entry with its argument.

The entry never returns. A fiber's last act is to switch away with
`exiting = true`; if the entry did return, the trampoline's next instruction
is `brk #0` / `ud2`, which traps loudly instead of jumping into zeros.

The trampoline is marked `.cfi_undefined x30` / `.cfi_undefined rip`. That
tells unwinders and debuggers there is no caller above it, so a backtrace
from inside a fiber stops there cleanly instead of walking off the end of the
stack.

## Stacks

Each fiber's stack is `mmap`'d, rounded up to whole pages, with the lowest
page set to `PROT_NONE` by `mprotect`. Running off the end of the stack then
faults on the guard page instead of silently corrupting whatever happens to
be mapped below. `Stack` is move-only and unmaps in its destructor. Default
size 64 KB; the tests use 16 KB for ten thousand fibers and 512 KB for deep
recursion.

Those two system calls are the whole cost of spawning a fiber: 3.7 µs fresh,
93 ns from the pool. The scheduler keeps the stacks of finished fibers and
reuses the most recently returned one that is big enough, which is also the
one most likely to still be in cache and in the TLB.

## The scheduler

Round robin over a `std::deque<Fiber*>`. `run()` pops the front, switches to
it, and when control comes back either re-queues it (it yielded) or recycles
its stack (it finished). `yield()` switches from the fiber to the scheduler's
own context, which is the thread's ordinary stack at the point where `run()`
is executing.

That is an **asymmetric** design: every yield is two switches, fiber →
scheduler → next fiber, and the measurement shows it (34 ns per yield against
11.6 ns per raw switch; the rest is the deque and the state updates). A
**symmetric** design switches fiber → fiber directly, one switch per yield,
at the price of every fiber knowing how to pick its successor. The asymmetric
one is simpler to reason about and is what most fiber libraries do;
LEARNING.md exercise 1 is to build the other and measure the difference.

## Exceptions

An exception thrown inside a fiber unwinds the fiber's own stack and is
caught in `Fiber::entry`, stored as an `exception_ptr`, and rethrown by
`run()` after every other fiber has finished. Nothing may unwind *through* a
switch: the frames below `fib_context_entry` are the trampoline's, which has
no unwind information on purpose. The same rule holds for any fiber library:
catch at the top of the fiber, never let it escape.

## AddressSanitizer

ASan keeps a shadow of the stack and a per-thread idea of where the stack
is. A hand-made stack switch confuses both unless the program announces it.
The protocol is two calls around every switch:

```
__sanitizer_start_switch_fiber(&fake_stack_save, to.stack_bottom, to.stack_size);
fib_switch(...);
__sanitizer_finish_switch_fiber(fake_stack_save, &from_bottom, &from_size);
```

The subtlety is the first switch into a *fresh* context: it does not return
from `fib_switch`, it starts in `fib_context_entry`, which is why the
`finish` call for that case lives there. As a side effect, `finish` reports
the bounds of the stack the switch came from, and that is how the scheduler's
own context (the thread's ordinary stack) learns its bounds without asking
`pthread`. A leaving fiber passes `nullptr` as the save slot so ASan stops
tracking a stack that is about to be recycled.

In an ordinary build all of this compiles to nothing. In a sanitized build,
getting it wrong produces "starting fiber switch while in fiber switch" or
false stack-buffer-overflow reports, and the CI matrix runs the whole suite
under ASan + UBSan on both architectures to keep it right.

## Stackful versus stackless

| | fiber (stackful) | C++20 coroutine (stackless) |
|---|---|---|
| where its state lives | its own stack | a heap-allocated frame holding the locals of one function |
| who can suspend | any function in the call chain | only the coroutine function itself |
| what a switch does | saves and restores registers, swaps `sp` | stores an integer state, returns; resume calls through the frame |
| measured cost | 11.6 ns switch, 34 ns yield | 2.0 ns resume + suspend |
| memory | a stack per fiber (KBs) | a frame per coroutine (bytes to hundreds of bytes) |
| what it costs the code | nothing: ordinary blocking-style code | `co_await` spreads through every function on the path |

The coroutine wins on cost because it has nothing to save: the compiler
turned the function into a state machine, so the "registers" are already in
the frame. It loses on generality: deep inside a call chain, a coroutine
cannot suspend, a fiber can. Trading systems use both: fibers to run
blocking-style strategy code on one thread without kernel threads, coroutines
for new asynchronous code where the small frames and the explicit suspension
points are an advantage.

## Why the kernel costs 100× more

A condition-variable hand-off on this machine is 1,332 ns. That buys: a
system call to sleep (entry, argument checks, the futex or psynch queue), the
kernel scheduler choosing the next thread, a full register save including
every vector register, a possible page-table switch, an inter-processor
interrupt to wake the other core, and the cache and TLB misses that follow
when the woken thread runs on a core that has forgotten it. The fiber switch
does none of that, because it does not need to: both sides agreed to the
switch, so nothing has to be protected from them.

The spin hand-off (35 ns) is the control: it also involves no kernel, and
what remains is the cost of one cache line travelling from one core's cache
to the other's. That is a floor for any cross-core communication and the
reason the single-threaded fiber switch can be cheaper than talking to a
second core at all.

## Testing

* `test_context`: the assembly and the first frame in isolation.
  `CalleeSavedRegistersSurviveSwitches` keeps more live values than the
  caller-saved registers can hold, on both sides of the switch, so the
  compiler must place some in callee-saved registers; a missing save or
  restore changes the result.
* `test_fiber`: scheduler semantics, scale, recursion, errors, recycling.
* `test_coro`: the comparison object behaves.
* ASan + UBSan over all of it, on x86-64 and arm64, in CI. In sanitized
  builds GoogleTest is compiled from source with the same flags; linking a
  prebuilt, uninstrumented GoogleTest produces false container-overflow
  reports from libc++'s annotations.

## Not here, on purpose

Preemption (a timer signal that forces a switch), a multi-threaded scheduler
with work stealing, I/O integration (parking a fiber on `epoll`/`kqueue`),
ThreadSanitizer annotations, the Windows x64 ABI (which has more callee-saved
registers, including `xmm6`–`xmm15`, and a thread information block to swap).
Each is a real feature and a real exercise; none changes the switch.
