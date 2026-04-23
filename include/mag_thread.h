#pragma once

#include <stddef.h>
#include <stdint.h>

// Note: In C23, bool, true, and false are native keywords. No <stdbool.h> needed.
// nullptr is also a native keyword.

typedef void (*MagThreadFunc)(void *arg);

typedef struct MagThread MagThread;
struct MagThread {
    void *sp;
    void *map_addr;
    size_t map_size;
    MagThreadFunc func;
    void *arg;
    bool is_finished;
    bool is_main;
    MagThread *caller;
};

void MagThread_InitMain(void);
MagThread *MagThread_Create(size_t stack_size, MagThreadFunc func, void *arg);
void MagThread_Resume(MagThread *target);
void MagThread_Yield(void);
void MagThread_Destroy(MagThread *thread);
MagThread *MagThread_GetCurrent(void);