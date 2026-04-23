#include "mag_thread.h"
#include "mag_mutex.h"
#include <assert.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

// Cache page size at startup
static long g_page_size = 0;
[[gnu::constructor]] static void _mag_init_page_size() { g_page_size = sysconf(_SC_PAGESIZE); }

// ----------------------------------------------------------------------------
// 1. Optimized Context Switch
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
                     "ret \n");
#elif defined(__x86_64__)
    __asm__ volatile("push %%rbp; push %%rbx; push %%r12; push %%r13; push %%r14; push %%r15 \n"
                     "mov %%rsp, (%%rdi) \n"
                     "mov %%rsi, %%rsp \n"
                     "pop %%r15; pop %%r14; pop %%r13; pop %%r12; pop %%rbx; pop %%rbp \n"
                     "ret \n" ::
                         : "memory");
#endif
}

// ----------------------------------------------------------------------------
// 2. Scheduler State
// ----------------------------------------------------------------------------

static thread_local MagThread t_main_thread;
static thread_local MagThread *t_current_thread = nullptr;

[[gnu::noreturn]] static void mag_trampoline(void) {
    MagThread *me = t_current_thread;
    me->func(me->arg);
    me->is_finished = true;
    while (true)
        MagThread_Yield();
}

// ----------------------------------------------------------------------------
// 3. Optimized API
// ----------------------------------------------------------------------------

void MagThread_InitMain(void) {
    t_main_thread.is_finished = false;
    t_main_thread.is_main     = true;
    t_main_thread.caller      = nullptr;
    t_current_thread          = &t_main_thread;
}

[[nodiscard, gnu::malloc, gnu::hot, gnu::nonnull(2)]]
MagThread *MagThread_Create(size_t stack_size, MagThreadFunc func, void *arg) {
    // 1. Normalize stack size to page boundaries.
    if (MAG_UNLIKELY(stack_size == 0)) {
        stack_size = 1024 * 1024; // 1MB Default
    }
    stack_size = (stack_size + g_page_size - 1) & ~(g_page_size - 1);

    // 2. Layout: [Guard Page (PROT_NONE)] [Stack Memory] [Metadata Page]
    // We allocate an extra page for the control block to avoid "stealing"
    // space from the user's requested stack and to keep metadata isolated.
    size_t total_size = stack_size + (g_page_size * 2);

    void *map =
        mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (MAG_UNLIKELY(map == MAP_FAILED)) {
        return nullptr;
    }

    // 3. Apply Hardware Minefield (Bottom Guard Page)
    if (MAG_UNLIKELY(mprotect(map, g_page_size, PROT_NONE) != 0)) {
        munmap(map, total_size);
        return nullptr;
    }

    // 4. Place MagThread control block at the very top of the mapping.
    // We align downward from the end address to a 16-byte boundary to satisfy
    // strict alignment requirements for 64-bit pointers and SIMD state.
    uintptr_t end_addr    = (uintptr_t)map + total_size;
    uintptr_t thread_addr = (end_addr - sizeof(MagThread)) & ~15ULL;
    MagThread *thread     = (MagThread *)thread_addr;

    // 5. Initialize fields (Fast path: avoids calloc/memset overhead)
    thread->map_addr    = map;
    thread->map_size    = total_size;
    thread->func        = func;
    thread->arg         = arg;
    thread->is_finished = false;
    thread->is_main     = false;
    thread->caller      = nullptr;

    // 6. Forge the initial stack frame.
    // We start the stack just below our control block.
    // thread_addr is already 16-byte aligned.
    uintptr_t sp = thread_addr;

#if defined(__aarch64__)
    // ARM64: Reserve 160 bytes for callee-saved registers (x19-x28),
    // Frame Pointer (x29), and Link Register (x30).
    sp -= 160;
    uint64_t *stack_ptr = (uint64_t *)sp;

    // x29 (FP) is at index 10, x30 (LR) is at index 11.
    stack_ptr[10] = 0;
    stack_ptr[11] = (uintptr_t)mag_trampoline;
#elif defined(__x86_64__)
    // x86_64: Subtract 8 for the return address, then 48 for callee-saved regs.
    sp -= 8;
    *(uintptr_t *)sp = (uintptr_t)mag_trampoline;

    sp -= 48; // Space for rbp, rbx, r12, r13, r14, r15
    for (int i = 0; i < 6; i++) {
        ((uintptr_t *)sp)[i] = 0;
    }
#endif

    thread->sp = (void *)sp;
    return thread;
}

[[gnu::always_inline]] MagThread *MagThread_GetCurrent(void) { return t_current_thread; }

[[gnu::hot]] void MagThread_Resume(MagThread *target) {
    MagThread *me    = t_current_thread;
    target->caller   = me;
    t_current_thread = target;
    mag_switch(&me->sp, target->sp);
}

[[gnu::hot]] void MagThread_Yield(void) {
    MagThread *me     = t_current_thread;
    MagThread *target = me->caller;
    t_current_thread  = target;
    mag_switch(&me->sp, target->sp);
}

void MagThread_Destroy(MagThread *thread) {
    if (thread) {
        // Since the struct is inside the map, munmap frees everything
        munmap(thread->map_addr, thread->map_size);
    }
}