#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <process.h>
#    include <windows.h>

typedef HANDLE os_thread_t;
typedef SRWLOCK os_mutex_t;
typedef CONDITION_VARIABLE os_cond_t;

static inline void os_mutex_init(os_mutex_t *m) { InitializeSRWLock(m); }
static inline void os_mutex_lock(os_mutex_t *m) { AcquireSRWLockExclusive(m); }
static inline void os_mutex_unlock(os_mutex_t *m) { ReleaseSRWLockExclusive(m); }
static inline void os_mutex_destroy(os_mutex_t *m) { (void)m; }

static inline void os_cond_init(os_cond_t *c) { InitializeConditionVariable(c); }
static inline void os_cond_wait(os_cond_t *c, os_mutex_t *m) {
    SleepConditionVariableSRW(c, m, INFINITE, 0);
}
static inline void os_cond_signal(os_cond_t *c) { WakeConditionVariable(c); }
static inline void os_cond_broadcast(os_cond_t *c) { WakeAllConditionVariable(c); }
static inline void os_cond_destroy(os_cond_t *c) { (void)c; }

typedef struct {
    void *(*func)(void *);
    void *arg;
} win_thread_proxy_t;

static unsigned __stdcall win_thread_proxy(void *raw) {
    win_thread_proxy_t *proxy = (win_thread_proxy_t *)raw;
    proxy->func(proxy->arg);
    free(proxy);
    return 0;
}

static inline void os_thread_create(os_thread_t *thread, void *(*start_routine)(void *),
                                    void *arg) {
    win_thread_proxy_t *proxy = (win_thread_proxy_t *)malloc(sizeof(win_thread_proxy_t));
    proxy->func               = start_routine;
    proxy->arg                = arg;
    *thread = (HANDLE)_beginthreadex(nullptr, 0, win_thread_proxy, proxy, 0, nullptr);
}

static inline void os_thread_join(os_thread_t thread) {
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
}

static inline uint64_t get_nanos(void) {
    static LARGE_INTEGER freq;
    static bool init = false;
    if (!init) {
        QueryPerformanceFrequency(&freq);
        init = true;
    }
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    return (uint64_t)((count.QuadPart * 1000000000ULL) / freq.QuadPart);
}

#else
#    include <pthread.h>
#    include <time.h>
#    ifdef __APPLE__
#        include <mach/mach_time.h>
#    endif

typedef pthread_t os_thread_t;
typedef pthread_mutex_t os_mutex_t;
typedef pthread_cond_t os_cond_t;

static inline void os_mutex_init(os_mutex_t *m) { pthread_mutex_init(m, nullptr); }
static inline void os_mutex_lock(os_mutex_t *m) { pthread_mutex_lock(m); }
static inline void os_mutex_unlock(os_mutex_t *m) { pthread_mutex_unlock(m); }
static inline void os_mutex_destroy(os_mutex_t *m) { pthread_mutex_destroy(m); }

static inline void os_cond_init(os_cond_t *c) { pthread_cond_init(c, nullptr); }
static inline void os_cond_wait(os_cond_t *c, os_mutex_t *m) { pthread_cond_wait(c, m); }
static inline void os_cond_signal(os_cond_t *c) { pthread_cond_signal(c); }
static inline void os_cond_broadcast(os_cond_t *c) { pthread_cond_broadcast(c); }
static inline void os_cond_destroy(os_cond_t *c) { pthread_cond_destroy(c); }

static inline void os_thread_create(os_thread_t *thread, void *(*start_routine)(void *),
                                    void *arg) {
    pthread_create(thread, nullptr, start_routine, arg);
}
static inline void os_thread_join(os_thread_t thread) { pthread_join(thread, nullptr); }

static inline uint64_t get_nanos(void) {
#    ifdef __APPLE__
    static mach_timebase_info_data_t info;
    if (info.denom == 0)
        mach_timebase_info(&info);
    return mach_absolute_time() * info.numer / info.denom;
#    else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
#    endif
}
#endif