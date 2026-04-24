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

// NOLINTBEGIN(llvmlibc-restrict-system-libc-headers)
#include <assert.h>
#ifdef __cplusplus
#    include <atomic>
// Map C11/C23 names to C++ names for the inline functions
using std::memory_order_acq_rel;
using std::memory_order_acquire;
using std::memory_order_relaxed;
using std::memory_order_release;
#    define MAG_ATOMIC(T) std::atomic<T>
#else
#    include <stdatomic.h>
#    define MAG_ATOMIC(T) _Atomic T
#endif
#include <stdint.h>

#ifdef __cplusplus
#    define MAG_ATOMIC_CX(obj, exp, des, succ, fail)                                               \
        std::atomic_compare_exchange_strong_explicit(obj, exp, des, succ, fail)
#else
#    define MAG_ATOMIC_CX(obj, exp, des, succ, fail)                                               \
        atomic_compare_exchange_strong_explicit(obj, exp, des, succ, fail)
#endif

#ifndef NDEBUG
#    define MAG_DEBUG 1
#endif

#if defined(__GNUC__) || defined(__clang__)
#    define MAG_LIKELY(x) __builtin_expect(!!(x), 1)
#    define MAG_UNLIKELY(x) __builtin_expect(!!(x), 0)
#    define MAG_COLD __attribute__((cold))
#    define MAG_ALWAYS_INLINE __attribute__((always_inline))
#    define MAG_ASSUME(x) __builtin_assume(x)
#else
#    define MAG_LIKELY(x) (x)
#    define MAG_UNLIKELY(x) (x)
#    define MAG_COLD
#    define MAG_ALWAYS_INLINE
#    define MAG_ASSUME(x) (x)
#endif

#if defined(MAG_EXPORT_INTERNAL) || !defined(__GNUC__)
#    define MAG_INTERNAL
#else
#    define MAG_INTERNAL [[gnu::visibility("hidden")]]
#endif

// NOLINTBEGIN(readability-identifier-naming)
#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>

/* --- I HATE WINDOWS --- */
#    undef MemoryBarrier
#    undef far
#    undef near
#    undef ERROR
/* ---------------------- */
typedef CRITICAL_SECTION plat_mtx_t;
typedef CONDITION_VARIABLE plat_cnd_t;
typedef DWORD plat_thread_id_t;
#    define PLAT_MTX_INIT(mod) InitializeCriticalSection(mod)
#    define PLAT_MTX_LOCK(mod) EnterCriticalSection(mod)
#    define PLAT_MTX_UNLOCK(mod) LeaveCriticalSection(mod)
#    define PLAT_CND_INIT(condvar) InitializeConditionVariable(condvar)
#    define PLAT_CND_WAIT(condvar, mod) SleepConditionVariableCS(condvar, mod, INFINITE)
#    define PLAT_CND_SIGNAL(condvar) WakeConditionVariable(condvar)
#    define PLAT_CND_DESTROY(condvar)
#    define PLAT_CURRENT_THREAD() GetCurrentThreadId()
#    define PLAT_THREADS_EQUAL(t1, t2) ((t1) == (t2))
#else
#    include <pthread.h>
#    include <sched.h>
typedef pthread_mutex_t plat_mtx_t;
typedef pthread_cond_t plat_cnd_t;
typedef pthread_t plat_thread_id_t;
#    define PLAT_MTX_INIT(mod) pthread_mutex_init(mod, nullptr)
#    define PLAT_MTX_LOCK(mod) pthread_mutex_lock(mod)
#    define PLAT_MTX_UNLOCK(mod) pthread_mutex_unlock(mod)
#    define PLAT_CND_INIT(condvar) pthread_cond_init(condvar, nullptr)
#    define PLAT_CND_WAIT(condvar, mod) pthread_cond_wait(condvar, mod)
#    define PLAT_CND_SIGNAL(condvar) pthread_cond_signal(condvar)
#    define PLAT_CND_DESTROY(condvar) pthread_cond_destroy(condvar)
#    define PLAT_CURRENT_THREAD() pthread_self()
#    define PLAT_THREADS_EQUAL(t1, t2) pthread_equal((t1), (t2))
#endif
// NOLINTEND(readability-identifier-naming)
// --- Memory Ordering Configuration ---

