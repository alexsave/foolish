/* Tallybones - the timeline. See tb_beats.h. */
#include "tb_beats.h"
#include "tb_code.h"
#include <string.h>

/* ---- curves ----------------------------------------------------------------------- */

static float back_out(float t, float c1)
{
    float u = t - 1.0f, c3 = c1 + 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

float tb_ease(int ease, float t)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    switch (ease) {
    case TB_EASE_OUT:    { float u = 1.0f - t; return 1.0f - u * u * u; }
    case TB_EASE_SETTLE: return back_out(t, 1.2f);
    case TB_EASE_STAMP:  return back_out(t, 2.4f);
    default:             return t;
    }
}

/* ---- the board before ---------------------------------------------------------------- */

static void frame_of(const TbGame *g, TbBeatFrame *f)
{
    memset(f, 0, sizeof *f);
    f->turn = TB_SEAT_NONE;
    f->next_ms = TB_BEAT_NEVER;
    if (!g) { f->in = 0xFF; return; }
    memcpy(f->dice, g->dice, TB_DICE);
    f->kept = g->roll > 1 ? g->kept : 0;
    f->turn = g->turn;
    f->roll = g->roll;
    f->results = g->over;
    f->winners = (uint8_t)tb_winners(g);
    for (int s = 0; s < g->n; s++) {
        if (tb_is_in(g, s)) f->in |= (uint8_t)(1u << s);
        f->filled[s] = g->filled[s];
        f->total[s] = (uint16_t)tb_total(g, s);
    }
}

int tb_beats_pre(const TbGame *g, int from, TbBeatFrame *out)
{
    static TbGame r;
    if (!g || from < -1 || from > g->hist_n) return 0;
    if (from < 0) { frame_of(0, out); return 1; }
    if (!tb_replay(&r, g->seed, g->n, g->starter, g->hist, from)) return 0;
    frame_of(&r, out);
    return 1;
}

/* ---- building --------------------------------------------------------------------------- */

static int popcount5(int m)
{
    int k = 0;
    for (int i = 0; i < TB_DICE; i++) k += m >> i & 1;
    return k;
}

int tb_beats_build(const TbEvent *ev, int n, const TbBeatFrame *start, int n_seats, int mode, TbBeats *out)
{
    memset(out, 0, sizeof *out);
    out->mode = (uint8_t)mode;
    out->n_seats = (uint8_t)n_seats;
    out->start = *start;
    uint32_t t = mode == TB_BEATS_OPEN ? TB_T_LEAD_OPEN : TB_T_LEAD_LIVE;
    uint16_t totals[TB_MAX_SEATS];
    memcpy(totals, start->total, sizeof totals);
    for (int i = 0; i < n; i++) {
        const TbEvent *e = &ev[i];
        TbBeat b;
        memset(&b, 0, sizeof b);
        b.start_ms = t;
        b.ev_i = (uint16_t)i;
        b.ev_kind = e->kind;
        b.seat = e->seat;
        b.other = TB_SEAT_NONE;
        switch (e->kind) {
        case TB_EV_ROLL:
            b.kind = TB_BK_SETTLE;
            b.ease = TB_EASE_SETTLE;
            b.mask = e->mask;
            b.parts = (uint8_t)popcount5(e->mask);
            b.stagger_ms = TB_T_SETTLE_STEP;
            b.part_ms = TB_T_SETTLE;
            b.dur_ms = (uint16_t)((b.parts - 1) * TB_T_SETTLE_STEP + TB_T_SETTLE);
            b.sub = e->roll;
            memcpy(b.dice, e->dice, TB_DICE);
            break;
        case TB_EV_SCORE: case TB_EV_BONUS:
            b.kind = TB_BK_STAMP;
            b.ease = TB_EASE_STAMP;
            b.sub = e->kind == TB_EV_SCORE ? TB_STAMP_SCORE : TB_STAMP_BONUS;
            b.cat = e->kind == TB_EV_SCORE ? e->cat : TB_CATS;
            b.value = e->value;
            if (e->seat < TB_MAX_SEATS) totals[e->seat] = (uint16_t)(totals[e->seat] + e->value);
            b.total = e->seat < TB_MAX_SEATS ? totals[e->seat] : 0;
            b.dur_ms = b.part_ms = TB_T_STAMP;
            break;
        case TB_EV_TURN:
            b.kind = TB_BK_TURN;
            b.ease = TB_EASE_OUT;
            b.other = e->other;
            b.dur_ms = b.part_ms = TB_T_TURN;
            break;
        case TB_EV_LEAVE:
            b.kind = TB_BK_FADE;
            b.ease = TB_EASE_OUT;
            b.sub = TB_FADE_SEAT_OUT;
            b.dur_ms = b.part_ms = TB_T_FADE;
            break;
        case TB_EV_OVER:
            b.kind = TB_BK_FADE;
            b.ease = TB_EASE_OUT;
            b.sub = TB_FADE_RESULTS_IN;
            b.mask = e->mask;
            b.dur_ms = b.part_ms = TB_T_FADE;
            break;
        default:
            continue;            /* START, KEEP and the lobby's move nothing */
        }
        b.parts = b.parts ? b.parts : 1;
        if (out->n >= TB_BEATS_MAX) return -1;
        out->beat[out->n++] = b;
        t += b.dur_ms + TB_T_GAP;
        if (e->kind == TB_EV_OVER) {
            TbBeat h;
            memset(&h, 0, sizeof h);
            h.kind = TB_BK_HOLD;
            h.start_ms = t;
            h.dur_ms = h.part_ms = TB_T_OVER;
            h.parts = 1;
            h.ev_i = (uint16_t)i;
            h.ev_kind = e->kind;
            h.seat = TB_SEAT_NONE;
            if (out->n >= TB_BEATS_MAX) return -1;
            out->beat[out->n++] = h;
            t += TB_T_OVER;
        }
    }
    out->total_ms = out->n ? out->beat[out->n - 1].start_ms + out->beat[out->n - 1].dur_ms : 0;
    return out->n;
}

