#include "mag_thread.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

#if defined(_WIN32)
#    include <windows.h>
#else
#    include <sys/mman.h>
#    include <unistd.h>
#endif

// Forward declaration to satisfy -Wmissing-prototypes and asm calls
void mag_trampoline(void);

// ----------------------------------------------------------------------------
// 1. Cross-Platform Assembly Context Switch
// ----------------------------------------------------------------------------

[[gnu::naked, gnu::noinline, gnu::hot, gnu::visibility("hidden"), gnu::no_stack_protector]]
static void mag_switch(void **old_sp, void *new_sp) {
#if defined(__aarch64__)
    __asm__ volatile("sub sp, sp, #160 \n"
                     "stp x19, x20, [sp, #0x00] \n"
                     "stp x21, x22, [sp, #0x10] \n"
                     "stp x23, x24, [sp, #0x20] \n"
                     "stp x25, x26, [sp, #0x30] \n"
                     "stp x27, x28, [sp, #0x40] \n"
                     "stp x29, x30, [sp, #0x50] \n"
                     "stp d8,  d9,  [sp, #0x60] \n"
                     "stp d10, d11, [sp, #0x70] \n"
                     "stp d12, d13, [sp, #0x80] \n"
                     "stp d14, d15, [sp, #0x90] \n"
                     "mov x10, sp \n"
                     "str x10, [x0] \n"
                     "mov sp, x1 \n"
                     "ldp x19, x20, [sp, #0x00] \n"
                     "ldp x21, x22, [sp, #0x10] \n"
                     "ldp x23, x24, [sp, #0x20] \n"
                     "ldp x25, x26, [sp, #0x30] \n"
                     "ldp x27, x28, [sp, #0x40] \n"
                     "ldp x29, x30, [sp, #0x50] \n"
                     "ldp d8,  d9,  [sp, #0x60] \n"
                     "ldp d10, d11, [sp, #0x70] \n"
                     "ldp d12, d13, [sp, #0x80] \n"
                     "ldp d14, d15, [sp, #0x90] \n"
                     "add sp, sp, #160 \n"
                     "ret \n"
                     :
                     :
                     : "memory");
#elif defined(__x86_64__)
#    if defined(_WIN32)
    __asm__ volatile(
        "push %%rbp; push %%rbx; push %%rdi; push %%rsi; push %%r12; push %%r13; push %%r14; push %%r15 \n"
        "sub $160, %%rsp \n" 
        "movups %%xmm6, 0(%%rsp);   movups %%xmm7, 16(%%rsp);  movups %%xmm8, 32(%%rsp);  movups %%xmm9, 48(%%rsp) \n"
        "movups %%xmm10, 64(%%rsp); movups %%xmm11, 80(%%rsp); movups %%xmm12, 96(%%rsp); movups %%xmm13, 112(%%rsp) \n"
        "movups %%xmm14, 128(%%rsp); movups %%xmm15, 144(%%rsp) \n"

        "mov %%rsp, (%%rcx) \n"
        "mov %%rdx, %%rsp \n"

        "movups 0(%%rsp), %%xmm6;   movups 16(%%rsp), %%xmm7;  movups 32(%%rsp), %%xmm8;  movups 48(%%rsp), %%xmm9 \n"
        "movups 64(%%rsp), %%xmm10; movups 80(%%rsp), %%xmm11; movups 96(%%rsp), %%xmm12; movups 112(%%rsp), %%xmm13 \n"
        "movups 128(%%rsp), %%xmm14; movups 144(%%rsp), %%xmm15 \n"
        "add $160, %%rsp \n"
        "pop %%r15; pop %%r14; pop %%r13; pop %%r12; pop %%rsi; pop %%rdi; pop %%rbx; pop %%rbp \n"
        "ret \n" 
        :
        :
        : "memory");
#    else
    __asm__ volatile("push %%rbp; push %%rbx; push %%r12; push %%r13; push %%r14; push %%r15 \n"
                     "mov %%rsp, (%%rdi) \n"
                     "mov %%rsi, %%rsp \n"
                     "pop %%r15; pop %%r14; pop %%r13; pop %%r12; pop %%rbx; pop %%rbp \n"
                     "ret \n" 
                     :
                     :
                     : "memory");
#    endif
#endif
}

// ----------------------------------------------------------------------------
// 2. Scheduler State & Trampoline
// ----------------------------------------------------------------------------

static thread_local MagThread t_main_thread;
static thread_local MagThread *t_current_thread = nullptr;

[[gnu::used, gnu::noinline, gnu::visibility("hidden")]]
void mag_trampoline(void) {
    MagThread *me = t_current_thread;
    if (me->func)
        me->func(me->arg);
    me->is_finished = true;
    while (true)
        MagThread_Yield();
}