#if defined(MAG_FORCE_CASAL)
// Experimental: Use Full Barriers for both Lock and Unlock
#    define MAG_LOCK_ORDER memory_order_acq_rel
#    define MAG_UNLOCK_ORDER memory_order_acq_rel
#else
// Standard: Standard Acquire for Lock, Release for Unlock
#    define MAG_LOCK_ORDER memory_order_acquire
#    define MAG_UNLOCK_ORDER memory_order_release
#endif
// NOLINTEND(llvmlibc-restrict-system-libc-headers)

// --- MagMutex States ---

constexpr uint8_t MAG_UNLOCKED    = 0x00;
constexpr uint8_t MAG_LOCKED      = 0x01;
constexpr uint8_t MAG_HAS_WAITERS = 0x02;
constexpr uint8_t MAG_POISONED    = 0x04;
#ifdef MAG_DEBUG
constexpr uint8_t MAG_DEBUG_ALIGN = 32;
#endif
/**
 * @struct MagMutex
 * @brief The core mutex structure.
 */
typedef struct MagMutex {
#ifdef MAG_DEBUG
    alignas(MAG_DEBUG_ALIGN)
#endif
        MAG_ATOMIC(uint8_t) bits;
#ifdef MAG_DEBUG
    MAG_ATOMIC(bool) has_owner;
    MAG_ATOMIC(uintptr_t) owner;
    MAG_ATOMIC(uint64_t) spin_success_count;
    MAG_ATOMIC(uint64_t) park_count;
#endif
} MagMutex;

// --- Debug Prototypes ---

#ifdef MAG_DEBUG
void mag_debug_check_pre_lock(MagMutex *mod);
void mag_debug_post_lock(MagMutex *mod);
void mag_debug_pre_unlock(MagMutex *mod);
void mag_debug_clear_owner(MagMutex *mod);
#else
static inline void mag_debug_check_pre_lock(MagMutex *mod) { (void)mod; }
static inline void mag_debug_post_lock(MagMutex *mod) { (void)mod; }
static inline void mag_debug_pre_unlock(MagMutex *mod) { (void)mod; }
static inline void mag_debug_clear_owner(MagMutex *mod) { (void)mod; }
#endif

[[gnu::cold, gnu::noinline, gnu::nonnull(1)]]
// NOLINTNEXTLINE(readability-identifier-naming)
void MagMutex_LockSlow(MagMutex *mod);
[[gnu::cold, gnu::noinline, gnu::nonnull(1)]]
// NOLINTNEXTLINE(readability-identifier-naming)
void MagMutex_UnlockSlow(MagMutex *mod);

// --- Public API ---

// NOLINTBEGIN(hicpp-no-assembler, cppcoreguidelines-pro-type-inline-assembly)
/**
 * Mag_CPURelax: Hardware-specific hint to the CPU that we are in a spin-loop.
 * On ARM64 (Apple Silicon), this uses the 'yield' instruction to save power
 * and potentially allow the other SMT thread to progress.
 */
// NOLINTNEXTLINE(readability-identifier-naming)
static inline void Mag_CPURelax() {
#if defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#elif defined(_WIN32)
    YieldProcessor();
#endif
}
// NOLINTEND(hicpp-no-assembler, cppcoreguidelines-pro-type-inline-assembly)

/**
 * @brief Permanently poisons the mutex. Any subsequent lock attempt will abort.
 */
// NOLINTNEXTLINE(readability-identifier-naming)
static inline void MagMutex_Poison(MagMutex *mod) {
    atomic_fetch_or_explicit(&mod->bits, MAG_POISONED, memory_order_release);
}

