// switch_bench.cpp — what a switch costs, five ways, and what the kernel charges.
//
//   usage: switch_bench [--runs 5] [--json out.json]
//
// Every number is a batch measurement: N operations inside one pair of clock
// reads, so the timer's resolution and overhead do not matter (on Apple
// Silicon the counter ticks every 41.67 ns, which would swamp a 10 ns switch
// if it were read per operation). Each measurement is repeated `runs` times
// and the median is reported.
//
// What each row measures, precisely:
//   raw switch        fib_switch ping-pong between two contexts. One switch
//                     = save callee-saved registers, swap sp, restore, ret.
//   fiber yield       Scheduler::yield() with two fibers: fiber -> scheduler
//                     -> other fiber, so two raw switches plus the queue.
//   coroutine resume  Generator::next(): resume a suspended C++20 coroutine
//                     until its next co_yield, and come back.
//   thread spin       two kernel threads handing a turn back and forth on one
//                     atomic, both spinning. No kernel involvement: this is
//                     the cost of moving a cache line between two cores.
//   thread condvar    the same hand-off through a mutex and a condition
//                     variable: the kernel puts one thread to sleep and wakes
//                     the other. The usual way threads wait.
//   thread pipe       the same hand-off through two pipes, one byte each way:
//                     two system calls and a context switch per hand-off.
//   create+join       std::thread (kernel) vs Fiber spawn + run (user), with a
//                     fresh mmap'd stack and with a stack recycled from the pool.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "fib/context.hpp"
#include "fib/coro.hpp"
#include "fib/fiber.hpp"

using namespace fib;
using clk = std::chrono::steady_clock;

namespace {

struct Row {
    const char* name;
    const char* unit;
    double      ns;        // per unit, median
    double      ns_min;    // best run
    std::size_t n;         // operations per run
    const char* note;
};

template <class F>
double median_ns_per(int runs, std::size_t n, F&& f) {
    std::vector<double> v;
    for (int r = 0; r < runs; ++r) {
        auto t0 = clk::now();
        f();
        auto t1 = clk::now();
        v.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / double(n));
    }
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
template <class F>
double min_ns_per(int runs, std::size_t n, F&& f) {
    double best = 1e300;
    for (int r = 0; r < runs; ++r) {
        auto t0 = clk::now();
        f();
        auto t1 = clk::now();
        best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / double(n));
    }
    return best;
}

// ---- raw switch: two contexts, no scheduler -------------------------------
Context g_main, g_co;
void ping(void*) {
    for (;;) g_co.switch_to(g_main);  // bounce forever; the stack is dropped with the Stack
}
Row bench_raw(int runs) {
    const std::size_t n = 2'000'000;  // round trips; two switches each
    Stack st(64 * 1024);
    g_co.make(st, ping, nullptr);
    auto body = [&] { for (std::size_t i = 0; i < n; ++i) g_main.switch_to(g_co); };
    double med = median_ns_per(runs, n * 2, body), best = min_ns_per(runs, n * 2, body);
    return {"raw context switch (fib_switch)", "switch", med, best, n * 2, "save/restore callee-saved regs, swap sp, ret"};
}

// ---- fiber yield through the scheduler ------------------------------------
Row bench_yield(int runs) {
    const std::size_t n = 1'000'000;  // yields per fiber
    auto body = [&] {
        Scheduler s;
        for (int k = 0; k < 2; ++k) s.spawn([&] { for (std::size_t i = 0; i < n; ++i) Scheduler::yield(); });
        s.run();
    };
    double med = median_ns_per(runs, n * 2, body), best = min_ns_per(runs, n * 2, body);
    return {"fiber yield (scheduler, 2 fibers)", "yield", med, best, n * 2, "two raw switches + run queue"};
}

// ---- C++20 coroutine ------------------------------------------------------
// noinline, so the coroutine body is a real function the driver resumes
// through the frame, and a barrier on every value, so the loop cannot be
// folded into a closed form. Without both, the optimizer deletes the whole
// thing and reports 0.0 ns.
__attribute__((noinline)) Generator<int> counter(std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) co_yield int(i);
}
Row bench_coro(int runs) {
    static volatile std::size_t vn = 5'000'000;
    const std::size_t n = vn;
    auto body = [&] {
        auto g = counter(n);
        while (g.next()) { int v = g.value(); asm volatile("" : : "r"(v) : "memory"); }
    };
    double med = median_ns_per(runs, n, body), best = min_ns_per(runs, n, body);
    return {"C++20 coroutine resume+suspend", "resume", med, best, n, "function call into the frame, state jump, return"};
}

// ---- kernel threads: spin -------------------------------------------------
Row bench_spin(int runs) {
    const std::size_t n = 500'000;  // round trips
    auto body = [&] {
        alignas(64) std::atomic<int> turn{0};
        std::thread t([&] {
            for (std::size_t i = 0; i < n; ++i) {
                while (turn.load(std::memory_order_acquire) != 1) {}
                turn.store(0, std::memory_order_release);
            }
        });
        for (std::size_t i = 0; i < n; ++i) {
            turn.store(1, std::memory_order_release);
            while (turn.load(std::memory_order_acquire) != 0) {}
        }
        t.join();
    };
    double med = median_ns_per(runs, n * 2, body), best = min_ns_per(runs, n * 2, body);
    return {"thread hand-off, spinning on an atomic", "hand-off", med, best, n * 2, "no kernel: a cache line moving between cores"};
}

