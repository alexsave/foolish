// thread_heap.h - a heap block that belongs to the thread that allocated it.
//
// The solvers keep their big scratch in a _Thread_local pointer filled on first
// use (cordite_sim.c's 1 MB transposition table, astrolite_strategy.c's child
// states and move lists), so every thread searches without a lock. The pointer
// dies with its thread; a plain calloc'd block does not. A server's worker
// threads end when its pool stops, so each worker that ever ran an endgame solve
// leaked its block: c_base's ASan smoke caught octogen's table on a bot worker
// at the SIGTERM drain, as a LeakSanitizer exit status the smoke read as "the
// server did not stop cleanly".
//
// thread_heap_calloc ties the block to the calling thread: a pthread key
// destructor frees every block a thread took when that thread exits. The caller
// keeps its _Thread_local pointer exactly as before and never frees the block.
//
// Header-only (static inline), so none of the build systems that compile the
// kernel has to learn a new .c. Each file that includes it gets its own key,
// which is a key per file, not per block. wasm has no threads (c/Makefile strips
// _Thread_local there) and its module memory goes with the instance, so there
// it is a plain calloc.
#ifndef THREAD_HEAP_H
#define THREAD_HEAP_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#if defined(__wasm__)

static inline void *thread_heap_calloc(size_t n, size_t size) { return calloc(n, size); }

#else

#include <pthread.h>

// Each block is preceded by the link to the thread's previous block; the key
// holds the newest. Aligned for anything, so the caller's block is too.
typedef struct thread_heap_hdr {
    _Alignas(max_align_t) struct thread_heap_hdr *next;
} thread_heap_hdr;

static pthread_key_t  thread_heap_key;
static pthread_once_t thread_heap_once = PTHREAD_ONCE_INIT;
static int            thread_heap_key_ok;

// The key's destructor: runs on the exiting thread with its newest block.
static inline void thread_heap_release(void *newest) {
    for (thread_heap_hdr *h = newest; h;) {
        thread_heap_hdr *next = h->next;
        free(h);
        h = next;
    }
}

static inline void thread_heap_make_key(void) {
    thread_heap_key_ok = pthread_key_create(&thread_heap_key, thread_heap_release) == 0;
}

// calloc(n, size) owned by the calling thread and freed when it exits. NULL
// when out of memory (or out of pthread keys), as calloc.
static inline void *thread_heap_calloc(size_t n, size_t size) {
    if (size != 0 && n > (SIZE_MAX - sizeof(thread_heap_hdr)) / size) return NULL;
    pthread_once(&thread_heap_once, thread_heap_make_key);
    if (!thread_heap_key_ok) return NULL;
    thread_heap_hdr *h = calloc(1, sizeof *h + n * size);
    if (!h) return NULL;
    h->next = pthread_getspecific(thread_heap_key);
    if (pthread_setspecific(thread_heap_key, h) != 0) {
        free(h);
        return NULL;
    }
    return h + 1;
}

#endif

#endif
