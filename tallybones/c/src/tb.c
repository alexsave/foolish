/* Tallybones - the rules. See tb.h. */
#include "tb.h"
#include "tb_internal.h"
#include "../../../shared/c/sha256.h"
#include "../../../shared/c/deal_rng.h"
#include <string.h>

/* ---- scoring (T4) ----------------------------------------------------------- */

int tb_score_of(const uint8_t dice[TB_DICE], int cat)
{
    if (!dice || cat < 0 || cat >= TB_CATS) return -1;
    int c[TB_FACES + 1] = { 0 }, sum = 0;
    for (int i = 0; i < TB_DICE; i++) {
        if (dice[i] < 1 || dice[i] > TB_FACES) return -1;
        c[dice[i]]++;
        sum += dice[i];
    }
    int most = 0, three = 0, two = 0;
    for (int f = 1; f <= TB_FACES; f++) {
        if (c[f] > most) most = c[f];
        if (c[f] == 3) three = 1;
        if (c[f] == 2) two = 1;
    }
    /* the longest run of consecutive faces present */
    int run = 0, best = 0;
    for (int f = 1; f <= TB_FACES; f++) {
        run = c[f] ? run + 1 : 0;
        if (run > best) best = run;
    }
    switch (cat) {
    case TB_C_THREE_ALIKE: return most >= 3 ? sum : 0;
    case TB_C_FOUR_ALIKE:  return most >= 4 ? sum : 0;
    case TB_C_FULL_HOUSE:  return three && two ? TB_FULL_HOUSE : 0;
    case TB_C_SHORT_RUN:   return best >= 4 ? TB_SHORT_RUN : 0;
    case TB_C_LONG_RUN:    return best >= 5 ? TB_LONG_RUN : 0;
    case TB_C_TALLYBONES:  return most == 5 ? TB_TALLYBONES : 0;
    case TB_C_ANY:         return sum;
    default:               return (cat + 1) * c[cat + 1];     /* Ones .. Sixes */
    }
}

/* ---- the card ------------------------------------------------------------------ */

int tb_is_in(const TbGame *g, int s)
{
    return s >= 0 && s < g->n && !(g->left >> s & 1);
}

static int seats_in(const TbGame *g)
{
    int k = 0;
    for (int s = 0; s < g->n; s++) k += tb_is_in(g, s);
    return k;
}

int tb_upper(const TbGame *g, int s)
{
    if (s < 0 || s >= g->n) return 0;
    int u = 0;
    for (int c = 0; c < TB_UPPER_CATS; c++) u += g->score[s][c];
    return u;
}

int tb_bonus(const TbGame *g, int s)
{
    return tb_upper(g, s) >= TB_BONUS_AT ? TB_BONUS : 0;
}

int tb_bonus_known(const TbGame *g, int s)
{
    const unsigned numbers = (1u << TB_UPPER_CATS) - 1u;
    if (s < 0 || s >= g->n) return 0;
    return tb_bonus(g, s) > 0 || (g->filled[s] & numbers) == numbers;
}

int tb_total(const TbGame *g, int s)
{
    if (s < 0 || s >= g->n) return 0;
    int t = tb_bonus(g, s);
    for (int c = 0; c < TB_CATS; c++) t += g->score[s][c];
    return t;
}

int tb_winners(const TbGame *g)
{
    if (!g->over) return 0;
    int best = -1, w = 0;
    for (int s = 0; s < g->n; s++) {
        if (!tb_is_in(g, s)) continue;
        int t = tb_total(g, s);
        if (t > best) { best = t; w = 0; }
        if (t == best) w |= 1 << s;
    }
    return w;
}

/* ---- the sink ------------------------------------------------------------------- */

static TbEvent ev0(int kind, int seat)
{
    TbEvent e;
    memset(&e, 0, sizeof e);
    e.kind = (uint8_t)kind;
    e.seat = (uint8_t)seat;
    e.other = TB_SEAT_NONE;
    return e;
}

static void emit(TbSink *k, TbEvent e, const TbGame *g)
{
    (void)g;
    if (!k || k->bubble <= k->from || k->bubble > k->to) return;
    e.bubble = k->bubble;
    k->count++;
    if (k->fn) k->fn(&e, k->ctx);
}

/* ---- the one derivation (T6, T11) ---------------------------------------------- */