// ---- kernel threads: condition variable -----------------------------------
Row bench_condvar(int runs) {
    const std::size_t n = 50'000;
    auto body = [&] {
        std::mutex m;
        std::condition_variable cv;
        int turn = 0;
        std::thread t([&] {
            for (std::size_t i = 0; i < n; ++i) {
                std::unique_lock lk(m);
                cv.wait(lk, [&] { return turn == 1; });
                turn = 0;
                lk.unlock();
                cv.notify_one();
            }
        });
        for (std::size_t i = 0; i < n; ++i) {
            { std::lock_guard lk(m); turn = 1; }
            cv.notify_one();
            std::unique_lock lk(m);
            cv.wait(lk, [&] { return turn == 0; });
        }
        t.join();
    };
    double med = median_ns_per(runs, n * 2, body), best = min_ns_per(runs, n * 2, body);
    return {"thread hand-off, mutex + condition variable", "hand-off", med, best, n * 2, "kernel sleep + wake: futex / psynch"};
}

// ---- kernel threads: pipe -------------------------------------------------
Row bench_pipe(int runs) {
    const std::size_t n = 50'000;
    auto body = [&] {
        int a[2], b[2];
        if (::pipe(a) || ::pipe(b)) { std::perror("pipe"); std::exit(1); }
        std::thread t([&] {
            char c;
            for (std::size_t i = 0; i < n; ++i) {
                if (::read(a[0], &c, 1) != 1) break;
                if (::write(b[1], &c, 1) != 1) break;
            }
        });
        char c = 'x';
        for (std::size_t i = 0; i < n; ++i) {
            if (::write(a[1], &c, 1) != 1) break;
            if (::read(b[0], &c, 1) != 1) break;
        }
        t.join();
        ::close(a[0]); ::close(a[1]); ::close(b[0]); ::close(b[1]);
    };
    double med = median_ns_per(runs, n * 2, body), best = min_ns_per(runs, n * 2, body);
    return {"thread hand-off, one byte through a pipe", "hand-off", med, best, n * 2, "write + read system calls, kernel switch"};
}

// ---- creation -------------------------------------------------------------
Row bench_thread_create(int runs) {
    const std::size_t n = 2'000;
    auto body = [&] { for (std::size_t i = 0; i < n; ++i) { std::thread t([] {}); t.join(); } };
    double med = median_ns_per(runs, n, body), best = min_ns_per(runs, n, body);
    return {"std::thread create + join", "thread", med, best, n, "clone/pthread_create, stack, kernel scheduling"};
}
Row bench_fiber_create_fresh(int runs) {
    const std::size_t n = 20'000;
    // All spawned before any runs, so every fiber needs a new stack.
    auto body = [&] { Scheduler s; for (std::size_t i = 0; i < n; ++i) s.spawn([] {}, 16 * 1024); s.run(); };
    double med = median_ns_per(runs, n, body), best = min_ns_per(runs, n, body);
    return {"fiber spawn + run, fresh stack", "fiber", med, best, n, "mmap + mprotect guard page: two system calls"};
}
Row bench_fiber_create_pooled(int runs) {
    const std::size_t n = 200'000;
    // Spawn and run one at a time: after the first, the stack comes from the pool.
    auto body = [&] { Scheduler s; for (std::size_t i = 0; i < n; ++i) { s.spawn([] {}, 16 * 1024); s.run(); } };
    double med = median_ns_per(runs, n, body), best = min_ns_per(runs, n, body);
    return {"fiber spawn + run, recycled stack", "fiber", med, best, n, "no kernel: frame set-up, queue, two switches"};
}

}  // namespace

int main(int argc, char** argv) {
    int runs = 5;
    std::string json;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        if (a == "--runs") runs = std::atoi(argv[i + 1]);
        else if (a == "--json") json = argv[i + 1];
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    std::vector<Row> rows;
    rows.push_back(bench_raw(runs));
    rows.push_back(bench_yield(runs));
    rows.push_back(bench_coro(runs));
    rows.push_back(bench_spin(runs));
    rows.push_back(bench_condvar(runs));
    rows.push_back(bench_pipe(runs));
    rows.push_back(bench_thread_create(runs));
    rows.push_back(bench_fiber_create_fresh(runs));
    rows.push_back(bench_fiber_create_pooled(runs));

    std::printf("## Switch costs (median of %d runs; best run in parentheses)\n\n", runs);
    std::printf("| what | ns per unit | unit | ops per run | what it is |\n|---|---:|---|---:|---|\n");
    for (const auto& r : rows)
        std::printf("| %s | **%.1f** (%.1f) | %s | %zu | %s |\n", r.name, r.ns, r.ns_min, r.unit, r.n, r.note);
    std::printf("\nkernel condvar hand-off / raw switch = %.0fx; pipe / raw = %.0fx; "
                "thread create / fiber spawn (fresh) = %.0fx, (recycled) = %.0fx\n",
                rows[4].ns / rows[0].ns, rows[5].ns / rows[0].ns, rows[6].ns / rows[7].ns, rows[6].ns / rows[8].ns);

    if (!json.empty()) {
        std::FILE* f = std::fopen(json.c_str(), "w");
        if (!f) { std::perror(json.c_str()); return 1; }
        std::fprintf(f, "{\"runs\":%d,\"rows\":[", runs);
        for (std::size_t i = 0; i < rows.size(); ++i)
            std::fprintf(f, "%s{\"name\":\"%s\",\"unit\":\"%s\",\"ns\":%.2f,\"ns_min\":%.2f,\"n\":%zu}", i ? "," : "",
                         rows[i].name, rows[i].unit, rows[i].ns, rows[i].ns_min, rows[i].n);
        std::fprintf(f, "]}\n");
        std::fclose(f);
    }
    return 0;
}
