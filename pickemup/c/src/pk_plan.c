/* Pick 'Em Up - the animation plan (RULES_AND_KERNEL.md section 5).
 * The events come from pk.c's own apply path through a sink; see pk_plan.h
 * for why there is no second derivation here. */
#include "pk_internal.h"
#include <string.h>

int pk_plan_each(const PkGame *g, int viewer, int from, int to, PkEventFn fn, void *ctx)
{
    PkSink k;
    memset(&k, 0, sizeof k);
    k.fn = fn;
    k.ctx = ctx;
    k.viewer = viewer;
    k.from = from;
    k.to = to;
    PkGame tmp;
    if (!pk__replay(&tmp, g, g->hist_n, &k)) return -1;
    return k.count;
}

typedef struct { PkEvent *out; int cap, n; } Collect;

static void collect(const PkEvent *e, void *ctx)
{
    Collect *c = (Collect *)ctx;
    if (c->n < c->cap) c->out[c->n] = *e;
    c->n++;
}

int pk_plan(const PkGame *g, int viewer, int from, int to, PkEvent *out, int cap)
{
    Collect c = { out, cap, 0 };
    int n = pk_plan_each(g, viewer, from, to, collect, &c);
    if (n < 0 || c.n > cap) return -1;
    return c.n;
}

int pk_plan_draft(const PkGame *g, int viewer, PkEvent *out, int cap)
{
    if (!g->b_open) return 0;
    return pk_plan(g, viewer, g->bubbles, g->bubbles + 1, out, cap);
}

/* ---- since last seen ------------------------------------------------------- */

static void since_one(const PkEvent *e, void *ctx)
{
    PkSince *s = (PkSince *)ctx;
    switch (e->kind) {
    case PK_EV_DRAW:             s->drawn[e->seat]++; break;
    case PK_EV_PENALTY_DRAW:     s->penalty[e->seat]++; break;
    case PK_EV_PLAY:             s->plays[e->seat]++; break;
    case PK_EV_RESHUFFLE_DONE:   s->reshuffles++; break;
    case PK_EV_CALL_HIT:         s->caught = e->seat; s->caught_by = e->other; break;
    case PK_EV_CALL_MISS:        s->wrong = e->seat; s->wrong_on = e->other; break;
    case PK_EV_SAY_IT:           s->said |= (uint8_t)(1u << e->seat); break;
    case PK_EV_SKIP:
    case PK_EV_REVERSE_AS_SKIP:  s->skipped |= (uint8_t)(1u << e->seat); break;
    case PK_EV_REVERSE:          s->reversed++; break;
    default: break;
    }
}

int pk_since(const PkGame *g, int from, int to, PkSince *out)
{
    memset(out, 0, sizeof *out);
    out->from = (uint16_t)(from < 0 ? 0 : from);
    out->to = (uint16_t)to;
    out->caught = out->caught_by = out->wrong = out->wrong_on = PK_SEAT_NONE;
    return pk_plan_each(g, PK_VIEW_ALL, from, to, since_one, out) >= 0;
}
