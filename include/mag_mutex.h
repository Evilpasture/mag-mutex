#pragma once

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifndef NDEBUG
#    define MAG_DEBUG 1
#endif

// Branch prediction hint
#if defined(__GNUC__) || defined(__clang__)
#    define MAG_LIKELY(x) __builtin_expect(!!(x), 1)
#    define MAG_ALWAYS_INLINE __attribute__((always_inline))
#else
#    define MAG_LIKELY(x) (x)
#    define MAG_ALWAYS_INLINE
#endif

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
typedef CRITICAL_SECTION plat_mtx_t;
typedef CONDITION_VARIABLE plat_cnd_t;
typedef DWORD plat_thread_id_t;
#    define PLAT_MTX_INIT(m) InitializeCriticalSection(m)
#    define PLAT_MTX_LOCK(m) EnterCriticalSection(m)
#    define PLAT_MTX_UNLOCK(m) LeaveCriticalSection(m)
#    define PLAT_CND_INIT(c) InitializeConditionVariable(c)
#    define PLAT_CND_WAIT(c, m) SleepConditionVariableCS(c, m, INFINITE)
#    define PLAT_CND_SIGNAL(c) WakeConditionVariable(c)
#    define PLAT_CND_DESTROY(c)
#    define PLAT_CURRENT_THREAD() GetCurrentThreadId()
#    define PLAT_THREADS_EQUAL(t1, t2) ((t1) == (t2))
#else
#    include <pthread.h>
#    include <sched.h>
typedef pthread_mutex_t plat_mtx_t;
typedef pthread_cond_t plat_cnd_t;
typedef pthread_t plat_thread_id_t;
#    define PLAT_MTX_INIT(m) pthread_mutex_init(m, nullptr)
#    define PLAT_MTX_LOCK(m) pthread_mutex_lock(m)
#    define PLAT_MTX_UNLOCK(m) pthread_mutex_unlock(m)
#    define PLAT_CND_INIT(c) pthread_cond_init(c, nullptr)
#    define PLAT_CND_WAIT(c, m) pthread_cond_wait(c, m)
#    define PLAT_CND_SIGNAL(c) pthread_cond_signal(c)
#    define PLAT_CND_DESTROY(c) pthread_cond_destroy(c)
#    define PLAT_CURRENT_THREAD() pthread_self()
#    define PLAT_THREADS_EQUAL(t1, t2) pthread_equal((t1), (t2))
#endif

constexpr uint8_t MAG_UNLOCKED    = 0x00;
constexpr uint8_t MAG_LOCKED      = 0x01;
constexpr uint8_t MAG_HAS_WAITERS = 0x02;
constexpr uint8_t MAG_POISONED    = 0x04;

typedef struct MagMutex {
    _Atomic uint8_t bits;
#ifdef MAG_DEBUG
    _Atomic bool has_owner;
    _Atomic plat_thread_id_t owner;
    _Atomic uint64_t spin_success_count;
    _Atomic uint64_t park_count;
#endif
} MagMutex;

#ifdef MAG_DEBUG
void mag_debug_check_pre_lock(MagMutex *m);
void mag_debug_post_lock(MagMutex *m);
void mag_debug_pre_unlock(MagMutex *m);
void mag_debug_clear_owner(MagMutex *m);
#else
static inline void mag_debug_check_pre_lock(MagMutex *m) { (void)m; }
static inline void mag_debug_post_lock(MagMutex *m) { (void)m; }
static inline void mag_debug_pre_unlock(MagMutex *m) { (void)m; }
static inline void mag_debug_clear_owner(MagMutex *m) { (void)m; }
#endif

void mag_mutex_lock_slow(MagMutex *m);
void mag_mutex_unlock_slow(MagMutex *m);

static inline void mag_mutex_lock(MagMutex *m) {
    mag_debug_check_pre_lock(m);
    uint8_t expected = MAG_UNLOCKED;
    
    // TTAS: Relaxed load avoids heavy CAS loop overhead if contended.
    // Weak CAS generates no loops in ASM, letting the CPU predict the branch perfectly.
    if (atomic_load_explicit(&m->bits, memory_order_relaxed) == MAG_UNLOCKED &&
        atomic_compare_exchange_weak_explicit(&m->bits, &expected, MAG_LOCKED, memory_order_acquire, memory_order_relaxed)) {
        mag_debug_post_lock(m);
        return;
    }
    mag_mutex_lock_slow(m);
}

static inline void mag_mutex_unlock(MagMutex *m) {
    mag_debug_pre_unlock(m);
    mag_debug_clear_owner(m); 

    uint8_t expected = MAG_LOCKED;
    if (atomic_load_explicit(&m->bits, memory_order_relaxed) == MAG_LOCKED &&
        atomic_compare_exchange_weak_explicit(&m->bits, &expected, MAG_UNLOCKED, memory_order_release, memory_order_relaxed)) {
        return;
    }
    mag_mutex_unlock_slow(m);
}