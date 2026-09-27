/* Tallybones - the animation plan. See tb_plan.h. */
#include "tb_plan.h"
#include "tb_internal.h"
#include <string.h>

typedef struct { TbEvent *out; int cap, n; } Keep;

static void keep_one(const TbEvent *e, void *ctx)
{
    Keep *k = ctx;
    if (k->n < k->cap) k->out[k->n] = *e;
    k->n++;
}

int tb_plan_each(const TbGame *g, int from, int to, TbEventFn fn, void *ctx)
{
    static TbGame r;
    if (!g || g->draft || from < -1 || to < from || to > g->hist_n) return -1;
    TbSink k = { fn, ctx, from, to, 0, 0 };
    if (!tb__replay(&r, g->seed, g->n, g->starter, g->hist, to, &k)) return -1;
    return k.count;
}

int tb_plan(const TbGame *g, int from, int to, TbEvent *out, int cap)
{
    Keep k = { out, cap, 0 };
    int n = tb_plan_each(g, from, to, keep_one, &k);
    if (n < 0 || k.n > cap) return -1;
    return k.n;
}

int tb_plan_draft(const TbGame *g, TbEvent *out, int cap)
{
    static TbGame d;
    if (!g || !g->draft) return 0;
    /* the resident is the draft without its move: the state it was staged on */
    if (!tb__replay(&d, g->seed, g->n, g->starter, g->hist, g->hist_n, 0)) return -1;
    Keep kp = { out, cap, 0 };
    TbSink k = { keep_one, &kp, g->hist_n, g->hist_n + 1, 0, (uint16_t)(g->hist_n + 1) };
    /* nothing derived: the draft's step is given no body */
    if (!tb__step(&d, g->pending, 0, 0, &k)) return -1;
    return kp.n > cap ? -1 : kp.n;
}
