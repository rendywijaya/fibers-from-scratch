# Benchmarks

## Method

Every row is a **batch** measurement: N operations between two
`steady_clock` reads, divided by N. Nothing is timed per operation, because a
single switch costs about a quarter of one tick of Apple Silicon's 41.67 ns
user-space counter; batching makes the clock's resolution and overhead
irrelevant. Each row is repeated 5 times and the **median** is reported, with
the best run beside it, so a reader can see how stable it was.

Nothing is pinned to a core: macOS offers no thread-to-core pinning API. The
M2 Pro has performance and efficiency cores, so the cross-thread rows may
land on either kind; the fiber and coroutine rows run on whatever core the
process started on. Linux numbers from CI are in `results/` when available.

## What each row measures

| row | exactly |
|---|---|
| raw context switch | `fib_switch` ping-pong between the main context and one fiber context, 2 M round trips, reported per switch (a round trip is two) |
| fiber yield | two fibers each calling `Scheduler::yield()` 1 M times; per yield, which is two raw switches plus the scheduler's queue and state updates |
| coroutine resume + suspend | `Generator<int>::next()` 5 M times on a `noinline` coroutine with a compiler barrier on every value (without both, the optimizer deletes the loop and reports 0.0 ns) |
| thread hand-off, spinning | two kernel threads passing a turn through one `std::atomic<int>` with acquire/release, both spinning, 500 k round trips; per hand-off |
| thread hand-off, condvar | the same hand-off through a `std::mutex` and a `std::condition_variable`; per hand-off |
| thread hand-off, pipe | the same hand-off as one byte written to a pipe and read from another; per hand-off |
| `std::thread` create + join | 2,000 threads that do nothing, created and joined one after another |
| fiber spawn + run, fresh | 20,000 fibers spawned then run: every one needs a new `mmap`'d stack |
| fiber spawn + run, recycled | 200,000 fibers spawned and run one at a time, so the stack comes back from the pool each time |

## Results, Apple M2 Pro, macOS, Apple clang 21, `-O3`

| what | median ns | best ns | per |
|---|---:|---:|---|
| raw context switch | 11.6 | 11.3 | switch |
| fiber yield (scheduler, 2 fibers) | 34.4 | 34.5 | yield |
| C++20 coroutine resume + suspend | 2.0 | 1.9 | resume |
| thread hand-off, spinning on an atomic | 35.1 | 34.9 | hand-off |
| thread hand-off, mutex + condition variable | 1,332 | 1,308 | hand-off |
| thread hand-off, one byte through a pipe | 1,609 | 1,440 | hand-off |
| `std::thread` create + join | 13,457 | 13,389 | thread |
| fiber spawn + run, fresh stack | 3,664 | 3,417 | fiber |
| fiber spawn + run, recycled stack | 93.5 | 92.5 | fiber |

Ratios: condvar hand-off / raw switch **115×**; pipe / raw **139×**; thread
create / fiber spawn **4×** fresh, **144×** recycled. Raw output in
`results/m2pro.md` and `.json`.

## Reading the numbers

* **11.6 ns for the switch** is about 35 cycles at this clock: ten stores,
  ten loads, two moves and a `ret` whose target the branch predictor has to
  guess (it alternates between two call sites, which it learns). The floor
  for this design is the memory traffic of the 160-byte frame.
* **34 ns for a yield** is two switches (23 ns) plus about 11 ns of
  scheduler: a deque pop and push, a few stores, the `thread_local` reads.
  A symmetric scheduler would cut the switches to one.
* **2.0 ns for the coroutine** is roughly a call through a function pointer,
  a switch on the state, a store and a return. There is nothing to save.
* **35 ns for the spinning hand-off** is not a context switch at all; it is
  how long a cache line takes to move between two cores and back, and it is
  the floor for anything two cores do together.
* **1,332 ns for the condvar** is the operating system: the system calls, the
  scheduler, a full register save, the wake-up interrupt, and the cold
  caches afterwards. This is the number fibers exist to avoid.
* **3,664 ns to spawn with a fresh stack** is two system calls, `mmap` and
  `mprotect`, and the first-touch page fault on the new stack. **93 ns from
  the pool** is what spawning costs once the kernel is out of the loop.

## What the numbers do not say

* They are from a laptop with no core isolation. The medians are stable to a
  few percent across runs; the tails (not reported here) are not.
* The hand-off rows measure a *ping-pong*, the worst case for threads: every
  hand-off forces a sleep and a wake. Real threads batch work between
  hand-offs, and real fibers do too.
* The fiber numbers are for one scheduler on one thread, which is the point
  of fibers, but a system that also needs several cores pays the cross-core
  cost somewhere.
* The x86-64 numbers from CI are from a shared virtual machine. Their
  ratios are meaningful; their absolute values move with the runner's load.

## Reproduce

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/switch_bench --runs 5 --json results/mine.json
```
