/* Chui Niu - the rules. See cn.h. */
#include "cn.h"
#include "cn_internal.h"
#include <stddef.h>
#include <string.h>

_Static_assert(CN_MAX_MOVES == 2349, "the longest six-seat game (cn.h)");
_Static_assert(CN_MAX_MOVES < 65535, "hist_n is a u16");
_Static_assert(CN_MAX_DICE * CN_BID_FACES + 1 < 256, "a menu is at most 151 entries");

/* ---- the sink ------------------------------------------------------------------ */

static CnEvent *ev(CnSink *k, int kind, int seat, const CnGame *g)
{
    if (!k || !k->on) return 0;
    int i = k->n++;
    if (i >= k->cap) return 0;
    CnEvent *e = &k->out[i];
    memset(e, 0, sizeof *e);
    e->kind = (uint8_t)kind;
    e->seat = (uint8_t)seat;
    e->other = CN_SEAT_NONE;
    e->round = g->round;
    e->move = g->hist_n;
    return e;
}

/* ---- helpers ------------------------------------------------------------------- */

int cn_live_seats(const CnGame *g)
{
    int k = 0;
    for (int s = 0; s < g->n; s++) k += g->dice_n[s] > 0;
    return k;
}

int cn_next_live(const CnGame *g, int from)
{
    for (int i = 1; i <= g->n; i++) {
        int s = (from + i) % g->n;
        if (g->dice_n[s] > 0) return s;
    }
    return CN_SEAT_NONE;
}

int cn_count(const CnGame *g, int f)
{
    int k = 0;
    for (int s = 0; s < g->n; s++)
        for (int i = 0; i < g->dice_n[s]; i++)
            k += g->dice[s][i] == f || g->dice[s][i] == CN_WILD;
    return k;
}

/* ---- the start ------------------------------------------------------------------ */

int cn__new(CnGame *g, const uint8_t seed[32], int n, CnSink *k)
{
    if (n < CN_MIN_SEATS || n > CN_MAX_SEATS) return 0;
    memset(g, 0, sizeof *g);
    g->n = (uint8_t)n;
    memcpy(g->seed, seed, 32);
    for (int s = 0; s < n; s++) g->dice_n[s] = CN_START_DICE;
    g->total = (uint8_t)(n * CN_START_DICE);
    g->phase = CN_PH_BIDDING;
    g->turn = 0;                                   /* R4: the host opens round 1 */
    g->bidder = CN_SEAT_NONE;
    g->winner = CN_SEAT_NONE;
    g->last_seat = CN_SEAT_NONE;
    g->call_seat = CN_SEAT_NONE;
    g->call_bidder = CN_SEAT_NONE;
    g->call_loser = CN_SEAT_NONE;
    cn_roll_round(g);
    ev(k, CN_EV_ROUND, 0, g);
    return 1;
}

int cn_new(CnGame *g, const uint8_t seed[32], int n) { return cn__new(g, seed, n, 0); }

/* ---- legality ------------------------------------------------------------------- */

int cn_can_call(const CnGame *g)
{
    return g->phase != CN_PH_OVER && g->bid_q > 0;
}

static int bid_ok(const CnGame *g, int q, int f)
{
    if (q < 1 || q > g->total || f < CN_FACE_LO || f > CN_FACES) return 0;       /* R2, R7 */
    return g->bid_q == 0 || cn_rank(q, f) > cn_rank(g->bid_q, g->bid_f);        /* R2, R5 */
}

int cn_is_legal(const CnGame *g, int seat, CnMove m)
{
    if (g->phase == CN_PH_OVER || seat != g->turn) return 0;
    if (cn_is_call(m)) return m.f == 0 && cn_can_call(g);
    return bid_ok(g, m.q, m.f);
}

int cn_min_raise(const CnGame *g, int *q, int *f)
{
    if (g->phase == CN_PH_OVER) return 0;
    int r = g->bid_q ? cn_rank(g->bid_q, g->bid_f) + 1 : 0;
    int bq = r / CN_BID_FACES + 1, bf = r % CN_BID_FACES + CN_FACE_LO;
    if (bq > g->total) return 0;
    if (q) *q = bq;
    if (f) *f = bf;
    return 1;
}

int cn_min_quantity(const CnGame *g, int f)
{
    if (g->phase == CN_PH_OVER || f < CN_FACE_LO || f > CN_FACES) return 0;
    int q = g->bid_q == 0 ? 1 : f > g->bid_f ? g->bid_q : g->bid_q + 1;
    return q <= g->total ? q : 0;
}

int cn_legal(const CnGame *g, CnMove *out, int cap)
{
    if (g->phase == CN_PH_OVER) return 0;
    int n = 0;
    if (cn_can_call(g)) {
        if (out && n < cap) out[n] = (CnMove){ 0, 0 };
        n++;
    }
    int q, f;
    if (!cn_min_raise(g, &q, &f)) return n;
    for (int r = cn_rank(q, f); r < g->total * CN_BID_FACES; r++) {
        if (out && n < cap) out[n] = (CnMove){ (uint8_t)(r / CN_BID_FACES + 1), (uint8_t)(r % CN_BID_FACES + CN_FACE_LO) };
        n++;
    }
    return n;
}

