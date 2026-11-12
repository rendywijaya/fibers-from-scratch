## Switch costs (median of 5 runs; best run in parentheses)

| what | ns per unit | unit | ops per run | what it is |
|---|---:|---|---:|---|
| raw context switch (fib_switch) | **11.6** (11.3) | switch | 4000000 | save/restore callee-saved regs, swap sp, ret |
| fiber yield (scheduler, 2 fibers) | **34.4** (34.5) | yield | 2000000 | two raw switches + run queue |
| C++20 coroutine resume+suspend | **2.0** (1.9) | resume | 5000000 | function call into the frame, state jump, return |
| thread hand-off, spinning on an atomic | **35.1** (34.9) | hand-off | 1000000 | no kernel: a cache line moving between cores |
| thread hand-off, mutex + condition variable | **1332.1** (1307.8) | hand-off | 100000 | kernel sleep + wake: futex / psynch |
| thread hand-off, one byte through a pipe | **1608.7** (1440.3) | hand-off | 100000 | write + read system calls, kernel switch |
| std::thread create + join | **13457.2** (13388.6) | thread | 2000 | clone/pthread_create, stack, kernel scheduling |
| fiber spawn + run, fresh stack | **3663.9** (3416.7) | fiber | 20000 | mmap + mprotect guard page: two system calls |
| fiber spawn + run, recycled stack | **93.5** (92.5) | fiber | 200000 | no kernel: frame set-up, queue, two switches |

kernel condvar hand-off / raw switch = 115x; pipe / raw = 139x; thread create / fiber spawn (fresh) = 4x, (recycled) = 144x
