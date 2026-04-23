#include "mag_thread.h"
// NOLINTBEGIN(llvmlibc-restrict-system-libc-headers)
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#else
#    include <sys/mman.h>
#    include <unistd.h>
#endif
// NOLINTEND(llvmlibc-restrict-system-libc-headers)

// Prototypes for functions defined in mag_asm.S
extern void mag_switch(void **old_sp, void *new_sp);
extern void mag_trampoline_asm(void);
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static thread_local MagThread t_main_thread;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static thread_local MagThread *t_current_thread = nullptr;

void mag_trampoline(void);
// Real C entry point called by mag_asm
void mag_trampoline(void) {
    MagThread *self = t_current_thread;
    if (self->func) {
        self->func(self->arg);
    }
    self->is_finished = true;
    // NOLINTNEXTLINE(altera-unroll-loops)
    while (true) {
        MagThread_Yield();
    }
}

static inline void mag_swap_teb(MagThread *target) {
#if defined(_WIN32)
    NT_TIB *tib                   = (NT_TIB *)NtCurrentTeb();
    t_current_thread->stack_base  = tib->StackBase;
    t_current_thread->stack_limit = tib->StackLimit;
    tib->StackBase                = target->stack_base;
    tib->StackLimit               = target->stack_limit;
#else
    (void)target;
#endif
}

