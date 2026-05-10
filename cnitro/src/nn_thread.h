// Tiny persistent pthread pool. Spawn workers once at startup; submit
// "split this range across threads" tasks for each parallel region.
//
// Designed for cnitro_train: low-latency synchronous parallel-for, no work
// stealing, no dynamic dispatch. Each call partitions [0, total) into
// num_workers contiguous chunks; workers run their chunk and the caller
// blocks until all are done.
#ifndef CNITRO_NN_THREAD_H
#define CNITRO_NN_THREAD_H

void nn_thread_init(int n);    // n<=0 → autodetect cores; capped at MAX_THREADS
void nn_thread_shutdown(void);
int  nn_thread_count(void);

// Worker function signature. tid is the worker index in [0, nn_thread_count()),
// useful for indexing per-thread scratch (caches, grad buffers).
typedef void (*WorkFn)(int start, int end, int tid, void *ctx);

// Synchronously run fn over a partition of [0, total). Falls back to inline
// execution when there is too little work to thread profitably.
void parallel_for(int total, WorkFn fn, void *ctx);

#endif
