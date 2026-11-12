// switch_x86_64.S — the context switch, x86-64, System V ABI (Linux, macOS).
//
//   void fib_switch(void** save_sp, void* load_sp);     rdi = save_sp, rsi = load_sp
//
// Same idea as the AArch64 version: push everything the ABI says a callee
// must preserve onto the current stack, store rsp, load the other rsp, pop,
// ret. SysV callee-saved: rbx, rbp, r12–r15, rsp, plus the MXCSR control
// bits and the x87 control word (rounding modes, exception masks). The
// vector registers are all caller-saved, so nothing else needs saving.
//
// Frame layout, from the saved rsp upward:
//    0: mxcsr (4 bytes) + x87 cw (2 bytes) + pad
//    8: r15    16: r14    24: r13    32: r12    40: rbx    48: rbp
//   56: return address (pushed by the `call fib_switch`)
//
// Alignment: at any call instruction rsp must be 16-byte aligned, so at a
// function's entry rsp % 16 == 8. fib_make_context() builds a fresh frame
// with its return-address slot 16-byte aligned, which makes fib_trampoline
// start like any normally-called function.
#if defined(__APPLE__)
#define SYM(name) _##name
#else
#define SYM(name) name
#endif

    .text
    .globl SYM(fib_switch)
#if !defined(__APPLE__)
    .type SYM(fib_switch), @function
#endif
SYM(fib_switch):
    pushq   %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    subq    $8, %rsp
    stmxcsr (%rsp)
    fnstcw  4(%rsp)
    movq    %rsp, (%rdi)        // *save_sp = rsp
    movq    %rsi, %rsp          // rsp = load_sp
    ldmxcsr (%rsp)
    fldcw   4(%rsp)
    addq    $8, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret
#if !defined(__APPLE__)
    .size SYM(fib_switch), .-SYM(fib_switch)
#endif

// fib_trampoline — where a fresh fiber's first `ret` lands.
//
// Context::make() put this address in the return slot, the Context pointer
// in the r12 slot and the entry function (fib_context_entry) in the r13
// slot. r12 is moved to rdi and r13 is called. The `subq $8` keeps the stack 16-byte aligned at
// the call, as the ABI requires. The entry never returns; ud2 makes it loud
// if it does.
    .globl SYM(fib_trampoline)
#if !defined(__APPLE__)
    .type SYM(fib_trampoline), @function
#endif
SYM(fib_trampoline):
    .cfi_startproc
    .cfi_undefined rip
    movq    %r12, %rdi
    subq    $8, %rsp
    call    *%r13
    ud2
    .cfi_endproc
#if !defined(__APPLE__)
    .size SYM(fib_trampoline), .-SYM(fib_trampoline)
    .section .note.GNU-stack,"",@progbits
#endif
