/* Chui Niu - what cn.c shares with cn_plan.c and nothing else includes.
 *
 * THE SINK. cn.c's one apply path takes an optional sink and reports every
 * thing it does to it; a NULL sink is the ordinary cn_apply, a live one is
 * the plan. */
#ifndef CN_INTERNAL_H
#define CN_INTERNAL_H

#include "cn.h"
#include "cn_plan.h"

typedef struct {
    CnEvent *out;
    int      cap;
    int      n;          /* events reported (may pass cap: then the plan fails) */
    int      on;         /* 0 while replaying up to the range's start           */
} CnSink;

int cn__new(CnGame *g, const uint8_t seed[32], int n, CnSink *k);
int cn__apply(CnGame *g, int seat, CnMove m, CnSink *k);

#endif
