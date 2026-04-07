#pragma once

/**
 * @file mag_mutex.h
 * @brief MagMutex: A High-Performance, Memory-Efficient 1-Byte Mutex.
 *
 * DESIGN GOALS:
 * 1. Minimal Footprint: 1 byte per mutex in Release mode (ideal for ECS/Large Object Graphs).
 * 2. High Contention Throughput: Uses a Decoupled Unlock Protocol and Address Sharding
 *    to eliminate "Thundering Herd" and False-Sharing bottlenecks.
 * 3. Cross-Platform: Specialized assembly for ARM64 (Apple Silicon) and x86_64.
 * 4. Safety: Built-in Deadlock Detection and Recursive Lock checks in Debug mode.
 *
 * BASIC USAGE:
 * @code
 *    MagMutex my_lock = { .bits = MAG_UNLOCKED }; 
 *    // or zero-init is fine
 *    MagMutex my_lock = {}; 
 *
 *    MagMutex_Lock(&my_lock);
 *    // ... critical section ...
 *    MagMutex_Unlock(&my_lock);
 * @endcode
 *
 * INITIALIZATION:
 * MagMutex can be zero-initialized or initialized with { .bits = MAG_UNLOCKED }.
 * In Release mode (NDEBUG defined), the struct is exactly 8 bits. In Debug mode,
 * the struct grows to include ownership and diagnostic metadata.
 */

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifndef NDEBUG
#    define MAG_DEBUG 1
#endif

#if defined(__GNUC__) || defined(__clang__)
#    define MAG_LIKELY(x) __builtin_expect(!!(x), 1)
#    define MAG_COLD __attribute__((cold))
#    define MAG_ALWAYS_INLINE __attribute__((always_inline))
#else
#    define MAG_LIKELY(x) (x)
#    define MAG_COLD
#    define MAG_ALWAYS_INLINE
#endif

#if defined(_WIN32)
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

// --- MagMutex States ---

constexpr uint8_t MAG_UNLOCKED    = 0x00;
constexpr uint8_t MAG_LOCKED      = 0x01;
constexpr uint8_t MAG_HAS_WAITERS = 0x02;
constexpr uint8_t MAG_POISONED    = 0x04;

/**
 * @struct MagMutex
 * @brief The core mutex structure. 
 */
typedef struct MagMutex {
    _Atomic uint8_t bits;
#ifdef MAG_DEBUG
    _Atomic bool has_owner;
    _Atomic plat_thread_id_t owner;
    _Atomic uint64_t spin_success_count;
    _Atomic uint64_t park_count;
#endif
} MagMutex;

// --- Debug Prototypes ---

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


MAG_COLD void MagMutex_LockSlow(MagMutex *m);
MAG_COLD void MagMutex_UnlockSlow(MagMutex *m);

// --- Public API ---

/**
 * @brief Permanently poisons the mutex. Any subsequent lock attempt will abort.
 */
static inline void MagMutex_Poison(MagMutex *m) {
    atomic_fetch_or_explicit(&m->bits, MAG_POISONED, memory_order_release);
}

/**
 * @brief Attempts to acquire the lock without blocking.
 * @return true if acquired, false if contended.
 */
static inline bool MagMutex_TryLock(MagMutex *m) {
    mag_debug_check_pre_lock(m);
    uint8_t expected = MAG_UNLOCKED;
    bool success     = atomic_compare_exchange_strong_explicit(
        &m->bits, &expected, MAG_LOCKED, memory_order_acquire, memory_order_relaxed);
    if (success) {
        mag_debug_post_lock(m);
    }
    return success;
}

/**
 * @brief Acquires the lock. Blocks if the lock is held by another thread.
 * Uses an adaptive spin-then-park strategy.
 */
static MAG_ALWAYS_INLINE inline void MagMutex_Lock(MagMutex *m) {
    mag_debug_check_pre_lock(m);
    uint8_t expected = MAG_UNLOCKED;
    // Fast-path: Uncontended Acquire
    if (MAG_LIKELY(atomic_compare_exchange_strong_explicit(&m->bits, &expected, MAG_LOCKED, 
                                                           memory_order_acquire, memory_order_relaxed))) {
        mag_debug_post_lock(m);
        return;
    }
    MagMutex_LockSlow(m);
}

/**
 * @brief Releases the lock. Wakes one waiting thread if necessary.
 */
static MAG_ALWAYS_INLINE inline void MagMutex_Unlock(MagMutex *m) {
    mag_debug_pre_unlock(m);
    mag_debug_clear_owner(m); 
    
    uint8_t expected = MAG_LOCKED;
    // Fast-path: Uncontended Release
    if (MAG_LIKELY(atomic_compare_exchange_strong_explicit(&m->bits, &expected, MAG_UNLOCKED, 
                                                           memory_order_release, memory_order_relaxed))) {
        return;
    }
    MagMutex_UnlockSlow(m);
}