#include "mag_mutex.h"
#include "test_utils.h"
#include <stdatomic.h>

static constexpr int ITERATIONS = 1000000;
static constexpr int NUM_TRIALS = 5;
static const int THREAD_CFGS[]  = {1, 2, 4, 8, 12, 16};

typedef struct {
    void *mtx;
    int type; // 0=Mag, 1=OS
    _Atomic int ready_count;
    int total_threads;
    uint64_t *elapsed;
} SharedArg;

static void *worker(void *arg) {
    SharedArg *a = (SharedArg *)arg;
    int id       = atomic_fetch_add_explicit(&a->ready_count, 1, memory_order_acq_rel);

    while (atomic_load_explicit(&a->ready_count, memory_order_acquire) < a->total_threads)
        Mag_CPURelax();

    uint64_t start = get_nanos();
    if (a->type == 0) {
        MagMutex *m = (MagMutex *)a->mtx;
        for (int i = 0; i < ITERATIONS; i++) {
            MagMutex_Lock(m);
            MagMutex_Unlock(m);
        }
    } else {
        os_mutex_t *m = (os_mutex_t *)a->mtx;
        for (int i = 0; i < ITERATIONS; i++) {
            os_mutex_lock(m);
            os_mutex_unlock(m);
        }
    }
    a->elapsed[id] = get_nanos() - start;
    return NULL;
}

int main(void) {
    printf("mutex,threads,trial,latency_ns_per_op\n"); // CSV Header

    for (int t_idx = 0; t_idx < (int)(sizeof(THREAD_CFGS) / sizeof(int)); t_idx++) {
        int threads = THREAD_CFGS[t_idx];
        for (int trial = 0; trial < NUM_TRIALS; trial++) {
            for (int type = 0; type < 2; type++) {
                os_thread_t pts[64];
                uint64_t elapsed[64] = {0};

                void *mtx = malloc(type == 0 ? sizeof(MagMutex) : sizeof(os_mutex_t));
                if (type == 0)
                    atomic_init(&((MagMutex *)mtx)->bits, MAG_UNLOCKED);
                else
                    os_mutex_init((os_mutex_t *)mtx);

                SharedArg arg = {.mtx           = mtx,
                                 .type          = type,
                                 .ready_count   = 0,
                                 .total_threads = threads,
                                 .elapsed       = elapsed};

                for (int i = 0; i < threads; i++)
                    os_thread_create(&pts[i], worker, &arg);
                for (int i = 0; i < threads; i++)
                    os_thread_join(pts[i]);

                uint64_t max_ns = 0;
                for (int i = 0; i < threads; i++)
                    if (elapsed[i] > max_ns)
                        max_ns = elapsed[i];

                printf("%s,%d,%d,%.2f\n", type == 0 ? "MagMutex" : "OSMutex", threads, trial,
                       (double)max_ns / ITERATIONS);

                if (type == 1)
                    os_mutex_destroy((os_mutex_t *)mtx);
                free(mtx);
            }
        }
    }
    return 0;
}