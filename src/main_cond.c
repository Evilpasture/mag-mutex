#define PY_SSIZE_T_CLEAN
#include "mag_mutex.h"
#include <Python.h> // Kept for CMake compatibility
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// --- Configurations for Deep Analysis ---
static constexpr int TOTAL_TASKS = 1000000;
static constexpr int NUM_TRIALS  = 5; // Run each config 5 times for variance
static int THREAD_CONFIGS[]      = {2, 4, 8, 12, 16}; // Min 2 (1 Prod, 1 Cons)
static int NUM_CONFIGS           = 5;

// --- Timing Helpers ---
static inline uint64_t get_nanos(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// --- Benchmark Payload ---
typedef struct {
    int type; // 0: Mag, 1: Pthread
    void *mtx;
    void *cv;
    
    int total_tasks;
    int queue_count;
    bool done;

    _Atomic int ready_count;
    int total_threads;
    uint64_t *elapsed_ns;
    int *items_consumed;
} MultiArg;

typedef struct {
    MultiArg *shared;
    int id;
} ThreadArg;

static void *worker_func(void *arg) {
    ThreadArg *targ = (ThreadArg *)arg;
    MultiArg *a     = targ->shared;
    int id          = targ->id;

    // Barrier for synchronized start
    atomic_fetch_add_explicit(&a->ready_count, 1, memory_order_acq_rel);
    while (atomic_load_explicit(&a->ready_count, memory_order_acquire) < a->total_threads) {
        Mag_CPURelax();
    }

    uint64_t start = get_nanos();
    int consumed   = 0;

    if (id == 0) {
        // ==========================================
        // PRODUCER (Thread 0)
        // ==========================================
        if (a->type == 0) {
            MagMutex *m = (MagMutex *)a->mtx;
            MagCond *cv = (MagCond *)a->cv;
            for (int i = 0; i < a->total_tasks; i++) {
                MagMutex_Lock(m);
                a->queue_count++;
                MagCond_Signal(cv);
                MagMutex_Unlock(m);
                Mag_CPURelax(); // Give consumers a tiny window to grab the lock
            }
            // Signal termination
            MagMutex_Lock(m);
            a->done = true;
            MagCond_Broadcast(cv);
            MagMutex_Unlock(m);
        } else {
            pthread_mutex_t *m = (pthread_mutex_t *)a->mtx;
            pthread_cond_t *cv = (pthread_cond_t *)a->cv;
            for (int i = 0; i < a->total_tasks; i++) {
                pthread_mutex_lock(m);
                a->queue_count++;
                pthread_cond_signal(cv);
                pthread_mutex_unlock(m);
                Mag_CPURelax();
            }
            pthread_mutex_lock(m);
            a->done = true;
            pthread_cond_broadcast(cv);
            pthread_mutex_unlock(m);
        }
        consumed = a->total_tasks; // Record how many it produced
    } else {
        // ==========================================
        // CONSUMERS (Threads 1..N)
        // ==========================================
        if (a->type == 0) {
            MagMutex *m = (MagMutex *)a->mtx;
            MagCond *cv = (MagCond *)a->cv;
            while (true) {
                MagMutex_Lock(m);
                while (a->queue_count == 0 && !a->done) {
                    MagCond_Wait(cv, m);
                }
                if (a->queue_count > 0) {
                    a->queue_count--;
                    consumed++;
                    MagMutex_Unlock(m);
                } else if (a->done) {
                    MagMutex_Unlock(m);
                    break;
                }
            }
        } else {
            pthread_mutex_t *m = (pthread_mutex_t *)a->mtx;
            pthread_cond_t *cv = (pthread_cond_t *)a->cv;
            while (true) {
                pthread_mutex_lock(m);
                while (a->queue_count == 0 && !a->done) {
                    pthread_cond_wait(cv, m);
                }
                if (a->queue_count > 0) {
                    a->queue_count--;
                    consumed++;
                    pthread_mutex_unlock(m);
                } else if (a->done) {
                    pthread_mutex_unlock(m);
                    break;
                }
            }
        }
    }

    a->elapsed_ns[id]     = get_nanos() - start;
    a->items_consumed[id] = consumed;
    return nullptr;
}

static void run_suite(int type, int threads, int trial_id) {
    pthread_t pts[64];
    ThreadArg targs[64];
    uint64_t elapsed[64];
    int items_consumed[64];

    const char *labels[] = {"MagCond", "PthreadCond"};

    void *mtx = NULL;
    void *cv  = NULL;

    if (type == 0) {
        mtx = malloc(sizeof(MagMutex));
        cv  = malloc(sizeof(MagCond));
        atomic_init(&((MagMutex *)mtx)->bits, MAG_UNLOCKED);
        MagCond_Init((MagCond *)cv);
    } else if (type == 1) {
        mtx = malloc(sizeof(pthread_mutex_t));
        cv  = malloc(sizeof(pthread_cond_t));
        pthread_mutex_init((pthread_mutex_t *)mtx, nullptr);
        pthread_cond_init((pthread_cond_t *)cv, nullptr);
    }

    MultiArg arg = {
        .type           = type,
        .mtx            = mtx,
        .cv             = cv,
        .total_tasks    = TOTAL_TASKS,
        .queue_count    = 0,
        .done           = false,
        .ready_count    = 0,
        .total_threads  = threads,
        .elapsed_ns     = elapsed,
        .items_consumed = items_consumed,
    };

    // Spawn Threads
    for (int i = 0; i < threads; i++) {
        targs[i].shared = &arg;
        targs[i].id     = i;
        pthread_create(&pts[i], nullptr, worker_func, &targs[i]);
    }
    
    // Join Threads
    for (int i = 0; i < threads; i++) {
        pthread_join(pts[i], nullptr);
    }

    // Output CSV rows: mutex,threads,trial,role,thread_id,items_processed,latency_ns
    for (int i = 0; i < threads; i++) {
        const char *role = (i == 0) ? "Producer" : "Consumer";
        printf("%s,%d,%d,%s,%d,%d,%llu\n", 
               labels[type], threads, trial_id, role, i, 
               items_consumed[i], (unsigned long long)elapsed[i]);
    }

    // Cleanup
    if (type == 1) {
        pthread_mutex_destroy((pthread_mutex_t *)mtx);
        pthread_cond_destroy((pthread_cond_t *)cv);
    }
    free(mtx);
    free(cv);
}

int main(void) {
    Py_Initialize();

    // Standard CSV Header for R / Pandas parsing
    printf("mutex,threads,trial,role,thread_id,items_processed,latency_ns\n");

    for (int c = 0; c < NUM_CONFIGS; c++) {
        int threads = THREAD_CONFIGS[c];
        for (int t = 0; t < NUM_TRIALS; t++) {
            run_suite(0, threads, t); // MagCond
            run_suite(1, threads, t); // PthreadCond
        }
    }

    Py_Finalize();
    return 0;
}