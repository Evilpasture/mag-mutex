#include "mag_thread.h"
#include <stdio.h>

void my_fiber_job(void *arg) {
    int id = *(int *)arg;
    printf("Fiber %d: Started!\n", id);

    MagThread_Yield(); // Give control back to main

    printf("Fiber %d: Resumed and finishing!\n", id);
}

int main() {
    MagThread_InitMain();

    int arg1 = 1, arg2 = 2;
    MagThread *f1 = MagThread_Create(0, my_fiber_job, &arg1);
    MagThread *f2 = MagThread_Create(0, my_fiber_job, &arg2);

    printf("Main: Resuming f1...\n");
    MagThread_Resume(f1); // f1 runs, then yields

    printf("Main: Resuming f2...\n");
    MagThread_Resume(f2); // f2 runs, then yields

    printf("Main: Resuming f1 again...\n");
    MagThread_Resume(f1); // f1 finishes

    printf("Main: Resuming f2 again...\n");
    MagThread_Resume(f2); // f2 finishes

    MagThread_Destroy(f1);
    MagThread_Destroy(f2);

    printf("Main: Done.\n");
    return 0;
}