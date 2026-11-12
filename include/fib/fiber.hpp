// fiber.hpp — fibers and a round-robin scheduler, on top of the context layer.
//
// A fiber is a function with its own stack. Fibers on one thread take turns:
// a running fiber keeps the CPU until it calls yield() or returns. Nothing is
// preemptive and nothing is shared between threads, so there are no locks in
// here and no data races to have. That is what makes a switch cost tens of
// nanoseconds instead of microseconds: no kernel, no scheduler, no interrupt.
//
//   Scheduler s;
//   s.spawn([&] { for (int i = 0; i < 3; ++i) { work(); Scheduler::yield(); } });
//   s.spawn([&] { ... });
//   s.run();            // returns when every fiber has finished
#pragma once

#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <vector>

#include "fib/context.hpp"

namespace fib {

class Scheduler;

class Fiber {
public:
    enum class State : unsigned char { Ready, Running, Done };

    Fiber(Scheduler& sched, std::function<void()> fn, Stack&& stack);
    ~Fiber() = default;
    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;

    State      state() const noexcept { return state_; }
    Scheduler& scheduler() const noexcept { return sched_; }

private:
    friend class Scheduler;
    static void entry(void* self) noexcept;  // runs on the fiber's stack

    Scheduler&            sched_;
    std::function<void()> fn_;
    Stack                 stack_;
    Context               ctx_;
    State                 state_ = State::Ready;
    std::exception_ptr    error_;
};

class Scheduler {
public:
    static constexpr std::size_t kDefaultStack = 64 * 1024;

    Scheduler() = default;
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // Create a fiber; it starts the next time the scheduler reaches it. The
    // stack comes from the pool of finished fibers' stacks when one is large
    // enough, so after warm-up spawning does not touch the kernel.
    Fiber& spawn(std::function<void()> fn, std::size_t stack_bytes = kDefaultStack);

    // Run fibers round-robin until none is left. If a fiber let an exception
    // escape, run() rethrows it after every other fiber has finished.
    void run();

    // Run one fiber until it yields or finishes. Returns false when there is
    // nothing left to run. Useful for stepping in tests and benchmarks.
    bool step();

    // From inside a fiber: give the CPU to the next ready fiber.
    static void yield();

    // The fiber currently running on this thread, or nullptr.
    static Fiber* current() noexcept;

    std::size_t ready_count() const noexcept { return ready_.size(); }
    std::size_t spawned() const noexcept { return spawned_; }
    std::size_t switches() const noexcept { return switches_; }
    std::size_t pooled_stacks() const noexcept { return stack_pool_.size(); }

private:
    friend class Fiber;

    void resume(Fiber& f);

    Context                             main_;      // the thread that called run()
    std::deque<Fiber*>                  ready_;
    std::vector<std::unique_ptr<Fiber>> all_;
    std::vector<Stack>                  stack_pool_;  // stacks of finished fibers, for reuse
    Fiber*                              current_ = nullptr;
    std::size_t                         spawned_ = 0;
    std::size_t                         switches_ = 0;
    std::exception_ptr                  first_error_;
};

}  // namespace fib
