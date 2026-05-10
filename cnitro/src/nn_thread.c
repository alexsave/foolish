#include "nn_thread.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_THREADS 16

// Work-stealing pool. All workers share a single atomic "next iteration"
// counter; whoever finishes its current chunk grabs the next one. No
// per-worker dispatch — master broadcasts once, workers self-distribute.
// Eliminates the N mutex/signal pairs per parallel_for call and makes
// straggler workers irrelevant (others steal the remaining iterations).
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  go;          // broadcast: new work available
    pthread_cond_t  done;        // last worker to finish signals this
    WorkFn          fn;
    void           *ctx;
    int             total;
    _Atomic int     next_idx;    // claim a job by atomic_fetch_add
    _Atomic int     n_active;    // workers that haven't yet returned
    _Atomic int     epoch;       // bumped by master; workers wait for new epoch
    int             shutdown;
    int             n_workers;
} Pool;

static Pool      g_pool;
static pthread_t g_threads[MAX_THREADS];
static int       g_tids[MAX_THREADS];

static void run_chunks(int tid) {
    // Pull iterations from the shared counter until exhausted.
    for (;;) {
        int idx = atomic_fetch_add_explicit(&g_pool.next_idx, 1, memory_order_relaxed);
        if (idx >= g_pool.total) break;
        g_pool.fn(idx, idx + 1, tid, g_pool.ctx);
    }
}

static void *worker_loop(void *arg) {
    int tid = *(int *)arg;
    int my_epoch = 0;
    for (;;) {
        pthread_mutex_lock(&g_pool.lock);
        while (atomic_load_explicit(&g_pool.epoch, memory_order_acquire) == my_epoch
               && !g_pool.shutdown) {
            pthread_cond_wait(&g_pool.go, &g_pool.lock);
        }
        if (g_pool.shutdown) { pthread_mutex_unlock(&g_pool.lock); break; }
        my_epoch = atomic_load_explicit(&g_pool.epoch, memory_order_acquire);
        pthread_mutex_unlock(&g_pool.lock);

        run_chunks(tid);

        // If this is the last worker to finish, signal master.
        if (atomic_fetch_sub_explicit(&g_pool.n_active, 1, memory_order_acq_rel) == 1) {
            pthread_mutex_lock(&g_pool.lock);
            pthread_cond_signal(&g_pool.done);
            pthread_mutex_unlock(&g_pool.lock);
        }
    }
    return NULL;
}

void nn_thread_init(int n) {
    if (g_pool.n_workers > 0) return;
    if (n <= 0) {
        long nc = sysconf(_SC_NPROCESSORS_ONLN);
        n = nc > 0 ? (int)nc : 4;
    }
    if (n > MAX_THREADS) n = MAX_THREADS;
    memset(&g_pool, 0, sizeof(g_pool));
    pthread_mutex_init(&g_pool.lock, NULL);
    pthread_cond_init(&g_pool.go,    NULL);
    pthread_cond_init(&g_pool.done,  NULL);
    atomic_store_explicit(&g_pool.epoch,    0, memory_order_relaxed);
    atomic_store_explicit(&g_pool.next_idx, 0, memory_order_relaxed);
    atomic_store_explicit(&g_pool.n_active, 0, memory_order_relaxed);
    g_pool.shutdown  = 0;
    g_pool.n_workers = n;
    // Workers use tids 1..n; tid 0 is the master (so master can pitch in
    // using the worker-0-style scratch slot without colliding).
    for (int i = 0; i < n; i++) {
        g_tids[i] = i + 1;
        pthread_create(&g_threads[i], NULL, worker_loop, &g_tids[i]);
    }
}

void nn_thread_shutdown(void) {
    if (g_pool.n_workers == 0) return;
    pthread_mutex_lock(&g_pool.lock);
    g_pool.shutdown = 1;
    pthread_cond_broadcast(&g_pool.go);
    pthread_mutex_unlock(&g_pool.lock);
    for (int i = 0; i < g_pool.n_workers; i++) pthread_join(g_threads[i], NULL);
    pthread_mutex_destroy(&g_pool.lock);
    pthread_cond_destroy(&g_pool.go);
    pthread_cond_destroy(&g_pool.done);
    g_pool.n_workers = 0;
}

// nn_thread_count returns the size of the SCRATCH array the caller should
// allocate (master + n workers), since the master also pitches in via tid 0.
int nn_thread_count(void) { return g_pool.n_workers > 0 ? g_pool.n_workers + 1 : 1; }

void parallel_for(int total, WorkFn fn, void *ctx) {
    if (g_pool.n_workers == 0 || total <= 0) {
        if (total > 0) fn(0, total, 0, ctx);
        return;
    }

    // Publish task and wake workers.
    pthread_mutex_lock(&g_pool.lock);
    g_pool.fn    = fn;
    g_pool.ctx   = ctx;
    g_pool.total = total;
    atomic_store_explicit(&g_pool.next_idx, 0, memory_order_relaxed);
    atomic_store_explicit(&g_pool.n_active, g_pool.n_workers, memory_order_release);
    atomic_fetch_add_explicit(&g_pool.epoch, 1, memory_order_release);
    pthread_cond_broadcast(&g_pool.go);
    pthread_mutex_unlock(&g_pool.lock);

    // Master pitches in: pull from the same atomic counter as the workers.
    // Master uses tid=0 (a dedicated scratch slot the workers never touch).
    run_chunks(0);

    // Wait for the last worker to drain its iteration and signal done.
    pthread_mutex_lock(&g_pool.lock);
    while (atomic_load_explicit(&g_pool.n_active, memory_order_acquire) > 0) {
        pthread_cond_wait(&g_pool.done, &g_pool.lock);
    }
    pthread_mutex_unlock(&g_pool.lock);
}
