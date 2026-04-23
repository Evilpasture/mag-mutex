#include "mag_mutex.h"
#include "mag_thread.h"
#include "test_utils.h"
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>

// 100,000 tasks is a good stress test for scheduling overhead
#define NUM_TASKS 100000

// --- The Work ---
// We do a tiny bit of math to ensure the compiler doesn't erase the loop
[[gnu::noinline]]
static void do_work(int id) {
    volatile int counter = id;
    for (int i = 0; i < 50; i++) {
        counter += i;
    }
}

// --- MagThread Worker ---
void mag_worker_func(void *arg) { do_work((int)(uintptr_t)arg); }

// --- OpenMP Benchmark ---
void bench_openmp(void) {
    // OpenMP usually defaults to the number of logical cores
    int max_threads = omp_get_max_threads();

    uint64_t start = get_nanos();

#pragma omp parallel for
    for (int i = 0; i < NUM_TASKS; i++) {
        do_work(i);
    }

    uint64_t end = get_nanos();
    printf("OpenMP,%d,%llu\n", max_threads, end - start);
}

// --- MagThread Benchmark ---
void bench_mag(void) {
    MagThread_InitMain();

    // Pre-allocate the fiber array to keep the timing focused on execution
    MagThread **fibers = malloc(sizeof(MagThread *) * NUM_TASKS);

    // Creation Phase (includes mmap/mprotect)
    for (int i = 0; i < NUM_TASKS; i++) {
        fibers[i] = MagThread_Create(0, mag_worker_func, (void *)(uintptr_t)i);
    }

    uint64_t start = get_nanos();

    // Execution Phase (Round-robin scheduler)
    bool active = true;
    while (active) {
        active = false;
        for (int i = 0; i < NUM_TASKS; i++) {
            if (!fibers[i]->is_finished) {
                active = true;
                MagThread_Resume(fibers[i]);
            }
        }
    }

    uint64_t end = get_nanos();
    printf("MagThread,1,%llu\n", end - start);

    // Cleanup
    for (int i = 0; i < NUM_TASKS; i++) {
        MagThread_Destroy(fibers[i]);
    }
    free(fibers);
}

int main() {
    // CSV Header
    printf("system,threads,total_time_ns\n");

    // Run MagThread (Concurrency on 1 Core)
    bench_mag();

    // Run OpenMP (Parallelism on N Cores)
    bench_openmp();

    return 0;
}