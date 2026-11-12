// context.hpp — a stack with a guard page, and a saved stack pointer.
//
// This is the layer directly above the assembly. It knows how to allocate a
// stack, how to lay out the first frame so that the first fib_switch into it
// lands in fib_trampoline, and how to tell AddressSanitizer about the stack
// switch so that sanitized builds work.
#pragma once

#include <cstddef>

extern "C" {
// src/switch_<arch>.S
void fib_switch(void** save_sp, void* load_sp) noexcept;
void fib_trampoline() noexcept;
}

namespace fib {

// A region of memory for a fiber's stack. mmap'd, with the lowest page made
// inaccessible: running off the end of the stack then faults immediately
// instead of silently corrupting whatever happens to be below it.
class Stack {
public:
    Stack() = default;  // invalid until assigned
    explicit Stack(std::size_t size_bytes);
    ~Stack();
    Stack(const Stack&) = delete;
    Stack& operator=(const Stack&) = delete;
    Stack(Stack&& o) noexcept : base_(o.base_), size_(o.size_), usable_(o.usable_) { o.base_ = nullptr; }
    Stack& operator=(Stack&& o) noexcept;
    bool valid() const noexcept { return base_ != nullptr; }

    // Highest address, where the stack starts (stacks grow down). 16-byte aligned.
    void*       top() const noexcept { return static_cast<char*>(base_) + size_; }
    // Lowest usable address (just above the guard page) and the usable size:
    // what ASan needs to know.
    const void* bottom() const noexcept { return static_cast<char*>(base_) + (size_ - usable_); }
    std::size_t usable() const noexcept { return usable_; }
    std::size_t page_size() const noexcept { return size_ - usable_; }

private:
    void*       base_ = nullptr;
    std::size_t size_ = 0;    // including the guard page
    std::size_t usable_ = 0;  // excluding it
};

// A saved execution state. For a fiber it is the stack pointer into its own
// stack; for the thread that runs the scheduler it is the stack pointer into
// the ordinary thread stack at the moment it last switched away.
struct Context {
    using Entry = void (*)(void*);

    void*       sp = nullptr;
    Entry       fn = nullptr;   // what a fresh context runs, and with what
    void*       arg = nullptr;
    // For ASan: where this context's stack lives. The main thread's bounds
    // are learned from the sanitizer on the first switch away from it.
    const void* stack_bottom = nullptr;
    std::size_t stack_size = 0;
    void*       fake_stack = nullptr;  // ASan's bookkeeping handle

    // Prepare `stack` so that switching to this context for the first time
    // runs fn(arg) on it. fn must never return: it ends by switching to
    // another context with `exiting = true`.
    void make(Stack& stack, Entry fn, void* arg) noexcept;

    // Save the current state into *this and resume `to`. Returns when some
    // other context switches back to *this. `exiting` is set when the current
    // stack is about to be destroyed, which ASan wants to know.
    void switch_to(Context& to, bool exiting = false) noexcept;
};

}  // namespace fib
