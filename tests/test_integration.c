#include "mag_mutex.h"
#include "mag_thread.h"
#include <stdio.h>

MagMutex global_lock = {0};
int shared_resource  = 0;

void worker_fiber(void *arg) {
    int id = *(int *)arg;
    printf("[Fiber %d] Starting...\n", id);

    // Lock the mutex. If another fiber has it, this will safely MagThread_Yield()
    MagMutex_Lock(&global_lock);
    printf("[Fiber %d] Acquired lock!\n", id);

    // Simulate doing some work while holding the lock
    int temp = shared_resource;
    MagThread_Yield(); // Simulate time passing
    shared_resource = temp + 1;

    printf("[Fiber %d] Releasing lock! Shared Resource = %d\n", id, shared_resource);
    MagMutex_Unlock(&global_lock);
}

int main() {
    MagThread_InitMain();

    int id1 = 1, id2 = 2, id3 = 3;
    MagThread *f1 = MagThread_Create(0, worker_fiber, &id1);
    MagThread *f2 = MagThread_Create(0, worker_fiber, &id2);
    MagThread *f3 = MagThread_Create(0, worker_fiber, &id3);

    MagThread *all_fibers[] = {f1, f2, f3};
    int num_fibers          = 3;

    // --- The Simple "Round-Robin" Scheduler ---
    bool active_fibers = true;
    while (active_fibers) {
        active_fibers = false;

        for (int i = 0; i < num_fibers; i++) {
            if (!all_fibers[i]->is_finished) {
                active_fibers = true;
                MagThread_Resume(all_fibers[i]);
            }
        }
    }

    MagThread_Destroy(f1);
    MagThread_Destroy(f2);
    MagThread_Destroy(f3);

    printf("Main: All fibers finished. Final Resource = %d\n", shared_resource);
    return 0;
}