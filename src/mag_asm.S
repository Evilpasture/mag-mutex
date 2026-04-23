#include "mag_asm_defs.h"

// ----------------------------------------------------------------------------
// mag_switch(void **old_sp, void *new_sp)
// ----------------------------------------------------------------------------
ASM_FUNC_START(mag_switch)
#if defined(OS_WIN) && defined(ARCH_X64)
    // Windows x64: RCX = old_sp, RDX = new_sp
    push %rbp; push %rbx; push %rdi; push %rsi
    push %r12; push %r13; push %r14; push %r15
    sub $160, %rsp
    movups %xmm6, 0(%rsp);   movups %xmm7, 16(%rsp);  movups %xmm8, 32(%rsp)
    movups %xmm9, 48(%rsp);  movups %xmm10, 64(%rsp); movups %xmm11, 80(%rsp)
    movups %xmm12, 96(%rsp); movups %xmm13, 112(%rsp);movups %xmm14, 128(%rsp)
    movups %xmm15, 144(%rsp)

    mov %rsp, (%rcx)
    mov %rdx, %rsp

    movups 0(%rsp), %xmm6;   movups 16(%rsp), %xmm7;  movups 32(%rsp), %xmm8
    movups 48(%rsp), %xmm9;  movups 64(%rsp), %xmm10; movups 80(%rsp), %xmm11
    movups 96(%rsp), %xmm12; movups 112(%rsp), %xmm13;movups 128(%rsp), %xmm14
    movups 144(%rsp), %xmm15
    add $160, %rsp
    pop %r15; pop %r14; pop %r13; pop %r12
    pop %rsi; pop %rdi; pop %rbx; pop %rbp
    ret

#elif defined(ARCH_X64)
    // System V x64 (Linux/macOS): RDI = old_sp, RSI = new_sp
    push %rbp; push %rbx; push %r12; push %r13; push %r14; push %r15
    mov %rsp, (%rdi)
    mov %rsi, %rsp
    pop %r15; pop %r14; pop %r13; pop %r12; pop %rbx; pop %rbp
    ret

#elif defined(ARCH_ARM64)
    // ARM64: X0 = old_sp, X1 = new_sp
    sub sp, sp, #160
    stp x19, x20, [sp, #0x00]
    stp x21, x22, [sp, #0x10]
    stp x23, x24, [sp, #0x20]
    stp x25, x26, [sp, #0x30]
    stp x27, x28, [sp, #0x40]
    stp x29, x30, [sp, #0x50]
    stp d8,  d9,  [sp, #0x60]
    stp d10, d11, [sp, #0x70]
    stp d12, d13, [sp, #0x80]
    stp d14, d15, [sp, #0x90]

    mov x10, sp
    str x10, [x0]
    mov sp, x1

    ldp x19, x20, [sp, #0x00]
    ldp x21, x22, [sp, #0x10]
    ldp x23, x24, [sp, #0x20]
    ldp x25, x26, [sp, #0x30]
    ldp x27, x28, [sp, #0x40]
    ldp x29, x30, [sp, #0x50]
    ldp d8,  d9,  [sp, #0x60]
    ldp d10, d11, [sp, #0x70]
    ldp d12, d13, [sp, #0x80]
    ldp d14, d15, [sp, #0x90]
    add sp, sp, #160
    ret
#endif
ASM_FUNC_END(mag_switch)

// ----------------------------------------------------------------------------
// mag_trampoline_asm
// ----------------------------------------------------------------------------
ASM_FUNC_START(mag_trampoline_asm)
#if defined(OS_WIN) && defined(ARCH_X64)
    sub $40, %rsp
    call CSYM(mag_trampoline)
    add $40, %rsp
    ret
#elif defined(ARCH_X64)
    sub $8, %rsp
    call CSYM(mag_trampoline)
    add $8, %rsp
    ret
#elif defined(ARCH_ARM64)
    b CSYM(mag_trampoline)
#endif
ASM_FUNC_END(mag_trampoline_asm)