/**
 * @brief Attempts to acquire the lock without blocking.
 * @return true if acquired, false if contended.
 */
[[gnu::flatten]] [[gnu::hot]] [[gnu::always_inline]] [[gnu::artificial]]
// NOLINTNEXTLINE(readability-identifier-naming)
static inline bool MagMutex_TryLock(MagMutex *mod) {
    mag_debug_check_pre_lock(mod);
    uint8_t expected = MAG_UNLOCKED;
    bool success     = MAG_ATOMIC_CX(&mod->bits, &expected, MAG_LOCKED, memory_order_acquire,
                                     memory_order_relaxed);
    if (success) {
        mag_debug_post_lock(mod);
    }
    return success;
}

/**
 * @brief Acquires the lock.
 * On ARM64 with MAG_FORCE_CASAL, this emits 'casalb'.
 * Otherwise, emits 'casab' (Acquire).
 */
[[gnu::flatten, gnu::hot, gnu::always_inline, gnu::artificial, gnu::nonnull(1)]]
// NOLINTNEXTLINE(readability-identifier-naming)
static inline void MagMutex_Lock(MagMutex *mod) {
    mag_debug_check_pre_lock(mod);
    uint8_t expected = MAG_UNLOCKED;

    if (MAG_LIKELY(MAG_ATOMIC_CX(&mod->bits, &expected, MAG_LOCKED, MAG_LOCK_ORDER,
                                 memory_order_relaxed))) {
        mag_debug_post_lock(mod);
        return;
    }
    MagMutex_LockSlow(mod);
}

/**
 * @brief Releases the lock.
 * On ARM64 with MAG_FORCE_CASAL, this emits 'casalb'.
 * Otherwise, emits 'caslb' (Release).
 */
[[gnu::flatten, gnu::hot, gnu::always_inline, gnu::artificial, gnu::nonnull(1)]]
// NOLINTNEXTLINE(readability-identifier-naming)
static inline void MagMutex_Unlock(MagMutex *mod) {
    mag_debug_pre_unlock(mod);
    mag_debug_clear_owner(mod);

    uint8_t expected = MAG_LOCKED;

    if (MAG_LIKELY(MAG_ATOMIC_CX(&mod->bits, &expected, MAG_UNLOCKED, MAG_UNLOCK_ORDER,
                                 memory_order_relaxed))) {
        return;
    }
    MagMutex_UnlockSlow(mod);
}

// --- MagCond (Condition Variable) ---

/**
 * @struct MagCond
 * @brief A 1-Byte Condition Variable.
 *
 * Works seamlessly with MagMutex. Uses the same parking lot backend to
 * provide ultra-low memory footprint synchronization.
 */
typedef struct MagCond {
    MAG_ATOMIC(uint8_t) bits;
} MagCond;

/**
 * @brief Initializes a MagCond.
 */
// NOLINTNEXTLINE(readability-identifier-naming)
static inline void MagCond_Init(MagCond *condvar) {
#ifdef __cplusplus
    condvar->bits.store(0, std::memory_order_relaxed);
#else
    atomic_init(&condvar->bits, 0);
#endif
}

/**
 * @brief Atomically releases the mutex and causes the calling thread to block on the condition
 * variable. Upon successful return, the mutex shall have been locked and is owned by the calling
 * thread.
 */
// NOLINTNEXTLINE(readability-identifier-naming)
void MagCond_Wait(MagCond *condvar, MagMutex *mod);

/**
 * @brief Unblocks at least one of the threads that are blocked on the specified condition variable.
 */
// NOLINTNEXTLINE(readability-identifier-naming)
void MagCond_Signal(MagCond *condvar);

/**
 * @brief Unblocks all threads currently blocked on the specified condition variable.
 */
// NOLINTNEXTLINE(readability-identifier-naming)
void MagCond_Broadcast(MagCond *condvar);