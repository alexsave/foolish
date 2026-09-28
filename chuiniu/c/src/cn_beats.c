/* Chui Niu - the plan on a clock. See cn_beats.h. */
#include "cn_beats.h"
#include <string.h>

float cn_ease(float t)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f) / 2.0f;
}

void cn_beats_start(const CnGame *g, CnBeatFrame *f)
{
    memset(f, 0, sizeof *f);
    f->bid_q = g->bid_q;
    f->bid_f = g->bid_f;
    f->bidder = g->bidder;
    f->winner = g->phase == CN_PH_OVER ? g->winner : CN_SEAT_NONE;
    f->round = g->round;
    memcpy(f->dice_n, g->dice_n, sizeof f->dice_n);
    f->next_ms = CN_BEAT_NEVER;
    f->done = 1;
}

static int beat(CnBeats *b, uint32_t *t, int kind, uint32_t dur, int ev_i, const CnEvent *e)
{
    if (b->n >= CN_BEATS_MAX) return -1;
    CnBeat *x = &b->beat[b->n++];
    memset(x, 0, sizeof *x);
    x->start_ms = *t;
    x->dur_ms = dur;
    x->ev_i = (uint16_t)ev_i;
    x->kind = (uint8_t)kind;
    x->seat = e->seat;
    x->other = e->other;
    x->q = e->q;
    x->f = e->f;
    x->count = e->count;
    x->round = e->round;
    *t += dur + CN_T_GAP;
    b->total_ms = x->start_ms + dur;
    return 0;
}

int cn_beats_build(const CnEvent *ev, int n, const CnBeatFrame *start, CnBeats *b)
{
    memset(b, 0, sizeof *b);
    b->start = *start;
    uint32_t t = 0;
    for (int i = 0; i < n; i++) {
        const CnEvent *e = &ev[i];
        int r = 0;
        switch (e->kind) {
        case CN_EV_ROUND:  r = beat(b, &t, CN_BK_SHAKE, CN_T_SHAKE, i, e); break;
        case CN_EV_BID:    r = beat(b, &t, CN_BK_BID, CN_T_BID, i, e); break;
        case CN_EV_CALL:   r = beat(b, &t, CN_BK_CALL, CN_T_CALL, i, e); break;
        case CN_EV_REVEAL:
            r = beat(b, &t, CN_BK_LIFT, CN_T_LIFT, i, e);
            if (!r) r = beat(b, &t, CN_BK_COUNT, (uint32_t)e->count * CN_T_COUNT_STEP + CN_T_COUNT_REST, i, e);
            break;
        case CN_EV_LOSE:   r = beat(b, &t, CN_BK_DROP, CN_T_DROP, i, e); break;
        case CN_EV_OUT:    r = beat(b, &t, CN_BK_OUT, CN_T_OUT, i, e); break;
        case CN_EV_OVER:   r = beat(b, &t, CN_BK_WIN, CN_T_WIN, i, e); break;
        default: break;                               /* the lobby's: no motion */
        }
        if (r) return -1;
    }
    b->start.n = (uint8_t)b->n;
    b->start.done = b->n == 0;
    return b->n;
}

static void soonest(uint32_t *next, uint32_t at, uint32_t now)
{
    if (at > now && at < *next) *next = at;
}

void cn_beats_frame(const CnBeats *b, uint32_t now, CnBeatFrame *f)
{
    *f = b->start;
    f->now_ms = now;
    f->next_ms = CN_BEAT_NEVER;
    f->n = (uint8_t)b->n;
    int all_done = 1;
    for (int i = 0; i < b->n && i < CN_BEATS_MAX; i++) {
        const CnBeat *x = &b->beat[i];
        const uint32_t end = x->start_ms + x->dur_ms;
        soonest(&f->next_ms, x->start_ms, now);
        soonest(&f->next_ms, end, now);
        if (now < x->start_ms) {
            f->state[i] = CN_BS_PENDING;
            f->prog[i] = 0.0f;
            all_done = 0;
            continue;
        }
        const int done = now >= end;
        f->state[i] = done ? CN_BS_DONE : CN_BS_ACTIVE;
        f->prog[i] = done || !x->dur_ms ? 1.0f : cn_ease((float)(now - x->start_ms) / (float)x->dur_ms);
        if (!done) all_done = 0;
        switch (x->kind) {
        case CN_BK_SHAKE:
            f->cups_up = 0;
            f->highlight_f = f->highlight_n = 0;
            f->bid_q = f->bid_f = 0;
            f->bidder = CN_SEAT_NONE;
            f->shaking = !done;
            f->round = x->round;
            break;
        case CN_BK_BID:
            f->bid_q = x->q;
            f->bid_f = x->f;
            f->bidder = x->seat;
            break;
        case CN_BK_LIFT:
            f->cups_up = 1;
            break;
        case CN_BK_COUNT: {
            f->highlight_f = x->f;
            uint32_t lit = x->count ? (now - x->start_ms) / CN_T_COUNT_STEP + 1 : 0;
            if (lit > x->count) lit = x->count;
            f->highlight_n = (uint8_t)lit;
            for (uint32_t k = 1; k <= x->count; k++) soonest(&f->next_ms, x->start_ms + k * CN_T_COUNT_STEP, now);
            break;
        }
        case CN_BK_DROP:
            if (done) f->dice_n[x->seat] = x->count;
            break;
        case CN_BK_WIN:
            f->winner = x->seat;
            f->bid_q = f->bid_f = 0;
            f->bidder = CN_SEAT_NONE;
            break;
        default:
            break;
        }
    }
    f->done = (uint8_t)all_done;
}
