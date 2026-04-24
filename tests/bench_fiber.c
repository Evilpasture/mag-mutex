#include "mag_mutex.h"
#include "mag_thread.h"
#include "test_utils.h"

static constexpr int ITERATIONS = 1000000;

// --- Mag ---
MagMutex mag_m     = {0};
MagCond mag_c      = {0};
uintptr_t mag_turn = 0;

void mag_ping(void *arg) {
    for (int i = 0; i < ITERATIONS; i++) {
        MagMutex_Lock(&mag_m);
        while (mag_turn != (uintptr_t)arg)
            MagCond_Wait(&mag_c, &mag_m);
        mag_turn = !mag_turn;
        MagCond_Signal(&mag_c);
        MagMutex_Unlock(&mag_m);
    }
}

// --- OS ---
os_mutex_t os_m;
os_cond_t os_c;
uintptr_t os_turn = 0;

void *os_ping(void *arg) {
    for (int i = 0; i < ITERATIONS; i++) {
        os_mutex_lock(&os_m);
        while (os_turn != (uintptr_t)arg)
            os_cond_wait(&os_c, &os_m);
        os_turn = !os_turn;
        os_cond_signal(&os_c);
        os_mutex_unlock(&os_m);
    }
    return NULL;
}

int main(void) {
    printf("benchmark,system,metric,value_ns\n");

    // 1. Fiber Ping-Pong
    MagThread_InitMain();
    MagThread *f1 = MagThread_Create(0, mag_ping, (void *)0);
    MagThread *f2 = MagThread_Create(0, mag_ping, (void *)1);

    uint64_t start = get_nanos();
    while (!f1->is_finished || !f2->is_finished) {
        if (!f1->is_finished)
            MagThread_Resume(f1);
        if (!f2->is_finished)
            MagThread_Resume(f2);
    }
    uint64_t fiber_time = get_nanos() - start;
    printf("ping_pong,MagFiber,ns_per_switch,%llu\n", fiber_time / (ITERATIONS * 2));
    MagThread_Destroy(f1);
    MagThread_Destroy(f2);

    // 2. OS Ping-Pong
    os_mutex_init(&os_m);
    os_cond_init(&os_c);
    os_thread_t t1, t2;
    start = get_nanos();
    os_thread_create(&t1, os_ping, (void *)0);
    os_thread_create(&t2, os_ping, (void *)1);
    os_thread_join(t1);
    os_thread_join(t2);
    uint64_t os_time = get_nanos() - start;
    printf("ping_pong,OSThread,ns_per_switch,%llu\n", os_time / (ITERATIONS * 2));

    return 0;
}