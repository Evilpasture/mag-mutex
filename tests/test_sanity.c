#include "mag_mutex.h"
#include "mag_thread.h"
#include <assert.h>
#include <stdio.h>

MagMutex global_lock = {0};
int shared_resource  = 0;

void worker_fiber(void *arg) {
    int id = *(int *)arg;
    MagMutex_Lock(&global_lock);

    int temp = shared_resource;
    MagThread_Yield(); // Yield while holding the lock!
    shared_resource = temp + 1;

    MagMutex_Unlock(&global_lock);
}

int main(void) {
    printf("[Sanity] Initializing Main Thread...\n");
    MagThread_InitMain();

    int ids[] = {1, 2, 3};
    MagThread *fibers[3];
    for (int i = 0; i < 3; i++)
        fibers[i] = MagThread_Create(0, worker_fiber, &ids[i]);

    printf("[Sanity] Running Cooperative Scheduler...\n");
    bool active = true;
    while (active) {
        active = false;
        for (int i = 0; i < 3; i++) {
            if (!fibers[i]->is_finished) {
                active = true;
                MagThread_Resume(fibers[i]);
            }
        }
    }

    for (int i = 0; i < 3; i++)
        MagThread_Destroy(fibers[i]);

    assert(shared_resource == 3);
    printf("[Sanity] PASS: Shared resource perfectly protected across fiber yields.\n");
    return 0;
}