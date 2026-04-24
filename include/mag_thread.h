#pragma once
// NOLINTBEGIN(llvmlibc-restrict-system-libc-headers)
#include <stddef.h>
// NOLINTEND(llvmlibc-restrict-system-libc-headers)

// Note: In C23, bool, true, and false are native keywords. No <stdbool.h> needed.
// nullptr is also a native keyword.

typedef void (*MagThreadFunc)(void *arg);

typedef struct MagThread MagThread;
static constexpr size_t MagThread_Alignment = 128;
struct MagThread {
    alignas(MagThread_Alignment) void *stack_pointer;
    void *map_addr;
    size_t map_size;
    MagThreadFunc func;
    void *arg;
    MagThread *caller;
#if defined(_WIN32)
    void *stack_base;
    void *stack_limit;
#endif
    bool is_finished;
    bool is_main;
};

void MagThread_InitMain(void);
MagThread *MagThread_Create(size_t stack_size, MagThreadFunc func, void *arg);
void MagThread_Resume(MagThread *target);
void MagThread_Yield(void);
void MagThread_Destroy(MagThread *thread);
MagThread *MagThread_GetCurrent(void);