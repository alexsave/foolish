#ifndef FOOLYARD_PQ_H
#define FOOLYARD_PQ_H

#include "types.h"

// 4-ary min-heap of packed event words, 1-based (slot 0 unused) so the
// parent/child arithmetic stays branch-free.
typedef struct PQ {
    u32  n;      // one past the last entry; n == 1 is empty
    u32  cap;
    u32 *heap;
} PQ;

void pq_init(PQ *pq);
void pq_push(PQ *pq, u32 event);
u32  pq_peek(const PQ *pq);   // undefined if empty
u32  pq_pop(PQ *pq);
void pq_free(PQ *pq);

static inline int pq_empty(const PQ *pq) { return pq->n == 1; }

#endif
