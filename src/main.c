#define PY_SSIZE_T_CLEAN
#include "mag_mutex.h"
#include <Python.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static constexpr int SINGLE_ITERATIONS = 10000000;
static constexpr int MULTI_ITERATIONS  = 1000000;

// --- Timing Helpers ---

static inline uint64_t get_nanos(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// --- Benchmark Payload ---

typedef struct {
    MagMutex *mag;
    pthread_mutex_t *pth;
    PyMutex *py;
    int iters;
    _Atomic long long *counter;

    // Barrier for synchronized start: all threads spin until
    // ready_count == total_threads, ensuring no thread gets an
    // uncontended fast-path head start.
    _Atomic int ready_count;
    int total_threads;

    // Each thread writes its own elapsed time here.
    // Caller allocates [total_threads] entries.
    uint64_t *elapsed_ns;
} MultiArg;

static void *multi_mag_worker(void *arg) {
    MultiArg *a  = (MultiArg *)arg;
    int my_index = atomic_fetch_add_explicit(&a->ready_count, 1, memory_order_acq_rel);

    // Spin until every thread is ready.
    while (atomic_load_explicit(&a->ready_count, memory_order_acquire) < a->total_threads)
        ;

    uint64_t start = get_nanos();
    for (int i = 0; i < a->iters; i++) {
        MagMutex_Lock(a->mag);
        atomic_fetch_add_explicit(a->counter, 1, memory_order_relaxed);
        MagMutex_Unlock(a->mag);
    }
    a->elapsed_ns[my_index] = get_nanos() - start;
    return nullptr;
}

static void *multi_pth_worker(void *arg) {
    MultiArg *a  = (MultiArg *)arg;
    int my_index = atomic_fetch_add_explicit(&a->ready_count, 1, memory_order_acq_rel);

    while (atomic_load_explicit(&a->ready_count, memory_order_acquire) < a->total_threads)
        ;

    uint64_t start = get_nanos();
    for (int i = 0; i < a->iters; i++) {
        pthread_mutex_lock(a->pth);
        atomic_fetch_add_explicit(a->counter, 1, memory_order_relaxed);
        pthread_mutex_unlock(a->pth);
    }
    a->elapsed_ns[my_index] = get_nanos() - start;
    return nullptr;
}

static void *multi_py_worker(void *arg) {
    MultiArg *a  = (MultiArg *)arg;
    int my_index = atomic_fetch_add_explicit(&a->ready_count, 1, memory_order_acq_rel);

    while (atomic_load_explicit(&a->ready_count, memory_order_acquire) < a->total_threads)
        ;

    uint64_t start = get_nanos();
    for (int i = 0; i < a->iters; i++) {
        PyMutex_Lock(a->py);
        atomic_fetch_add_explicit(a->counter, 1, memory_order_relaxed);
        PyMutex_Unlock(a->py);
    }
    a->elapsed_ns[my_index] = get_nanos() - start;
    return nullptr;
}

// Returns the slowest thread's elapsed time — the true wall latency
// at which MULTI_ITERATIONS contended acquisitions completed.
static uint64_t max_elapsed(const uint64_t *elapsed_ns, int n) {
    uint64_t m = 0;
    for (int i = 0; i < n; i++)
        if (elapsed_ns[i] > m)
            m = elapsed_ns[i];
    return m;
}

// --- Execution Runners ---

static void profile_single_thread(void) {
    printf("--- Single Thread (Uncontended) ---\n");

    // 1. MagMutex
    {
        MagMutex m                 = {.bits = MAG_UNLOCKED};
        volatile long long counter = 0;
        uint64_t start             = get_nanos();
        for (int i = 0; i < SINGLE_ITERATIONS; i++) {
            MagMutex_Lock(&m);
            counter++;
            MagMutex_Unlock(&m);
        }
        printf("MagMutex:      %6.2f ns/op\n", (double)(get_nanos() - start) / SINGLE_ITERATIONS);
    }

    // 2. pthread_mutex
    {
        pthread_mutex_t m;
        pthread_mutex_init(&m, nullptr);
        volatile long long counter = 0;
        uint64_t start             = get_nanos();
        for (int i = 0; i < SINGLE_ITERATIONS; i++) {
            pthread_mutex_lock(&m);
            counter++;
            pthread_mutex_unlock(&m);
        }
        printf("pthread_mutex: %6.2f ns/op\n", (double)(get_nanos() - start) / SINGLE_ITERATIONS);
        pthread_mutex_destroy(&m);
    }

    // 3. PyMutex
    {
        PyMutex m                  = {0};
        volatile long long counter = 0;
        uint64_t start             = get_nanos();
        for (int i = 0; i < SINGLE_ITERATIONS; i++) {
            PyMutex_Lock(&m);
            counter++;
            PyMutex_Unlock(&m);
        }
        printf("PyMutex (3.14):%6.2f ns/op\n", (double)(get_nanos() - start) / SINGLE_ITERATIONS);
    }

    // 4. Naive single-threaded code
    {
        volatile long long counter = 0;
        uint64_t start             = get_nanos();
        for (auto i = 0; i < SINGLE_ITERATIONS; i++) {
            counter++;
        }
        printf("Naive:         %6.2f ns/op\n\n", (double)(get_nanos() - start) / SINGLE_ITERATIONS);
    }
}

static void profile_multi_thread(int threads) {
    pthread_t pts[64];
    uint64_t elapsed[64];

    // Metric reported: ns per lock acquisition from the perspective of the
    // *slowest* thread. This is the correct contended latency — it reflects
    // how long a thread actually spent waiting to get MULTI_ITERATIONS locks,
    // not a throughput-divided-by-parallelism illusion.
    printf("--- Multi-Threaded (%d threads, %d ops/thread) ---\n", threads, MULTI_ITERATIONS);

    // 1. MagMutex
    {
        MagMutex mag            = {.bits = MAG_UNLOCKED};
        _Atomic long long count = 0;
        MultiArg arg            = {
            .mag           = &mag,
            .iters         = MULTI_ITERATIONS,
            .counter       = &count,
            .ready_count   = 0,
            .total_threads = threads,
            .elapsed_ns    = elapsed,
        };
        for (int i = 0; i < threads; i++)
            pthread_create(&pts[i], nullptr, multi_mag_worker, &arg);
        for (int i = 0; i < threads; i++)
            pthread_join(pts[i], nullptr);
        printf("MagMutex:      %6.2f ns/op\n",
               (double)max_elapsed(elapsed, threads) / MULTI_ITERATIONS);
    }

    // 2. pthread_mutex
    {
        pthread_mutex_t pth;
        pthread_mutex_init(&pth, nullptr);
        _Atomic long long count = 0;
        MultiArg arg            = {
            .pth           = &pth,
            .iters         = MULTI_ITERATIONS,
            .counter       = &count,
            .ready_count   = 0,
            .total_threads = threads,
            .elapsed_ns    = elapsed,
        };
        for (int i = 0; i < threads; i++)
            pthread_create(&pts[i], nullptr, multi_pth_worker, &arg);
        for (int i = 0; i < threads; i++)
            pthread_join(pts[i], nullptr);
        printf("pthread_mutex: %6.2f ns/op\n",
               (double)max_elapsed(elapsed, threads) / MULTI_ITERATIONS);
        pthread_mutex_destroy(&pth);
    }

    // 3. PyMutex
    {
        PyMutex py              = {0};
        _Atomic long long count = 0;
        MultiArg arg            = {
            .py            = &py,
            .iters         = MULTI_ITERATIONS,
            .counter       = &count,
            .ready_count   = 0,
            .total_threads = threads,
            .elapsed_ns    = elapsed,
        };
        for (int i = 0; i < threads; i++)
            pthread_create(&pts[i], nullptr, multi_py_worker, &arg);
        for (int i = 0; i < threads; i++)
            pthread_join(pts[i], nullptr);
        printf("PyMutex (3.14):%6.2f ns/op\n\n",
               (double)max_elapsed(elapsed, threads) / MULTI_ITERATIONS);
    }
}

int main(void) {
    Py_Initialize();

    printf("=== Mutex Triple-Threat Profile (Mag vs PTH vs PY) ===\n\n");

    profile_single_thread();
    profile_multi_thread(2);
    profile_multi_thread(4);
    profile_multi_thread(8);
    profile_multi_thread(16);

    Py_Finalize();
    return 0;
}