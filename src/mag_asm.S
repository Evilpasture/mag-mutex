#include "mag_asm_defs.h"

// ----------------------------------------------------------------------------
// mag_switch(void **old_sp, void *new_sp)
// ----------------------------------------------------------------------------
ASM_FUNC_START(mag_switch)
#if defined(OS_WIN) && defined(ARCH_X64)
    // 1. Current RSP ends in 8
    push %rax; // DUMMY PUSH to align stack to 16 (8 + 8 = 16)
    push %rbp; push %rbx; push %rdi; push %rsi
    push %r12; push %r13; push %r14; push %r15
    // 2. Now RSP ends in 0 (Call=8 + Dummy=8 + 8 GPRs=64 => 80 bytes)
    
    sub $160, %rsp // 160 is a multiple of 16. RSP stays ending in 0.
    
    movaps %xmm6, 0(%rsp);    movaps %xmm7, 16(%rsp);   movaps %xmm8, 32(%rsp)
    movaps %xmm9, 48(%rsp);   movaps %xmm10, 64(%rsp);  movaps %xmm11, 80(%rsp)
    movaps %xmm12, 96(%rsp);  movaps %xmm13, 112(%rsp); movaps %xmm14, 128(%rsp)
    movaps %xmm15, 144(%rsp)

    mov %rsp, (%rcx)
    mov %rdx, %rsp

    movaps 0(%rsp), %xmm6;    movaps 16(%rsp), %xmm7;   movaps 32(%rsp), %xmm8
    movaps 48(%rsp), %xmm9;   movaps 64(%rsp), %xmm10;  movaps 80(%rsp), %xmm11
    movaps 96(%rsp), %xmm12;  movaps 112(%rsp), %xmm13; movaps 128(%rsp), %xmm14
    movaps 144(%rsp), %xmm15
    
    add $160, %rsp
    pop %r15; pop %r14; pop %r13; pop %r12
    pop %rsi; pop %rdi; pop %rbx; pop %rbp
    pop %rax; // Pop the dummy
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
    sub $32, %rsp             // Standard 32-byte shadow space
    call CSYM(mag_trampoline)
    add $32, %rsp
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