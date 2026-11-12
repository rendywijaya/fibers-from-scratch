// switch_aarch64.S — the context switch, AArch64 (Apple Silicon and Linux).
//
//   void fib_switch(void** save_sp, void* load_sp);
//
// A context is nothing but a stack pointer. Everything the ABI says a callee
// must preserve is pushed onto the *current* stack, the stack pointer is
// stored through save_sp, the other context's stack pointer is loaded, its
// registers are popped, and `ret` jumps to wherever that context last called
// fib_switch from (or, for a fresh fiber, to fib_trampoline).
//
// AAPCS64 callee-saved: x19–x28, x29 (fp), x30 (lr), sp, and the low 64 bits
// of v8–v15 (d8–d15). Everything else may be clobbered by a call, so the
// caller already assumes it is lost. 160 bytes, 16-byte aligned, because the
// hardware checks sp alignment on every access.
//
// Frame layout, from the saved sp upward:
//    0: x19 x20     16: x21 x22     32: x23 x24     48: x25 x26
//   64: x27 x28     80: x29 x30     96: d8 d9      112: d10 d11
//  128: d12 d13    144: d14 d15
//
// On macOS the C symbol `foo` is `_foo` in assembly.
#if defined(__APPLE__)
#define SYM(name) _##name
#else
#define SYM(name) name
#endif

    .text
    .balign 4
    .globl SYM(fib_switch)
#if !defined(__APPLE__)
    .type SYM(fib_switch), %function
#endif
SYM(fib_switch):
    sub  sp, sp, #160
    stp  x19, x20, [sp, #0]
    stp  x21, x22, [sp, #16]
    stp  x23, x24, [sp, #32]
    stp  x25, x26, [sp, #48]
    stp  x27, x28, [sp, #64]
    stp  x29, x30, [sp, #80]
    stp  d8,  d9,  [sp, #96]
    stp  d10, d11, [sp, #112]
    stp  d12, d13, [sp, #128]
    stp  d14, d15, [sp, #144]
    mov  x2, sp
    str  x2, [x0]            // *save_sp = sp
    mov  sp, x1              // sp = load_sp
    ldp  x19, x20, [sp, #0]
    ldp  x21, x22, [sp, #16]
    ldp  x23, x24, [sp, #32]
    ldp  x25, x26, [sp, #48]
    ldp  x27, x28, [sp, #64]
    ldp  x29, x30, [sp, #80]
    ldp  d8,  d9,  [sp, #96]
    ldp  d10, d11, [sp, #112]
    ldp  d12, d13, [sp, #128]
    ldp  d14, d15, [sp, #144]
    add  sp, sp, #160
    ret
#if !defined(__APPLE__)
    .size SYM(fib_switch), .-SYM(fib_switch)
#endif

// fib_trampoline — where a fresh fiber's first `ret` lands.
//
// Context::make() built a frame whose x30 slot holds this address, whose
// x19 slot holds the Context pointer and whose x20 slot holds the entry
// function (fib_context_entry). The pointer is moved into x0 and the entry
// is called. It never returns: when a fiber's function finishes it switches
// away for the last time. The brk makes a bug loud if it ever does return.
//
// .cfi_undefined x30 tells unwinders and debuggers that there is no caller
// above this frame, so a backtrace from inside a fiber stops here cleanly.
    .balign 4
    .globl SYM(fib_trampoline)
#if !defined(__APPLE__)
    .type SYM(fib_trampoline), %function
#endif
SYM(fib_trampoline):
    .cfi_startproc
    .cfi_undefined x30
    mov  x0, x19
    blr  x20
    brk  #0
    .cfi_endproc
#if !defined(__APPLE__)
    .size SYM(fib_trampoline), .-SYM(fib_trampoline)
    .section .note.GNU-stack,"",%progbits
#endif
