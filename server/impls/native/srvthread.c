// Per-thread entry state - see srvthread.h for what each of these is for and
// the INVARIANT the cancellation setting carries.
#define _GNU_SOURCE
#include "srvthread.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

void thread_disable_cancellation(void) {
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
}

static _Thread_local unsigned int t_rand_seed;
static _Thread_local bool t_rand_seeded = false;

unsigned int next_rand(void) {
    if (!t_rand_seeded) {
        t_rand_seed = (unsigned int)((uintptr_t)pthread_self() ^ (uintptr_t)time(NULL) ^ (uintptr_t)getpid());
        t_rand_seeded = true;
    }
    return (unsigned int)rand_r(&t_rand_seed);
}
