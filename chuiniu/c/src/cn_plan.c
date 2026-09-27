/* Chui Niu - the animation plan. See cn_plan.h. */
#include "cn_plan.h"
#include "cn_internal.h"
#include <string.h>

int cn_plan(const CnGame *g, int from, int to, CnEvent *out, int cap)
{
    static CnGame r;
    if (!g || from < -1 || to < from || to > g->hist_n || to < 0 || cap < 0) return -1;
    CnSink k = { out, cap, 0, from < 0 };
    if (!cn__new(&r, g->seed, g->n, &k)) return -1;
    for (int i = 0; i < to; i++) {
        k.on = i >= from;
        if (!cn__apply(&r, r.turn, g->hist[i], &k)) return -1;
    }
    return k.n <= cap ? k.n : -1;
}

int cn_plan_move(const CnGame *g, CnMove m, CnEvent *out, int cap)
{
    static CnGame r;
    if (!g || cap < 0) return -1;
    r = *g;
    CnSink k = { out, cap, 0, 1 };
    if (!cn__apply(&r, r.turn, m, &k)) return -1;
    return k.n <= cap ? k.n : -1;
}
