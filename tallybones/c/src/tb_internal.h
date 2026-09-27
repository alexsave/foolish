/* Tallybones - what tb.c shares with tb_code.c, tb_plan.c and the tests.
 *
 * THE SINK (pk_internal.h's): the one apply path takes an optional sink and
 * reports every thing it does to it; a NULL sink is play, a live sink is the
 * plan.
 *
 * THE BODY. Where a step rolls dice, `body` is the encoded history through
 * the move just applied (the exact mixed-radix bytes the wire carries for
 * it, tb_code.h), and the dice are derived from it. A NULL body derives
 * nothing: the rolled dice read 0. Only the resident replay (tb_code.c's
 * walker) and tb_new ever pass a body. */
#ifndef TB_INTERNAL_H
#define TB_INTERNAL_H

#include "tb.h"
#include "tb_plan.h"

typedef struct {
    TbEventFn fn;
    void     *ctx;
    int       from, to;     /* bubbles (from, to] are reported */
    int       count;
    uint16_t  bubble;       /* the bubble being applied        */
} TbSink;

/* tb_new with the derivation optional (NULL body: the dice of roll 1 read 0). */
int tb__new(TbGame *g, const uint8_t seed[32], int n, int starter, const uint8_t *body, int body_len,
            TbSink *k);

/* Apply a legal move WITHOUT appending it to hist[]. 1, or 0 for an illegal
 * one (g untouched). */
int tb__step(TbGame *g, TbMove m, const uint8_t *body, int body_len, TbSink *k);

/* The one derivation (T11): the dice at the positions NOT in `keep` take
 * values from SHA-256(seed || u16 body length || body || seat || u16 turn ||
 * roll) through deal_rng; with no body they read 0. */
void tb__roll(TbGame *g, int keep, const uint8_t *body, int body_len);

/* tb_replay reporting to `k` (may be NULL), straight into `out`. */
int tb__replay(TbGame *out, const uint8_t seed[32], int n, int starter, const TbMove *moves, int kk,
               TbSink *k);

#endif
