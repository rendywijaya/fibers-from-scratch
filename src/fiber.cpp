// fiber.cpp — the scheduler, and the entry point every fiber starts in.
#include "fib/fiber.hpp"

#include <utility>

namespace {
thread_local fib::Scheduler* t_scheduler = nullptr;
thread_local fib::Fiber*     t_current = nullptr;
}  // namespace

namespace fib {

Fiber::Fiber(Scheduler& sched, std::function<void()> fn, Stack&& stack)
    : sched_(sched), fn_(std::move(fn)), stack_(std::move(stack)) {
    ctx_.make(stack_, &Fiber::entry, this);
}

Fiber& Scheduler::spawn(std::function<void()> fn, std::size_t stack_bytes) {
    // Reuse the most recently returned stack that is big enough: it is the
    // one most likely to still be in cache and mapped in the TLB.
    Stack stack;
    for (auto it = stack_pool_.rbegin(); it != stack_pool_.rend(); ++it) {
        if (it->usable() >= stack_bytes) {
            stack = std::move(*it);
            stack_pool_.erase(std::next(it).base());
            break;
        }
    }
    if (!stack.valid()) stack = Stack(stack_bytes);
    all_.push_back(std::make_unique<Fiber>(*this, std::move(fn), std::move(stack)));
    Fiber& f = *all_.back();
    ready_.push_back(&f);
    ++spawned_;
    return f;
}

void Scheduler::resume(Fiber& f) {
    Scheduler* prev_sched = t_scheduler;
    Fiber*     prev_fiber = t_current;
    t_scheduler = this;
    t_current = &f;
    current_ = &f;
    f.state_ = Fiber::State::Running;
    ++switches_;
    main_.switch_to(f.ctx_);  // returns when f yields or finishes
    current_ = nullptr;
    t_current = prev_fiber;
    t_scheduler = prev_sched;
    if (f.state_ == Fiber::State::Done) {
        if (f.error_ && !first_error_) first_error_ = f.error_;
        // The fiber has switched away for the last time, so its stack is free
        // to reuse. The Fiber object itself stays in all_ so that Fiber&
        // handles held by the caller remain valid.
        stack_pool_.push_back(std::move(f.stack_));
    } else {
        f.state_ = Fiber::State::Ready;
        ready_.push_back(&f);
    }
}

bool Scheduler::step() {
    if (ready_.empty()) return false;
    Fiber* f = ready_.front();
    ready_.pop_front();
    resume(*f);
    return true;
}

void Scheduler::run() {
    while (step()) {}
    if (first_error_) std::rethrow_exception(std::exchange(first_error_, nullptr));
}

void Scheduler::yield() {
    Fiber* f = t_current;
    if (!f) return;  // not inside a fiber: nothing to yield to
    ++f->sched_.switches_;
    f->ctx_.switch_to(f->sched_.main_);  // back to resume(); returns when rescheduled
}

Fiber* Scheduler::current() noexcept { return t_current; }

// The body of every fiber, run on the fiber's own stack. It must never
// return: when the function finishes, the fiber switches to the scheduler for
// the last time, with `exiting` set so the stack can be recycled (and so ASan,
// in a sanitized build, stops tracking it).
void Fiber::entry(void* p) noexcept {
    auto* f = static_cast<Fiber*>(p);
    try {
        f->fn_();
    } catch (...) {
        f->error_ = std::current_exception();
    }
    f->state_ = Fiber::State::Done;
    f->ctx_.switch_to(f->sched_.main_, /*exiting=*/true);
    __builtin_unreachable();  // the scheduler never resumes a Done fiber
}

}  // namespace fib
