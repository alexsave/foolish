/* Pick 'Em Up - what pk.c shares with pk_plan.c and nothing else includes.
 *
 * THE SINK. pk.c's one apply path takes an optional sink and reports every
 * thing it does to it; a NULL sink is the ordinary pk_apply, and a live sink
 * is the plan. That is how the plan and the state cannot disagree: they are
 * the same code running once. */
#ifndef PK_INTERNAL_H
#define PK_INTERNAL_H

#include "pk.h"
#include "pk_plan.h"

typedef struct {
    PkEventFn fn;
    void     *ctx;
    int       viewer;       /* a seat, PK_VIEW_SPECTATOR or PK_VIEW_ALL        */
    int       from, to;     /* bubbles (from, to] are reported                 */
    int       count;        /* events reported                                 */
    uint16_t  step;         /* the running step number                         */
    uint16_t  bubble;       /* the bubble being replayed, 0 for the deal       */
    uint16_t  turn_draws;   /* own draws in the current turn (DRAW i, PASS n)  */
} PkSink;

/* pk_new with the starter known, reporting to `k` (may be NULL). */
int pk__new(PkGame *g, const uint8_t seed[32], int n, int starter, PkSink *k);

/* pk_apply and pk_seal reporting to `k` (may be NULL). Tests use these to
 * watch a hand-built position; nothing else should. */
int pk__apply(PkGame *g, int seat, PkAct a, PkSink *k);
int pk__seal(PkGame *g, PkSink *k);

/* pk_replay reporting to `k` (may be NULL). */
int pk__replay(PkGame *out, const PkGame *g, int kk, PkSink *k);

#endif
