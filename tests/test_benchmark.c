#include "mag_mutex.h"
#include "mag_thread.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef __APPLE__
#    include <mach/mach_time.h>
#endif

// --- High Precision Timing ---
static inline uint64_t ns_now(void) {
#ifdef __APPLE__
    static mach_timebase_info_data_t info;
    if (info.denom == 0)
        mach_timebase_info(&info);
    return mach_absolute_time() * info.numer / info.denom;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
#endif
}

// --- Configuration ---
#define ITERATIONS 1000000
#define NUM_ENTITIES 10 // Number of threads/fibers fighting

// ============================================================================
// 1. FAST PATH BENCHMARK (No Contention)
// ============================================================================
void bench_fast_path(void) {
    printf("--- Fast Path (Lock + Unlock) ---\n");

    // MagMutex
    MagMutex mag_l = {0};
    uint64_t start = ns_now();
    for (int i = 0; i < ITERATIONS; i++) {
        MagMutex_Lock(&mag_l);
        MagMutex_Unlock(&mag_l);
    }
    uint64_t mag_time = ns_now() - start;
    printf("MagMutex: %llu ns total (%llu ns/op)\n", mag_time, mag_time / ITERATIONS);

    // Pthread Mutex
    pthread_mutex_t posix_l;
    pthread_mutex_init(&posix_l, NULL);
    start = ns_now();
    for (int i = 0; i < ITERATIONS; i++) {
        pthread_mutex_lock(&posix_l);
        pthread_mutex_unlock(&posix_l);
    }
    uint64_t posix_time = ns_now() - start;
    printf("Pthread:  %llu ns total (%llu ns/op)\n", posix_time, posix_time / ITERATIONS);
    pthread_mutex_destroy(&posix_l);
    printf("\n");
}

// ============================================================================
// 2. PING-PONG BENCHMARK (CondVar Signaling Latency)
// ============================================================================

// --- Mag Version ---
MagMutex mag_m = {0};
MagCond mag_c  = {0};
int mag_turn   = 0;

void mag_ping_pong(void *arg) {
    for (int i = 0; i < ITERATIONS / 10; i++) {
        MagMutex_Lock(&mag_m);
        while (mag_turn != (uintptr_t)arg) {
            MagCond_Wait(&mag_c, &mag_m);
        }
        mag_turn = !mag_turn;
        MagCond_Signal(&mag_c);
        MagMutex_Unlock(&mag_m);
    }
}

// --- POSIX Version ---
pthread_mutex_t posix_m = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t posix_c  = PTHREAD_COND_INITIALIZER;
int posix_turn          = 0;

void *posix_ping_pong(void *arg) {
    for (int i = 0; i < ITERATIONS / 10; i++) {
        pthread_mutex_lock(&posix_m);
        while (posix_turn != (uintptr_t)arg) {
            pthread_cond_wait(&posix_c, &posix_m);
        }
        posix_turn = !posix_turn;
        pthread_cond_signal(&posix_c);
        pthread_mutex_unlock(&posix_m);
    }
    return NULL;
}

void bench_ping_pong(void) {
    printf("--- Ping-Pong Latency (Context Switch + CondVar) ---\n");

    // MagThreads
    MagThread_InitMain();
    MagThread *f1 = MagThread_Create(0, mag_ping_pong, (void *)0);
    MagThread *f2 = MagThread_Create(0, mag_ping_pong, (void *)1);

    uint64_t start = ns_now();
    while (!f1->is_finished || !f2->is_finished) {
        if (!f1->is_finished)
            MagThread_Resume(f1);
        if (!f2->is_finished)
            MagThread_Resume(f2);
    }
    uint64_t mag_time = ns_now() - start;
    printf("Mag (Fibers):  %llu ns total (%llu ns/switch)\n", mag_time,
           mag_time / (ITERATIONS / 5));

    // Pthreads
    pthread_t t1, t2;
    start = ns_now();
    pthread_create(&t1, NULL, posix_ping_pong, (void *)0);
    pthread_create(&t2, NULL, posix_ping_pong, (void *)1);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    uint64_t posix_time = ns_now() - start;
    printf("POSIX (Threads): %llu ns total (%llu ns/switch)\n", posix_time,
           posix_time / (ITERATIONS / 5));
    printf("\n");
}

// ============================================================================
// 3. MASSIVE CONTENTION (The Parking Lot Stress Test)
// ============================================================================
MagMutex stress_mag_l = {0};
int stress_counter    = 0;

void mag_stress_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 1000; i++) {
        MagMutex_Lock(&stress_mag_l);
        stress_counter++;
        MagThread_Yield(); // Force contention
        MagMutex_Unlock(&stress_mag_l);
    }
}

void bench_contention(void) {
    printf("--- Massive Contention (100 Entities Fighting) ---\n");
    const int count = 100;
    MagThread *fibers[count];
    MagThread_InitMain();

    for (int i = 0; i < count; i++)
        fibers[i] = MagThread_Create(0, mag_stress_worker, NULL);

    uint64_t start = ns_now();
    bool alive     = true;
    while (alive) {
        alive = false;
        for (int i = 0; i < count; i++) {
            if (!fibers[i]->is_finished) {
                alive = true;
                MagThread_Resume(fibers[i]);
            }
        }
    }
    uint64_t mag_time = ns_now() - start;
    printf("Mag (1-Byte + Fiber): %llu ns total\n", mag_time);

    // Note: Benchmarking 100 Pthreads here would mostly measure the OS Kernel
    // Scheduler's ability to handle starvation, not our lock's performance.
    // But MagSync allows this many "threads" with almost zero cost.
}

int main() {
    bench_fast_path();
    bench_ping_pong();
    bench_contention();
    return 0;
}