[[gnu::naked]] static void mag_trampoline_thunk(void) {
#if defined(__aarch64__)
    __asm__ volatile("b mag_trampoline \n");
#elif defined(__x86_64__)
#    if defined(_WIN32)
    __asm__ volatile(
        "sub $40, %%rsp \n"
        "call mag_trampoline \n" 
        "add $40, %%rsp \n"
        "ret \n"
        : : : "memory"); // Colons required to use %% register prefix
#    else
    __asm__ volatile(
        "sub $8, %%rsp \n" 
        "call mag_trampoline \n" 
        "add $8, %%rsp \n"
        "ret \n"
        : : : "memory");
#    endif
#endif
}

// ----------------------------------------------------------------------------
// 3. API Implementation
// ----------------------------------------------------------------------------

static long g_page_size = 0;
[[gnu::constructor]] static void _mag_init_page_size(void) {
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_page_size = (long)si.dwPageSize;
#else
    g_page_size = sysconf(_SC_PAGESIZE);
#endif
}

void MagThread_InitMain(void) {
    t_main_thread.is_finished = false;
    t_main_thread.is_main     = true;
    t_main_thread.caller      = nullptr;
#if defined(_WIN32)
    NT_TIB *tib               = (NT_TIB *)NtCurrentTeb();
    t_main_thread.stack_base  = tib->StackBase;
    t_main_thread.stack_limit = tib->StackLimit;
#endif
    t_current_thread = &t_main_thread;
}

[[nodiscard, gnu::malloc]]
MagThread *MagThread_Create(size_t stack_size, MagThreadFunc func, void *arg) {
    if (stack_size == 0)
        stack_size = 1024 * 1024;
    stack_size        = (stack_size + g_page_size - 1) & ~(g_page_size - 1);
    size_t total_size = stack_size + (g_page_size * 2);

    void *map;
#if defined(_WIN32)
    map = VirtualAlloc(NULL, total_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!map) return nullptr;
    DWORD old;
    VirtualProtect(map, g_page_size, PAGE_READWRITE | PAGE_GUARD, &old);
#else
    map = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) return nullptr;
    mprotect(map, g_page_size, PROT_NONE);
#endif

    uintptr_t end_addr    = (uintptr_t)map + total_size;
    uintptr_t thread_addr = (end_addr - sizeof(MagThread)) & ~15ULL;
    MagThread *thread     = (MagThread *)thread_addr;

    thread->map_addr    = map;
    thread->map_size    = total_size;
    thread->func        = func;
    thread->arg         = arg;
    thread->is_finished = false;
    thread->is_main     = false;

#if defined(_WIN32)
    thread->stack_base  = (void *)end_addr;
    thread->stack_limit = (void *)((uintptr_t)map + g_page_size);
#endif

    uintptr_t sp = thread_addr;
#if defined(__aarch64__)
    sp -= 160;
    ((uint64_t *)sp)[11] = (uintptr_t)mag_trampoline_thunk; 
#elif defined(__x86_64__)
#    if defined(_WIN32)
    sp -= 8; // Align
    *(uintptr_t *)sp = 0;
    sp -= 8; 
    *(uintptr_t *)sp = (uintptr_t)mag_trampoline_thunk;
    sp -= 224; // 160 XMM + 64 GPR
    for (int i = 0; i < 28; i++) ((uintptr_t *)sp)[i] = 0;
#    else
    sp -= 8;
    *(uintptr_t *)sp = (uintptr_t)mag_trampoline_thunk;
    sp -= 48;
    for (int i = 0; i < 6; i++) ((uintptr_t *)sp)[i] = 0;
#    endif
#endif

    thread->sp = (void *)sp;
    return thread;
}

static inline void mag_swap_teb(MagThread *target) {
#if defined(_WIN32)
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    t_current_thread->stack_base = tib->StackBase;
    t_current_thread->stack_limit = tib->StackLimit;
    tib->StackBase = target->stack_base;
    tib->StackLimit = target->stack_limit;
#else
    (void)target;
#endif
}

[[gnu::always_inline]] MagThread *MagThread_GetCurrent(void) { return t_current_thread; }

void MagThread_Resume(MagThread *target) {
    MagThread *me  = t_current_thread;
    target->caller = me;
    mag_swap_teb(target);
    t_current_thread = target;
    mag_switch(&me->sp, target->sp);
}

void MagThread_Yield(void) {
    MagThread *me     = t_current_thread;
    MagThread *target = me->caller;
    if (!target) return;
    mag_swap_teb(target);
    t_current_thread = target;
    mag_switch(&me->sp, target->sp);
}

void MagThread_Destroy(MagThread *thread) {
    if (thread) {
#if defined(_WIN32)
        VirtualFree(thread->map_addr, 0, MEM_RELEASE);
#else
        munmap(thread->map_addr, thread->map_size);
#endif
    }
}