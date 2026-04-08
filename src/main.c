#include "mag_mutex.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* ============================================================================
 * THE C23 WINDOWS SHIM LAYER (CLANG/GNU Driver)
 * ============================================================================ */
#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <process.h>
#    include <windows.h>

typedef HANDLE pthread_t;

// Proxy to bridge C-Standard thread signatures with Win32 __stdcall requirements
typedef struct {
    void *(*func)(void *);
    void *arg;
} win_thread_proxy_t;

static unsigned __stdcall win_thread_proxy(void *raw) {
    auto proxy = (win_thread_proxy_t *)raw;
    proxy->func(proxy->arg);
    free(proxy);
    return 0;
}

static inline int pthread_create(pthread_t *thread, [[maybe_unused]] void *attr,
                                 void *(*start_routine)(void *), void *arg) {
    auto proxy  = (win_thread_proxy_t *)malloc(sizeof(win_thread_proxy_t));
    proxy->func = start_routine;
    proxy->arg  = arg;

    uintptr_t h = _beginthreadex(nullptr, 0, win_thread_proxy, proxy, 0, nullptr);
    if (!h) {
        free(proxy);
        return -1;
    }
    *thread = (HANDLE)h;
    return 0;
}

static inline int pthread_join(pthread_t thread, [[maybe_unused]] void **retval) {
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return 0;
}

static inline uint64_t get_nanos() {
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

// Map Pthread-like names to Windows SRWLocks for the benchmark
typedef SRWLOCK native_mutex_t;
static inline void native_mutex_init(native_mutex_t *m) { InitializeSRWLock(m); }
static inline void native_mutex_lock(native_mutex_t *m) { AcquireSRWLockExclusive(m); }
static inline void native_mutex_unlock(native_mutex_t *m) { ReleaseSRWLockExclusive(m); }

#else
#    include <pthread.h>
#    include <time.h>
typedef pthread_mutex_t native_mutex_t;
static inline void native_mutex_init(native_mutex_t *m) { pthread_mutex_init(m, nullptr); }
static inline void native_mutex_lock(native_mutex_t *m) { pthread_mutex_lock(m); }
static inline void native_mutex_unlock(native_mutex_t *m) { pthread_mutex_unlock(m); }
static inline uint64_t get_nanos() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#endif

/* ============================================================================
 * BENCHMARK CONFIGURATION (C23 constexpr)
 * ============================================================================ */

static constexpr int MULTI_ITERATIONS = 1'000'000;
static constexpr int NUM_TRIALS       = 5;
static const int THREAD_CONFIGS[]     = {1, 2, 4, 8, 12, 16};
static constexpr int NUM_CONFIGS      = sizeof(THREAD_CONFIGS) / sizeof(int);

typedef struct {
    void *mutex_ptr;
    int type; // 0: Mag, 1: Native
    _Atomic long long *counter;
    _Atomic int ready_count;
    int total_threads;
    uint64_t *elapsed_ns;
} MultiArg;

/* ============================================================================
 * WORKER LOGIC
 * ============================================================================ */

static void *worker_func(void *arg) {
    auto a       = (MultiArg *)arg;
    int my_index = atomic_fetch_add_explicit(&a->ready_count, 1, memory_order_acq_rel);

    // High-frequency spin barrier for synchronized start
    while (atomic_load_explicit(&a->ready_count, memory_order_acquire) < a->total_threads) {
        Mag_CPURelax();
    }

    uint64_t start = get_nanos();

    if (a->type == 0) {
        auto m = (MagMutex *)a->mutex_ptr;
        for (int i = 0; i < MULTI_ITERATIONS; i++) {
            MagMutex_Lock(m);
            atomic_fetch_add_explicit(a->counter, 1, memory_order_relaxed);
            MagMutex_Unlock(m);
        }
    } else {
        auto m = (native_mutex_t *)a->mutex_ptr;
        for (int i = 0; i < MULTI_ITERATIONS; i++) {
            native_mutex_lock(m);
            atomic_fetch_add_explicit(a->counter, 1, memory_order_relaxed);
            native_mutex_unlock(m);
        }
    }

    a->elapsed_ns[my_index] = get_nanos() - start;
    return nullptr;
}

/* ============================================================================
 * SUITE RUNNER
 * ============================================================================ */

static void run_suite(int type, int threads, int trial_id) {
    pthread_t pts[64];
    uint64_t elapsed[64];
    _Atomic long long count = 0;
    const char *labels[]    = {"MagMutex", "WinSRWLock"};

    void *mtx = nullptr;
    if (type == 0) {
        mtx = malloc(sizeof(MagMutex));
        atomic_init(&((MagMutex *)mtx)->bits, MAG_UNLOCKED);
    } else {
        mtx = malloc(sizeof(native_mutex_t));
        native_mutex_init((native_mutex_t *)mtx);
    }

    MultiArg arg = {
        .mutex_ptr     = mtx,
        .type          = type,
        .counter       = &count,
        .ready_count   = 0,
        .total_threads = threads,
        .elapsed_ns    = elapsed,
    };

    for (int i = 0; i < threads; i++)
        pthread_create(&pts[i], nullptr, worker_func, &arg);
    for (int i = 0; i < threads; i++)
        pthread_join(pts[i], nullptr);

    // Calculate tail latency (slowest thread)
    uint64_t max_ns = 0;
    for (int i = 0; i < threads; i++) {
        if (elapsed[i] > max_ns) max_ns = elapsed[i];
    }

    printf("%s,%d,%d,%.2f\n", labels[type], threads, trial_id,
           (double)max_ns / MULTI_ITERATIONS);

    free(mtx);
}

int main() {
    printf("mutex,threads,trial,latency_ns\n");

    for (int c = 0; c < NUM_CONFIGS; c++) {
        int threads = THREAD_CONFIGS[c];
        for (int t = 0; t < NUM_TRIALS; t++) {
            run_suite(0, threads, t); // Mag
            run_suite(1, threads, t); // WinSRW
        }
    }

    return 0;
}