void tb__roll(TbGame *g, int keep, const uint8_t *body, int body_len)
{
    DealRng r;
    if (body) {
        uint8_t d[SHA256_DIGEST_LEN], w[2];
        Sha256 c;
        sha256_init(&c);
        sha256_update(&c, g->seed, 32);
        w[0] = (uint8_t)body_len; w[1] = (uint8_t)(body_len >> 8);
        sha256_update(&c, w, 2);
        sha256_update(&c, body, (size_t)body_len);
        sha256_update(&c, &g->turn, 1);
        w[0] = (uint8_t)g->turns; w[1] = (uint8_t)(g->turns >> 8);
        sha256_update(&c, w, 2);
        sha256_update(&c, &g->roll, 1);
        sha256_final(&c, d);
        deal_rng_seed(&r, d);
    }
    /* ONE DRAW PER POSITION, kept or not, so die i's value is draw i of the
     * roll's stream whatever else is kept: two subsets that reroll the same
     * position differ there only because their histories differ (T11.2) */
    for (int i = 0; i < TB_DICE; i++) {
        uint8_t v = body ? (uint8_t)(1 + deal_rng_bounded(&r, TB_FACES)) : 0;
        if (!(keep >> i & 1)) g->dice[i] = v;
    }
}

/* ---- turns ----------------------------------------------------------------------- */

static void roll_event(TbGame *g, int rolled, TbSink *k)
{
    TbEvent e = ev0(TB_EV_ROLL, g->turn);
    e.roll = g->roll;
    e.mask = (uint8_t)rolled;
    memcpy(e.dice, g->dice, TB_DICE);
    emit(k, e, g);
}

static void end_game(TbGame *g, TbSink *k)
{
    g->over = 1;
    g->turn = TB_SEAT_NONE;
    g->roll = 0;
    TbEvent e = ev0(TB_EV_OVER, TB_SEAT_NONE);
    e.mask = (uint8_t)tb_winners(g);
    for (int s = 0; s < g->n; s++)
        if (e.mask >> s & 1) { e.value = (uint16_t)tb_total(g, s); break; }
    emit(k, e, g);
}

/* The next seat still in with a category open, in seat order after `from`
 * (T5: a seat that left is skipped); none, and the game is over. */
static void next_turn(TbGame *g, int from, const uint8_t *body, int body_len, TbSink *k)
{
    for (int i = 1; i <= g->n; i++) {
        int s = (from + i) % g->n;
        if (!tb_is_in(g, s) || g->filled[s] == TB_FULL_CARD) continue;
        TbEvent e = ev0(TB_EV_TURN, s);
        e.other = (uint8_t)from;
        g->turn = (uint8_t)s;
        g->roll = 1;
        g->kept = 0;
        emit(k, e, g);
        tb__roll(g, 0, body, body_len);
        roll_event(g, TB_ALL_KEPT, k);
        return;
    }
    end_game(g, k);
}

int tb__new(TbGame *g, const uint8_t seed[32], int n, int starter, const uint8_t *body, int body_len,
            TbSink *k)
{
    if (!g || !seed || n < TB_MIN_SEATS || n > TB_MAX_SEATS || starter < 0 || starter >= n) return 0;
    memset(g, 0, sizeof *g);
    g->n = (uint8_t)n;
    g->starter = (uint8_t)starter;
    memcpy(g->seed, seed, 32);
    TbEvent e = ev0(TB_EV_START, starter);
    e.value = (uint16_t)n;
    if (k) k->bubble = 0;
    emit(k, e, g);
    /* seat 0 rolls first (T12) */
    e = ev0(TB_EV_TURN, 0);
    g->turn = 0;
    g->roll = 1;
    emit(k, e, g);
    tb__roll(g, 0, body, body_len);
    roll_event(g, TB_ALL_KEPT, k);
    return 1;
}

int tb_new(TbGame *g, const uint8_t seed[32], int n, int starter)
{
    static const uint8_t empty_history[1] = { 1 };     /* the sentinel: no digits */
    return tb__new(g, seed, n, starter, empty_history, 1, 0);
}

/* ---- legality ------------------------------------------------------------------- */

int tb_is_legal(const TbGame *g, TbMove m)
{
    if (!g || g->over || g->draft || m.pad0) return 0;
    switch (m.kind) {
    case TB_M_KEEP:
        return m.seat == g->turn && g->roll < TB_ROLLS && m.arg < TB_ALL_KEPT;
    case TB_M_SCORE:
        return m.seat == g->turn && m.arg < TB_CATS && !(g->filled[m.seat] >> m.arg & 1);
    case TB_M_LEAVE:
        return m.arg == 0 && tb_is_in(g, m.seat);
    }
    return 0;
}

