/* Pick 'Em Up - the bots. See pk_bot.h for the contract and
 * RULES_AND_KERNEL.md "Bot" for the reference (foolish's octogen) and how it
 * was measured. */
#include "pk_bot.h"
#include "pk_belief.h"
#include <string.h>

const char *const PK_BOT_NAME[PK_BOT_COUNT] = { "random", "greedy", "mc" };

uint64_t pk_bot_rand(uint64_t *rng)
{
    uint64_t x = *rng ? *rng : 0x9e3779b97f4a7c15ull;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    *rng = x;
    return x * 2685821657736338717ull;
}

static uint32_t below(uint64_t *rng, uint32_t n)
{
    return n ? (uint32_t)((pk_bot_rand(rng) >> 33) % n) : 0;
}

static uint64_t mix64(uint64_t z)
{
    z += 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

/* THE DEFAULTS ARE MEASURED (docs/BOT_REPORT.md): at these budgets MC beats
 * random with the interval clear of 50% at every table size, and the arena's
 * 2,000 games a line-up run in minutes. */
void pk_bot_knobs_default(PkBotKnobs *k)
{
    memset(k, 0, sizeof *k);
    k->w1 = 48;
    k->w2 = 64;
    k->w3 = 64;
    k->depth = 12;
    k->wild_keep = 20;
    k->draw_keep = 100;
    k->soft_mod = 4;
    k->flags = 0;
    k->seed = 0x5eed;
}

/* ---- greedy ------------------------------------------------------------------ */

/* THE GREEDY RULE, in points (higher plays first):
 *   - going out wins: the last card is always played;
 *   - stay in a suit: +2 per card of the played card's suit still held after
 *     it (the suit you hold most keeps the most answers), and a wild names
 *     the suit held most, lowest suit on a tie;
 *   - shed action cards early: +3 for a Skip, Reverse or +2 (they are worth
 *     as much tempo now as later, and a hand of them is a hand of dead ends
 *     once the suit turns);
 *   - hit the leader: +6 for a +2, Wild +4 or Skip (and a two-player Reverse)
 *     when the seat it hits holds two cards or fewer;
 *   - keep wilds: -8 for a Wild, -10 for a Wild +4, so a wild is played only
 *     when nothing suited is, or when it is the one that hits the leader.
 * With no play: draw once, then pass. Greedy never draws holding a play. */
static int suited_held(const PkGame *g, int seat, int skip_pos, int suit)
{
    int n = 0;
    for (int i = 0; i < g->hand_n[seat]; i++)
        if (i != skip_pos && pk_suit(g->hand[seat][i]) == suit) n++;
    return n;
}

static int best_suit(const PkGame *g, int seat, int skip_pos)
{
    int best = 0, bn = -1;
    for (int s = 0; s < PK_SUITS; s++) {
        int n = suited_held(g, seat, skip_pos, s);
        if (n > bn) { bn = n; best = s; }
    }
    return best;
}

static int play_points(const PkGame *g, int seat, PkAct a)
{
    int pos = a.a;
    uint8_t c = g->hand[seat][pos];
    if (g->hand_n[seat] == 1) return 1000;
    int r = pk_rank(c), pts = 0;
    int victim = pk_next(g, seat, 1);
    int hits = r == PK_R_PLUS2 || r == PK_R_WILD4 || r == PK_R_SKIP
            || (r == PK_R_REVERSE && g->n == 2);
    if (hits && g->hand_n[victim] <= 2) pts += 6;
    if (pk_is_wild(c)) {
        pts -= r == PK_R_WILD4 ? 10 : 8;
        int want = best_suit(g, seat, pos);
        pts += a.b == want ? 2 * suited_held(g, seat, pos, a.b) : -1;
    } else {
        pts += 2 * suited_held(g, seat, pos, pk_suit(c));
        if (r == PK_R_SKIP || r == PK_R_REVERSE || r == PK_R_PLUS2) pts += 3;
    }
    return pts;
}

int pk_bot_greedy(const PkGame *g, int seat, const PkAct *m, int n)
{
    int best = -1, bp = 0, draw = -1, pass = -1;
    for (int i = 0; i < n; i++) {
        if (m[i].kind == PK_A_DRAW) { draw = i; continue; }
        if (m[i].kind == PK_A_PASS) { pass = i; continue; }
        if (m[i].kind != PK_A_PLAY) continue;
        int p = play_points(g, seat, m[i]);
        if (best < 0 || p > bp) { best = i; bp = p; }
    }
    if (best >= 0) return best;
    if (draw >= 0 && (!g->t_drew || pass < 0)) return draw;
    if (pass >= 0) return pass;
    return 0;
}

/* ---- Monte Carlo -------------------------------------------------------------- */

#define MC_MAX_CANDS 64

static void lite_copy(PkGame *d, const PkGame *s)
{
    size_t head = (size_t)((const char *)&s->hist[0] - (const char *)s);
    memcpy(d, s, head + (size_t)s->hist_n * sizeof(PkAct));
    memcpy(d->seed, s->seed, sizeof d->seed);
}

/* Greedy for every seat from here, `depth` turn actions or to the end. The
 * value for `me`: 1 won, 0 lost, else the share of 1/cards. */
static double rollout(PkGame *w, int me, int depth)
{
    PkAct m[PK_BOT_MENU_CAP];
    int steps = 0;
    while (!w->over && (!depth || steps < depth)) {
        int seat = w->b_open ? w->b_sender : w->turn;
        int n = pk_legal_turn(w, seat, m, PK_BOT_MENU_CAP);
        if (n == 0) {
            if (w->b_open && pk_can_seal(w) && pk_seal(w)) continue;
            break;
        }
        PkAct a = m[pk_bot_greedy(w, seat, m, n)];
        if (!pk_apply(w, seat, a)) break;
        steps++;
        if (a.kind != PK_A_DRAW && pk_can_seal(w)) pk_seal(w);
    }
    if (w->over) return w->winner == me ? 1.0 : 0.0;
    double mine = 1.0 / (double)(w->hand_n[me] ? w->hand_n[me] : 1), all = 0.0;
    for (int s = 0; s < w->n; s++) all += 1.0 / (double)(w->hand_n[s] ? w->hand_n[s] : 1);
    return mine / all;
}

/* Two menu entries do the same thing when they play cards of one suit and
 * rank (the two copies of a card) with the same suit named. */
static int same_move(const PkGame *g, int seat, PkAct x, PkAct y)
{
    if (x.kind != y.kind) return 0;
    if (x.kind != PK_A_PLAY) return 1;
    uint8_t a = g->hand[seat][x.a], b = g->hand[seat][y.a];
    return pk_suit(a) == pk_suit(b) && pk_rank(a) == pk_rank(b) && x.b == y.b;
}

static int mc_pick(const PkGame *g, int seat, const PkAct *m, int n,
                   const PkBotKnobs *k, uint64_t *rng)
{
    /* the candidates, greedy's choice first, then plays by greedy's points,
     * then draw, then pass: the order ties fall back on */
    int cand[MC_MAX_CANDS], key[MC_MAX_CANDS], nc = 0;
    int gpick = pk_bot_greedy(g, seat, m, n);
    int rsuit = (int)below(rng, PK_SUITS);
    int any_suited = 0, any_play = 0;
    for (int i = 0; i < n; i++)
        if (m[i].kind == PK_A_PLAY) {
            any_play = 1;
            if (!pk_is_wild(g->hand[seat][m[i].a])) any_suited = 1;
        }
    for (int i = 0; i < n && nc < MC_MAX_CANDS; i++) {
        PkAct a = m[i];
        if ((k->flags & PK_KNOB_RANDOM_WILD) && a.kind == PK_A_PLAY && a.b < PK_SUITS && a.b != rsuit)
            continue;
        int dup = 0;
        for (int j = 0; j < nc && !dup; j++) dup = same_move(g, seat, a, m[cand[j]]);
        if (dup) continue;
        int kk = a.kind == PK_A_PLAY ? play_points(g, seat, a) : a.kind == PK_A_DRAW ? -100000 : -200000;
        if (i == gpick) kk = 1 << 30;
        /* insertion, stable: equal keys keep menu order */
        int at = nc;
        while (at > 0 && key[at - 1] < kk) { cand[at] = cand[at - 1]; key[at] = key[at - 1]; at--; }
        cand[at] = i; key[at] = kk; nc++;
    }
    /* the ablation's random suit may have been merged away from greedy's pick */
    if (nc == 1) return cand[0];

    PkBelief b;
    if (!pk_belief_build(&b, g, seat)) return gpick;
    if (k->flags & PK_KNOB_NO_BELIEF) {
        memset(b.hard_n, 0, sizeof b.hard_n);
        memset(b.soft_n, 0, sizeof b.soft_n);
    }

    double score[MC_MAX_CANDS], tax[MC_MAX_CANDS];
    int nsim[MC_MAX_CANDS], alive[MC_MAX_CANDS];
    for (int i = 0; i < nc; i++) {
        score[i] = 0.0; nsim[i] = 0; alive[i] = 1;
        PkAct a = m[cand[i]];
        tax[i] = 0.0;
        if (a.kind == PK_A_PLAY && pk_is_wild(g->hand[seat][a.a]) && any_suited && g->hand_n[seat] > 1)
            tax[i] = (double)k->wild_keep / 1000.0;
        if (a.kind == PK_A_DRAW && any_play) tax[i] = (double)k->draw_keep / 1000.0;
    }

    uint64_t base = mix64(pk_bot_rand(rng) ^ k->seed ^ ((uint64_t)seat * UINT64_C(0x9e3779b97f4a7c15)));
    PkGame world, trial;
    int W[3] = { k->w1, k->w2, k->w3 }, w = 0;
    for (int stage = 0; stage < 3; stage++) {
        for (int wi = 0; wi < W[stage]; wi++, w++) {
            uint64_t ws = mix64(base + (uint64_t)(w + 1) * UINT64_C(0xd1b54a32d192ed03));
            int use_soft = k->soft_mod && (w % k->soft_mod) != k->soft_mod - 1;
            pk_belief_sample(&b, g, ws, use_soft, &world);
            for (int ci = 0; ci < nc; ci++) {
                if (!alive[ci]) continue;
                lite_copy(&trial, &world);          /* the same world for every candidate */
                double v = 0.0;
                if (pk_apply(&trial, seat, m[cand[ci]])) {
                    if (m[cand[ci]].kind != PK_A_DRAW && pk_can_seal(&trial)) pk_seal(&trial);
                    v = rollout(&trial, seat, k->depth);
                }
                score[ci] += v;
                nsim[ci]++;
            }
        }
        if (stage == 2) break;
        int n_alive = 0;
        for (int i = 0; i < nc; i++) n_alive += alive[i];
        int keep = stage == 0 ? (nc / 3 < 3 ? 3 : nc / 3) : 2;
        for (int drop = n_alive - keep; drop > 0; drop--) {
            int worst = -1;
            double wv = 1e30;
            for (int i = 0; i < nc; i++) {
                if (!alive[i]) continue;
                double v = score[i] / (double)(nsim[i] ? nsim[i] : 1) - tax[i];
                /* <= : among the tied worst drop the LAST, so the greedier
                 * candidate survives a tie (octogen's tie-break inversion) */
                if (v <= wv) { wv = v; worst = i; }
            }
            if (worst < 0) break;
            alive[worst] = 0;
        }
    }
    int best = -1;
    double bv = -1e30;
    for (int i = 0; i < nc; i++) {
        if (!alive[i] || !nsim[i]) continue;
        double v = score[i] / (double)nsim[i] - tax[i];
        if (v > bv) { bv = v; best = i; }
    }
    return best >= 0 ? cand[best] : gpick;
}

/* ---- choose ------------------------------------------------------------------- */

int pk_bot_choose(const PkGame *g, int seat, int strategy, const PkBotKnobs *k,
                  uint64_t *rng, PkBotMove *out)
{
    memset(out, 0, sizeof *out);
    out->seat = (uint8_t)seat;
    if (seat < 0 || seat >= g->n) return 0;
    if (g->b_open && g->b_sender != seat) return 0;
    if (g->over) {
        if (g->b_open && pk_can_seal(g)) { out->what = PK_BOT_SEAL; return 1; }
        return 0;
    }

    PkAct m[PK_BOT_MENU_CAP];
    int n = pk_legal(g, seat, m, PK_BOT_MENU_CAP);
    int nt = pk_legal_turn(g, seat, m, PK_BOT_MENU_CAP);

    /* D60: say it at once; call only a seat the counting proves exposed.
     * `exposed` is a function of public history alone: the counts (seven,
     * plus draws, less plays), the says and the windows. */
    uint8_t proven = g->b_open ? (uint8_t)(g->exposed & g->b_exposed_at_open) : g->exposed;
    for (int i = nt; i < n; i++)
        if (m[i].kind == PK_A_SAY_IT) { out->what = PK_BOT_ACT; out->act = m[i]; return 1; }
    for (int i = nt; i < n; i++)
        if (m[i].kind == PK_A_CALL_OUT && (proven >> m[i].a & 1)) {
            out->what = PK_BOT_ACT; out->act = m[i]; return 1;
        }

    if (nt > 0) {
        int pick;
        switch (strategy) {
        case PK_BOT_GREEDY: pick = pk_bot_greedy(g, seat, m, nt); break;
        case PK_BOT_MC:     pick = nt == 1 ? 0 : mc_pick(g, seat, m, nt, k, rng); break;
        default:            pick = (int)below(rng, (uint32_t)nt); break;
        }
        out->what = PK_BOT_ACT;
        out->act = m[pick];
        return 1;
    }
    if (g->b_open && pk_can_seal(g)) { out->what = PK_BOT_SEAL; return 1; }
    return 0;
}

int pk_bot_apply(PkGame *g, const PkBotMove *m)
{
    if (m->what == PK_BOT_SEAL) return pk_seal(g);
    if (m->what == PK_BOT_ACT) return pk_apply(g, m->seat, m->act);
    return 0;
}

int pk_bot_bubble(PkGame *g, int seat, int strategy, const PkBotKnobs *k, uint64_t *rng)
{
    int done = 0;
    for (int guard = 0; guard < 4 * PK_MAX_ACTIONS; guard++) {
        PkBotMove mv;
        if (!pk_bot_choose(g, seat, strategy, k, rng, &mv)) return done;
        if (!pk_bot_apply(g, &mv)) return -1;
        done++;
        if (mv.what == PK_BOT_SEAL) return done;
    }
    return -1;
}

static int last_sender(const PkGame *g)
{
    for (int i = g->hist_n - 1; i >= 0; i--)
        if (g->hist[i].kind == PK_A_BUBBLE) return g->hist[i].a;
    return -1;
}

int pk_bot_round(PkGame *g, const uint8_t strategy[PK_MAX_SEATS],
                 const PkBotKnobs knobs[PK_MAX_SEATS], uint64_t *rng)
{
    if (g->over) return 0;
    int from = last_sender(g);
    if (from < 0) from = g->turn;
    for (int j = 0; j < g->n && !g->over; j++) {
        int s = pk_next(g, from, j);
        if (s == g->turn) continue;
        if (pk_bot_bubble(g, s, strategy[s], &knobs[s], rng) < 0) return -1;
    }
    if (g->over) return 0;
    int t = g->turn;
    int r = pk_bot_bubble(g, t, strategy[t], &knobs[t], rng);
    if (r < 0) return -1;
    if (r == 0) return -1;       /* the turn seat always has something to do */
    return g->over ? 0 : 1;
}
