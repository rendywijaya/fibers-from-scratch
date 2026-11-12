// context.cpp — stacks, the first frame, and the sanitizer handshake.
#include "fib/context.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>

#include "asan.hpp"

namespace fib {

Stack::Stack(std::size_t size_bytes) {
    const auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    // Round the usable part up to whole pages and add one guard page below it.
    usable_ = (size_bytes + page - 1) / page * page;
    size_ = usable_ + page;
    void* p = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) throw std::bad_alloc();
    if (::mprotect(p, page, PROT_NONE) != 0) {  // the guard page: any touch faults
        ::munmap(p, size_);
        throw std::runtime_error("mprotect failed");
    }
    base_ = p;
}

Stack::~Stack() {
    if (base_) ::munmap(base_, size_);
}

Stack& Stack::operator=(Stack&& o) noexcept {
    if (this != &o) {
        if (base_) ::munmap(base_, size_);
        base_ = o.base_; size_ = o.size_; usable_ = o.usable_;
        o.base_ = nullptr;
    }
    return *this;
}

namespace {
#ifdef FIB_ASAN
// The context that most recently called switch_to on this thread. A fresh
// context's first instruction is fib_context_entry, which must finish the
// sanitizer handshake and, as a side effect, learns the bounds of the stack
// it came from; this is how the main thread's stack bounds are discovered.
thread_local Context* t_switching_from = nullptr;
#endif
}  // namespace

// Every fresh context starts here, called by fib_trampoline with the Context
// pointer that make() put in a callee-saved register. It completes the
// sanitizer handshake and then runs the user's entry, which never returns.
extern "C" void fib_context_entry(void* p) noexcept {
    auto* c = static_cast<Context*>(p);
#ifdef FIB_ASAN
    const void* from_bottom = nullptr;
    std::size_t from_size = 0;
    __sanitizer_finish_switch_fiber(nullptr, &from_bottom, &from_size);
    if (Context* from = t_switching_from; from && !from->stack_bottom) {
        from->stack_bottom = from_bottom;
        from->stack_size = from_size;
    }
#endif
    c->fn(c->arg);
    __builtin_trap();  // an entry returned: there is nothing below this frame
}

// The first frame. It must look exactly like what fib_switch would have
// pushed, so that fib_switch's pops and `ret` take us into fib_trampoline
// with this Context and fib_context_entry in the registers it expects.
void Context::make(Stack& stack, Entry entry, void* entry_arg) noexcept {
    fn = entry;
    arg = entry_arg;
    auto top = reinterpret_cast<std::uintptr_t>(stack.top());
    stack_bottom = stack.bottom();
    stack_size = stack.usable();
#if defined(__aarch64__)
    // 160-byte frame; x19 at +0, x20 at +8, x30 (lr) at +88.
    top &= ~std::uintptr_t{15};
    auto* frame = reinterpret_cast<std::uintptr_t*>(top - 160);
    std::memset(frame, 0, 160);
    frame[0] = reinterpret_cast<std::uintptr_t>(this);              // x19: the Context
    frame[1] = reinterpret_cast<std::uintptr_t>(&fib_context_entry);  // x20: where to go
    frame[11] = reinterpret_cast<std::uintptr_t>(&fib_trampoline);  // x30
    sp = frame;
#elif defined(__x86_64__)
    // Return-address slot 16-byte aligned; r13 at +24 and r12 at +32 from
    // the saved rsp.
    top &= ~std::uintptr_t{15};
    auto* ret_slot = reinterpret_cast<std::uintptr_t*>(top - 16);  // 16-aligned
    auto* frame = ret_slot - 7;                                     // 56 bytes below it
    std::memset(frame, 0, 64);
    // frame[0]: mxcsr + x87 cw. Default control words: mxcsr 0x1F80, fcw 0x037F.
    frame[0] = std::uintptr_t{0x1F80} | (std::uintptr_t{0x037F} << 32);
    frame[3] = reinterpret_cast<std::uintptr_t>(&fib_context_entry);  // r13: where to go
    frame[4] = reinterpret_cast<std::uintptr_t>(this);                // r12: the Context
    *ret_slot = reinterpret_cast<std::uintptr_t>(&fib_trampoline);  // return address
    sp = frame;
#else
#error "unsupported architecture: this project implements AArch64 and x86-64"
#endif
}

void Context::switch_to(Context& to, bool exiting) noexcept {
#ifdef FIB_ASAN
    t_switching_from = this;
    __sanitizer_start_switch_fiber(exiting ? nullptr : &fake_stack, to.stack_bottom, to.stack_size);
#else
    (void)exiting;
#endif
    fib_switch(&sp, to.sp);
#ifdef FIB_ASAN
    // We are back on this context's stack: complete the handshake.
    const void* from_bottom = nullptr;
    std::size_t from_size = 0;
    __sanitizer_finish_switch_fiber(fake_stack, &from_bottom, &from_size);
#endif
}

}  // namespace fib