int tb_menu(const TbGame *g, TbMove *out, int cap)
{
    if (!g || g->over || g->draft) return 0;
    int n = 0;
    TbMove m = { 0, g->turn, 0, 0 };
#define PUT(k, a) do { m.kind = (uint8_t)(k); m.arg = (uint8_t)(a); if (n < cap) out[n] = m; n++; } while (0)
    if (g->roll < TB_ROLLS)
        for (int mask = 0; mask < TB_ALL_KEPT; mask++) PUT(TB_M_KEEP, mask);
    for (int c = 0; c < TB_CATS; c++)
        if (!(g->filled[g->turn] >> c & 1)) PUT(TB_M_SCORE, c);
    for (int s = 0; s < g->n; s++)
        if (tb_is_in(g, s)) { m.seat = (uint8_t)s; PUT(TB_M_LEAVE, 0); }
#undef PUT
    return n;
}

/* ---- apply ----------------------------------------------------------------------- */

int tb__step(TbGame *g, TbMove m, const uint8_t *body, int body_len, TbSink *k)
{
    if (!tb_is_legal(g, m)) return 0;
    const int s = m.seat;
    TbEvent e = ev0(TB_EV_NONE, s);
    switch (m.kind) {
    case TB_M_KEEP:
        e.kind = TB_EV_KEEP;
        e.mask = m.arg;
        memcpy(e.dice, g->dice, TB_DICE);
        emit(k, e, g);
        g->kept = m.arg;
        g->roll++;
        tb__roll(g, m.arg, body, body_len);
        roll_event(g, TB_ALL_KEPT & ~m.arg, k);
        return 1;
    case TB_M_SCORE: {
        const int had_bonus = tb_bonus(g, s);
        int pts = tb_score_of(g->dice, m.arg);
        g->score[s][m.arg] = (uint8_t)(pts > 0 ? pts : 0);  /* unknown dice (the coder's walk) score 0 */
        g->filled[s] |= (uint16_t)(1u << m.arg);
        e.kind = TB_EV_SCORE;
        e.cat = m.arg;
        e.value = g->score[s][m.arg];
        emit(k, e, g);
        if (!had_bonus && tb_bonus(g, s)) {
            e = ev0(TB_EV_BONUS, s);
            e.value = TB_BONUS;
            emit(k, e, g);
        }
        g->turns++;
        next_turn(g, s, body, body_len, k);
        return 1;
    }
    case TB_M_LEAVE:
        g->left |= (uint8_t)(1u << s);
        e.kind = TB_EV_LEAVE;
        emit(k, e, g);
        /* the last seat in cannot play alone: the game ends (T13) */
        if (seats_in(g) < TB_MIN_SEATS) end_game(g, k);
        else if (s == g->turn) next_turn(g, s, body, body_len, k);
        return 1;
    }
    return 0;
}

/* ---- the draft (T11) -------------------------------------------------------------- */

int tb_draft(TbGame *out, const TbGame *g, TbMove m)
{
    static TbGame d;
    if (!out || !g || g->draft) return 0;
    d = *g;
    /* THE GUARD: a draft's step is given no body, so it derives nothing and
     * every die it rolls reads 0 */
    if (!tb__step(&d, m, 0, 0, 0)) return 0;
    d.draft = 1;
    d.pending = m;
    *out = d;
    return 1;
}

/* ---- the hash --------------------------------------------------------------------- */

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

uint64_t tb_hash(const TbGame *g)
{
    uint64_t h = 1469598103934665603ull;
    h = fnv(h, &g->n, 1); h = fnv(h, &g->turn, 1); h = fnv(h, &g->roll, 1); h = fnv(h, &g->over, 1);
    h = fnv(h, &g->left, 1); h = fnv(h, &g->starter, 1); h = fnv(h, &g->kept, 1); h = fnv(h, &g->draft, 1);
    h = fnv(h, g->dice, TB_DICE);
    h = fnv(h, &g->turns, 2); h = fnv(h, &g->hist_n, 2);
    h = fnv(h, g->filled, sizeof g->filled);
    h = fnv(h, g->score, sizeof g->score);
    if (g->draft) h = fnv(h, &g->pending, sizeof g->pending);
    h = fnv(h, g->hist, sizeof(TbMove) * g->hist_n);
    return fnv(h, g->seed, 32);
}
