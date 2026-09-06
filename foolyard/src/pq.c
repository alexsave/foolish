#include <stdio.h>
#include <stdlib.h>

#include "pq.h"

#define PQ_INITIAL_CAP 256

void pq_init(PQ *pq) {
    pq->n = 1;
    pq->cap = PQ_INITIAL_CAP;
    pq->heap = malloc(pq->cap * sizeof(u32));
    if (!pq->heap) { fprintf(stderr, "pq: out of memory\n"); exit(1); }
}

void pq_push(PQ *pq, u32 event) {
    if (pq->n == pq->cap) {
        u32 *bigger = realloc(pq->heap, (size_t)pq->cap * 2 * sizeof(u32));
        if (!bigger) { fprintf(stderr, "pq: out of memory at cap %u\n", pq->cap); exit(1); }
        pq->heap = bigger;
        pq->cap *= 2;
    }

    u32 j = pq->n;
    while (j > 1) {
        u32 parent = ((j - 2) >> 2) + 1;
        if (pq->heap[parent] <= event) break;
        pq->heap[j] = pq->heap[parent];
        j = parent;
    }
    pq->heap[j] = event;
    pq->n++;
}

u32 pq_peek(const PQ *pq) {
    return pq->heap[1];
}

u32 pq_pop(PQ *pq) {
    if (pq->n == 1) return 0;

    u32 *heap = pq->heap;
    u32 top = heap[1];

    pq->n--;
    u32 last = heap[pq->n];

    u32 j = 1;
    for (;;) {
        u32 first = (j << 2) - 2;
        if (first >= pq->n) break;
        u32 end = first + 4;
        if (end > pq->n) end = pq->n;

        u32 best = first;
        u32 best_e = heap[first];
        for (u32 k = first + 1; k < end; k++) {
            if (heap[k] < best_e) { best = k; best_e = heap[k]; }
        }
        if (best_e >= last) break;

        heap[j] = best_e;
        j = best;
    }
    heap[j] = last;

    return top;
}

void pq_free(PQ *pq) {
    free(pq->heap);
    pq->heap = NULL;
    pq->n = 1;
    pq->cap = 0;
}
