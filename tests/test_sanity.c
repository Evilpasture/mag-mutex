#include "mag_mutex.h"
#include "mag_thread.h"
#include <assert.h>
#include <stdio.h>

MagMutex global_lock = {0};
int shared_resource  = 0;

void worker_fiber(void *arg) {
    int id = *(int *)arg;
    printf("[Fiber %d] START\n", id); fflush(stdout);

    printf("[Fiber %d] calling Lock...\n", id); fflush(stdout);
    MagMutex_Lock(&global_lock);
    printf("[Fiber %d] Lock acquired\n", id); fflush(stdout);

    int temp = shared_resource;
    printf("[Fiber %d] read shared_resource=%d, about to Yield\n", id, temp); fflush(stdout);
    MagThread_Yield();
    printf("[Fiber %d] resumed after Yield\n", id); fflush(stdout);

    shared_resource = temp + 1;
    printf("[Fiber %d] wrote shared_resource=%d, calling Unlock\n", id, shared_resource); fflush(stdout);

    MagMutex_Unlock(&global_lock);
    printf("[Fiber %d] Unlock done, returning\n", id); fflush(stdout);
}

int main(void) {
    printf("[Main] Initializing...\n"); fflush(stdout);
    MagThread_InitMain();

    int ids[] = {1, 2, 3};
    MagThread *fibers[3];
    for (int i = 0; i < 3; i++) {
        fibers[i] = MagThread_Create(0, worker_fiber, &ids[i]);
        printf("[Main] Created fiber %d: %p\n", ids[i], (void*)fibers[i]); fflush(stdout);
    }

    printf("[Main] Entering scheduler loop\n"); fflush(stdout);
    bool active = true;
    int tick = 0;
    while (active) {
        active = false;
        for (int i = 0; i < 3; i++) {
            if (!fibers[i]->is_finished) {
                active = true;
                printf("[Sched tick=%d] Resuming fiber %d\n", tick++, ids[i]); fflush(stdout);
                MagThread_Resume(fibers[i]);
                printf("[Sched tick=%d] Returned from fiber %d\n", tick-1, ids[i]); fflush(stdout);
            }
        }
    }

    for (int i = 0; i < 3; i++)
        MagThread_Destroy(fibers[i]);

    printf("[Main] shared_resource = %d (expected 3)\n", shared_resource); fflush(stdout);
    assert(shared_resource == 3);
    printf("[Sanity] PASS\n");
    return 0;
}