/* ---- apply ------------------------------------------------------------------------ */

static void open_round(CnGame *g, int opener, CnSink *k)
{
    g->round++;
    g->round_at = g->hist_n;
    g->bid_q = g->bid_f = 0;
    g->bidder = CN_SEAT_NONE;
    g->turn = (uint8_t)opener;
    g->phase = CN_PH_REVEALED;
    cn_roll_round(g);
    ev(k, CN_EV_ROUND, opener, g);
}

static void call(CnGame *g, int seat, CnSink *k)
{
    const int q = g->bid_q, f = g->bid_f, bidder = g->bidder;
    const int count = cn_count(g, f);
    /* R3: the bid stands when the count reaches it, and the caller loses */
    const int loser = count >= q ? seat : bidder;

    g->call_seat = (uint8_t)seat;
    g->call_at = g->hist_n;
    g->call_bidder = (uint8_t)bidder;
    g->call_q = (uint8_t)q;
    g->call_f = (uint8_t)f;
    g->call_count = (uint8_t)count;
    g->call_loser = (uint8_t)loser;
    memcpy(g->shown_n, g->dice_n, sizeof g->shown_n);
    memcpy(g->shown, g->dice, sizeof g->shown);

    CnEvent *e = ev(k, CN_EV_CALL, seat, g);
    if (e) { e->other = (uint8_t)bidder; e->q = (uint8_t)q; e->f = (uint8_t)f; }
    e = ev(k, CN_EV_REVEAL, seat, g);
    if (e) {
        e->other = (uint8_t)bidder; e->q = (uint8_t)q; e->f = (uint8_t)f; e->count = (uint8_t)count;
        memcpy(e->dice_n, g->dice_n, sizeof e->dice_n);
        for (int s = 0; s < g->n; s++)
            for (int i = 0; i < g->dice_n[s]; i++) e->dice[s * CN_START_DICE + i] = g->dice[s][i];
    }

    g->dice_n[loser]--;
    g->total--;
    e = ev(k, CN_EV_LOSE, loser, g);
    if (e) e->count = g->dice_n[loser];
    if (g->dice_n[loser] == 0) ev(k, CN_EV_OUT, loser, g);

    if (cn_live_seats(g) == 1) {
        g->phase = CN_PH_OVER;
        g->winner = (uint8_t)cn_next_live(g, loser);
        g->turn = CN_SEAT_NONE;
        g->bid_q = g->bid_f = 0;
        g->bidder = CN_SEAT_NONE;
        memset(g->dice, 0, sizeof g->dice);
        ev(k, CN_EV_OVER, g->winner, g);
        return;
    }
    /* R4: the loser opens, or the next live seat after an eliminated one */
    open_round(g, g->dice_n[loser] ? loser : cn_next_live(g, loser), k);
}

int cn__apply(CnGame *g, int seat, CnMove m, CnSink *k)
{
    if (!cn_is_legal(g, seat, m) || g->hist_n >= CN_MAX_MOVES) return 0;
    g->hist[g->hist_n++] = m;
    g->last_seat = (uint8_t)seat;
    if (cn_is_call(m)) {
        call(g, seat, k);
        return 1;
    }
    g->bid_q = m.q;
    g->bid_f = m.f;
    g->bidder = (uint8_t)seat;
    g->phase = CN_PH_BIDDING;
    g->turn = (uint8_t)cn_next_live(g, seat);
    CnEvent *e = ev(k, CN_EV_BID, seat, g);
    if (e) { e->q = m.q; e->f = m.f; }
    return 1;
}

int cn_apply(CnGame *g, int seat, CnMove m) { return cn__apply(g, seat, m, 0); }

int cn_replay(CnGame *out, const CnGame *g, int k)
{
    static CnGame r;
    if (k < 0 || k > g->hist_n) return 0;
    if (!cn_new(&r, g->seed, g->n)) return 0;
    for (int i = 0; i < k; i++)
        if (!cn_apply(&r, r.turn, g->hist[i])) return 0;
    *out = r;
    return 1;
}

/* ---- the hash ---------------------------------------------------------------------- */

static uint64_t fnv(uint64_t h, const void *p, int n)
{
    const uint8_t *b = (const uint8_t *)p;
    for (int i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

uint64_t cn_hash(const CnGame *g)
{
    uint64_t h = 1469598103934665603ull;
    h = fnv(h, g, (int)offsetof(CnGame, hist));
    h = fnv(h, g->hist, (int)sizeof(CnMove) * g->hist_n);
    return fnv(h, g->seed, 32);
}
