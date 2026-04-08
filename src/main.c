#define PY_SSIZE_T_CLEAN
#include "mag_mutex.h"
#include <Python.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// Configurations for deep analysis
static constexpr int MULTI_ITERATIONS = 1000000;
static constexpr int NUM_TRIALS       = 5; // Run each config 5 times for variance
static int THREAD_CONFIGS[]           = {1, 2, 4, 8, 12, 16};
static int NUM_CONFIGS                = 6;

typedef struct {
    void *mutex_ptr;
    int type; // 0: Mag, 1: Pth, 2: Py
    int iters;
    _Atomic long long *counter;
    _Atomic int ready_count;
    int total_threads;
    uint64_t *elapsed_ns;
} MultiArg;

static inline uint64_t get_nanos(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void *worker_func(void *arg) {
    MultiArg *a  = (MultiArg *)arg;
    int my_index = atomic_fetch_add_explicit(&a->ready_count, 1, memory_order_acq_rel);

    while (atomic_load_explicit(&a->ready_count, memory_order_acquire) < a->total_threads)
        Mag_CPURelax();

    uint64_t start = get_nanos();

    if (a->type == 0) { // MagMutex
        MagMutex *m = (MagMutex *)a->mutex_ptr;
        for (int i = 0; i < a->iters; i++) {
            MagMutex_Lock(m);
            atomic_fetch_add_explicit(a->counter, 1, memory_order_relaxed);
            MagMutex_Unlock(m);
        }
    } else if (a->type == 1) { // pthread_mutex
        pthread_mutex_t *m = (pthread_mutex_t *)a->mutex_ptr;
        for (int i = 0; i < a->iters; i++) {
            pthread_mutex_lock(m);
            atomic_fetch_add_explicit(a->counter, 1, memory_order_relaxed);
            pthread_mutex_unlock(m);
        }
    } else { // PyMutex
        PyMutex *m = (PyMutex *)a->mutex_ptr;
        for (int i = 0; i < a->iters; i++) {
            PyMutex_Lock(m);
            atomic_fetch_add_explicit(a->counter, 1, memory_order_relaxed);
            PyMutex_Unlock(m);
        }
    }

    a->elapsed_ns[my_index] = get_nanos() - start;
    return nullptr;
}

static void run_suite(int type, int threads, int trial_id) {
    pthread_t pts[64];
    uint64_t elapsed[64];
    _Atomic long long count = 0;
    const char *labels[]    = {"MagMutex", "pthread_mutex", "PyMutex"};

    void *mtx;
    if (type == 0) {
        mtx = malloc(sizeof(MagMutex));
        memset(mtx, 0, sizeof(MagMutex));
    } else if (type == 1) {
        mtx = malloc(sizeof(pthread_mutex_t));
        pthread_mutex_init((pthread_mutex_t *)mtx, nullptr);
    } else {
        mtx = malloc(sizeof(PyMutex));
        memset(mtx, 0, sizeof(PyMutex));
    }

    MultiArg arg = {
        .mutex_ptr     = mtx,
        .type          = type,
        .iters         = MULTI_ITERATIONS,
        .counter       = &count,
        .ready_count   = 0,
        .total_threads = threads,
        .elapsed_ns    = elapsed,
    };

    for (int i = 0; i < threads; i++)
        pthread_create(&pts[i], nullptr, worker_func, &arg);
    for (int i = 0; i < threads; i++)
        pthread_join(pts[i], nullptr);

    // Output CSV rows: mutex,threads,trial,thread_id,ns_per_op
    for (int i = 0; i < threads; i++) {
        printf("%s,%d,%d,%d,%.2f\n", labels[type], threads, trial_id, i,
               (double)elapsed[i] / MULTI_ITERATIONS);
    }

    if (type == 1)
        pthread_mutex_destroy((pthread_mutex_t *)mtx);
    free(mtx);
}

int main(void) {
    Py_Initialize();

    // CSV Header
    printf("mutex,threads,trial,thread_id,latency_ns\n");

    for (int c = 0; c < NUM_CONFIGS; c++) {
        int threads = THREAD_CONFIGS[c];
        for (int t = 0; t < NUM_TRIALS; t++) {
            run_suite(0, threads, t); // Mag
            run_suite(1, threads, t); // Pthread
            run_suite(2, threads, t); // PyMutex
        }
    }

    Py_Finalize();
    return 0;
}