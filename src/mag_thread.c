#include "mag_thread.h"
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#else
#    include <sys/mman.h>
#    include <unistd.h>
#endif

// Prototypes for functions defined in mag_asm.S
extern void mag_switch(void **old_sp, void *new_sp);
extern void mag_trampoline_asm(void);

static thread_local MagThread t_main_thread;
static thread_local MagThread *t_current_thread = NULL;

// Real C entry point called by mag_trampoline_asm
void mag_trampoline(void) {
    MagThread *me = t_current_thread;
    if (me->func) me->func(me->arg);
    me->is_finished = true;
    while (1) MagThread_Yield();
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

static long get_page_size(void) {
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (long)si.dwPageSize;
#else
    return sysconf(_SC_PAGESIZE);
#endif
}

void MagThread_InitMain(void) {
    t_main_thread.is_finished = false;
    t_main_thread.is_main     = true;
    t_main_thread.caller      = NULL;
#if defined(_WIN32)
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    t_main_thread.stack_base  = tib->StackBase;
    t_main_thread.stack_limit = tib->StackLimit;
#endif
    t_current_thread = &t_main_thread;
}

MagThread *MagThread_Create(size_t stack_size, MagThreadFunc func, void *arg) {
    long page_size = get_page_size();
    if (stack_size == 0) stack_size = 1024 * 1024;
    stack_size = (stack_size + page_size - 1) & ~(page_size - 1);
    size_t total_size = stack_size + (page_size * 2);

    void *map;
#if defined(_WIN32)
    map = VirtualAlloc(NULL, total_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!map) return NULL;
    DWORD old;
    VirtualProtect(map, page_size, PAGE_READWRITE | PAGE_GUARD, &old);
#else
    map = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) return NULL;
    mprotect(map, page_size, PROT_NONE);
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
    thread->stack_limit = (void *)((uintptr_t)map + page_size);
#endif

    uintptr_t sp = (uintptr_t)thread;

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    sp -= 8; // Alignment
    *(uintptr_t *)sp = 0;
    sp -= 8; 
    *(uintptr_t *)sp = (uintptr_t)mag_trampoline_asm;
    sp -= 224; // 160 XMM + 64 GPR
    memset((void*)sp, 0, 224);
#elif defined(__x86_64__) || defined(_M_X64)
    sp -= 8;
    *(uintptr_t *)sp = (uintptr_t)mag_trampoline_asm;
    sp -= 48; // 6 GPRs
    memset((void*)sp, 0, 48);
#elif defined(__aarch64__) || defined(_M_ARM64)
    sp -= 160;
    memset((void*)sp, 0, 160);
    ((uint64_t *)sp)[11] = (uintptr_t)mag_trampoline_asm; 
#endif

    thread->sp = (void *)sp;
    return thread;
}

void MagThread_Resume(MagThread *target) {
    MagThread *me = t_current_thread;
    target->caller = me;
    mag_swap_teb(target);
    t_current_thread = target;
    mag_switch(&me->sp, target->sp);
}

void MagThread_Yield(void) {
    MagThread *me = t_current_thread;
    MagThread *target = me->caller;
    if (!target) return;
    mag_swap_teb(target);
    t_current_thread = target;
    mag_switch(&me->sp, target->sp);
}

void MagThread_Destroy(MagThread *thread) {
    if (!thread) return;
#if defined(_WIN32)
    VirtualFree(thread->map_addr, 0, MEM_RELEASE);
#else
    munmap(thread->map_addr, thread->map_size);
#endif
}

MagThread *MagThread_GetCurrent(void) {
    return t_current_thread;
}