static size_t get_page_size(void) {
#if defined(_WIN32)
    SYSTEM_INFO sys_info = {};
    GetSystemInfo(&sys_info);
    return (size_t)sys_info.dwPageSize;
#else
    long sz = sysconf(_SC_PAGESIZE);
    // If sysconf fails, it returns -1. We provide a sane 4KB default.
    return (sz > 0) ? (size_t)sz : 4096;
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

MagThread *MagThread_Create(size_t stack_size, MagThreadFunc func, void *arg) {
    size_t page_size         = get_page_size();
    constexpr size_t ONE_MIB = 1024ULL * 1024ULL;
    if (stack_size == 0) {
        stack_size = ONE_MIB;
    }
    stack_size        = (stack_size + page_size - 1) & ~(page_size - 1);
    size_t total_size = stack_size + ((long long)page_size * 2);

    void *map = nullptr;
#if defined(_WIN32)
    map = VirtualAlloc(nullptr, total_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!map) {
        return nullptr;
    }
    DWORD old = 0;
    VirtualProtect(map, page_size, PAGE_READWRITE | PAGE_GUARD, &old);
#else
    map = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) {
        return nullptr;
    }
    mprotect(map, page_size, PROT_NONE);
#endif
    constexpr size_t STACK_ALIGNMENT  = 16ULL;
    constexpr size_t STACK_ALIGN_MASK = STACK_ALIGNMENT - 1ULL;
    uintptr_t end_addr                = (uintptr_t)map + total_size;
    uintptr_t thread_addr             = (end_addr - sizeof(MagThread)) & ~STACK_ALIGN_MASK;
    MagThread *thread                 = nullptr;
    memcpy((void *)&thread, &thread_addr, sizeof(MagThread *));

    thread->map_addr    = map;
    thread->map_size    = total_size;
    thread->func        = func;
    thread->arg         = arg;
    thread->is_finished = false;
    thread->is_main     = false;

#if defined(_WIN32)
    uintptr_t limit_addr = (uintptr_t)map + page_size;
    
    // Copy the bits of the integer directly into the pointer
    memcpy((void *)&thread->stack_base,  &end_addr,   sizeof(void *));
    memcpy((void *)&thread->stack_limit, &limit_addr, sizeof(void *));
#endif

    uintptr_t stack_pointer = (uintptr_t)thread;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
    static_assert(alignof(MagThread) % 16 == 0, 
    "MagThread struct alignment must be a multiple of 16 to ensure the stack starts aligned");

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    // Symmetry: 8 (Ret) + 8 (Dummy) + 64 (GPR) + 160 (XMM) = 240
    constexpr size_t WIN64_CTX_SZ = 232ULL; // Everything except the Ret addr
    constexpr size_t WIN64_RET_SZ = 8ULL;
    constexpr size_t WIN64_TOTAL  = WIN64_CTX_SZ + WIN64_RET_SZ; // 240

    // Using the constants defined in the previous working block:
    // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
    static_assert(WIN64_TOTAL % 16 == 0, 
        "Windows x64 total stack frame must be a multiple of 16 for movaps alignment");
    
    static_assert(WIN64_TOTAL == 240, 
        "Windows x64 stack frame size changed; check mag_asm.S synchronization");
        
    static_assert((WIN64_CTX_SZ + 8) == WIN64_TOTAL,
        "Windows x64 Context + Return Address mismatch");
    // NOLINTEND(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)

    void *dest_ptr = nullptr;
    uintptr_t val_to_write = 0;

    // 1. Align stack pointer (thread starts 16-aligned)
    // 0x...00 - 240 (0xF0) = 0x...10 (Still 16-aligned!)
    stack_pointer -= WIN64_TOTAL;

    // 2. The 'ret' in mag_switch happens after 'add 160' and 'pop 9' (160+72=232)
    uintptr_t ret_addr_loc = stack_pointer + WIN64_CTX_SZ;
    val_to_write = (uintptr_t)mag_trampoline_asm;
    memcpy((void*)&dest_ptr, &ret_addr_loc, sizeof(void *));
    memcpy(dest_ptr, &val_to_write, sizeof(uintptr_t));

    // 3. Zero the context
    memcpy((void*)&dest_ptr, &stack_pointer, sizeof(void *));
    memset(dest_ptr, 0, WIN64_CTX_SZ);

    // Save the pointer
    memcpy((void *)&thread->stack_pointer, &stack_pointer, sizeof(void *));
#elif defined(__x86_64__) || defined(_M_X64)
    [[maybe_unused]] constexpr size_t SYSV_GPR_STORAGE  = 48ULL; // 6 registers (rbx, rbp, r12-r15)
    
    static_assert(SYSV_GPR_STORAGE % 8 == 0, "System V GPR storage must be 8-byte multiples");
    // System V ABI (Linux/macOS): Only 6 GPRs are callee-saved
    void *dest_ptr;
    uintptr_t val_to_write;

    // 1. Set Trampoline (Initial RIP)
    stack_pointer -= GPR_SIZE;
    val_to_write = (uintptr_t)mag_trampoline_asm;
    memcpy((void*)&dest_ptr, &stack_pointer, sizeof(void *));
    memcpy(dest_ptr, &val_to_write, GPR_SIZE);

    // 2. Zero the saved register area (RBX, RBP, R12-R15)
    stack_pointer -= SYSV_GPR_STORAGE;
    memcpy((void*)&dest_ptr, &stack_pointer, sizeof(void *));
    memset(dest_ptr, 0, SYSV_GPR_STORAGE);

#elif defined(__aarch64__) || defined(_M_ARM64)
    [[maybe_unused]] constexpr size_t ARM64_CONTEXT_SZ  = 160ULL;
    // ARM64: Save area for X19-X30 and D8-D15
    void *dest_ptr;
    uintptr_t val_to_write;

    // 1. Zero out the context save area
    stack_pointer -= ARM64_CONTEXT_SZ;
    memcpy((void*)&dest_ptr, &stack_pointer, sizeof(void *));
    memset(dest_ptr, 0, ARM64_CONTEXT_SZ);
    
    // 2. Set Trampoline into the Link Register (x30) slot
    // Offset 88 is typically where x30 (LR) is stored in the save block
    uintptr_t lr_offset = 11ULL * GPR_SIZE;
    uintptr_t lr_addr   = stack_pointer + lr_offset;
    val_to_write = (uintptr_t)mag_trampoline_asm;
    
    memcpy((void*)&dest_ptr, &lr_addr, sizeof(void *));
    memcpy(dest_ptr, &val_to_write, GPR_SIZE);
#endif

    // Use memcpy to avoid the provenance/optimization warning
    memcpy((void *)&thread->stack_pointer, &stack_pointer, sizeof(void *));

    return thread;
}

void MagThread_Resume(MagThread *target) {
    MagThread *self  = t_current_thread;
    target->caller = self;
    mag_swap_teb(target);
    t_current_thread = target;
    mag_switch(&self->stack_pointer, target->stack_pointer);
}

void MagThread_Yield(void) {
    MagThread *self     = t_current_thread;
    MagThread *target = self->caller;
    if (!target) {
        return;
    }
    mag_swap_teb(target);
    t_current_thread = target;
    mag_switch(&self->stack_pointer, target->stack_pointer);
}

void MagThread_Destroy(MagThread *thread) {
    if (!thread) {
        return;
    }
#if defined(_WIN32)
    VirtualFree(thread->map_addr, 0, MEM_RELEASE);
#else
    munmap(thread->map_addr, thread->map_size);
#endif
}

MagThread *MagThread_GetCurrent(void) { return t_current_thread; }