/* ---- sampling ------------------------------------------------------------------------------ */

/* Where die i starts in a SETTLE: its rank among the rolled dice. -1 when it
 * is not rolled. */
static int die_start(const TbBeat *b, int i, uint32_t *at)
{
    if (i < 0 || i >= TB_DICE || !(b->mask >> i & 1)) return -1;
    int k = 0;
    for (int j = 0; j < i; j++) k += b->mask >> j & 1;
    *at = b->start_ms + (uint32_t)k * b->stagger_ms;
    return k;
}

static void soonest(uint32_t *next, uint32_t at, uint32_t now)
{
    if (at > now && at < *next) *next = at;
}

void tb_beats_frame(const TbBeats *b, uint32_t now, TbBeatFrame *out)
{
    *out = b->start;
    out->rolling = 0;
    uint32_t next = TB_BEAT_NEVER;
    for (int k = 0; k < b->n; k++) {
        const TbBeat *x = &b->beat[k];
        const uint32_t end = x->start_ms + x->dur_ms;
        soonest(&next, x->start_ms, now);
        soonest(&next, end, now);
        if (now < x->start_ms) continue;
        switch (x->kind) {
        case TB_BK_SETTLE:
            out->roll = x->sub;
            out->kept = (uint8_t)(x->sub > 1 ? TB_ALL_KEPT & ~x->mask : 0);
            for (int i = 0; i < TB_DICE; i++) {
                uint32_t at;
                if (die_start(x, i, &at) < 0) continue;
                soonest(&next, at, now);
                soonest(&next, at + x->part_ms, now);
                if (now < at) { out->dice[i] = 0; continue; }
                if (now < at + x->part_ms && x->dice[i]) {
                    /* tumbling: a face that is not a value, only a blur of them */
                    uint32_t tick = (now - at) / TB_T_TUMBLE;
                    out->dice[i] = (uint8_t)(1 + (tick * 5u + (uint32_t)i * 2u + x->dice[i]) % 6u);
                    out->rolling |= (uint8_t)(1u << i);
                    soonest(&next, at + (tick + 1) * TB_T_TUMBLE, now);
                    continue;
                }
                out->dice[i] = x->dice[i];
            }
            break;
        case TB_BK_STAMP:
            if (now < end || x->seat >= TB_MAX_SEATS) break;
            if (x->cat < TB_CATS) out->filled[x->seat] |= (uint16_t)(1u << x->cat);
            out->total[x->seat] = x->total;
            break;
        case TB_BK_TURN:
            out->turn = x->seat;
            break;
        case TB_BK_FADE:
            if (x->sub == TB_FADE_RESULTS_IN) {
                out->results = 1;
                out->winners = x->mask;
                out->turn = TB_SEAT_NONE;
            } else if (now >= end && x->seat < TB_MAX_SEATS) {
                out->in &= (uint8_t)~(1u << x->seat);
            }
            break;
        }
    }
    out->now_ms = now;
    out->next_ms = next;
    out->done = now >= b->total_ms;
}

void tb_beat_sample(const TbBeat *b, uint32_t now, int part, TbBeatSample *out)
{
    memset(out, 0, sizeof *out);
    out->scale = 1.0f;
    out->opacity = 1.0f;
    uint32_t at = b->start_ms, run = b->part_ms;
    if (b->kind == TB_BK_SETTLE && die_start(b, part, &at) < 0) {
        out->state = TB_BS_DONE;           /* a kept die: it never moves */
        out->p = 1.0f;
        return;
    }
    float t = now <= at ? 0.0f : now >= at + run ? 1.0f : (float)(now - at) / (float)run;
    out->state = (uint8_t)(now < at ? TB_BS_PENDING : now < at + run ? TB_BS_ACTIVE : TB_BS_DONE);
    out->apply = out->state == TB_BS_ACTIVE;
    float p = tb_ease(b->ease, t);
    out->p = p;
    const float q = 1.0f - t;
    switch (b->kind) {
    case TB_BK_SETTLE:
        /* a drop from above the tray, spinning, that lands a little big */
        out->rot = 540.0f * q * q;
        out->dy = -36.0f * q * q;
        out->scale = 1.0f + 0.22f * q;
        out->apply = out->state == TB_BS_ACTIVE;
        break;
    case TB_BK_STAMP:
        out->scale = 1.7f - 0.7f * p;
        out->opacity = t < 0.3f ? t / 0.3f : 1.0f;
        out->apply = out->state != TB_BS_DONE;     /* hidden until it starts */
        if (out->state == TB_BS_PENDING) out->opacity = 0.0f;
        break;
    case TB_BK_FADE:
        out->opacity = b->sub == TB_FADE_RESULTS_IN ? p : 1.0f - p;
        out->apply = b->sub == TB_FADE_RESULTS_IN ? out->state != TB_BS_DONE : out->state != TB_BS_PENDING;
        break;
    default:
        break;
    }
}
