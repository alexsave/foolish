// Bot Cup entry for team "opus".
// Determinized Monte Carlo (PIMC) over a fast handwritten rollout policy.
// See NOTES.md for the approach.
#include "botcup.h"
#include <string.h>
#include <time.h>
#include <stddef.h>
#include <math.h>
#ifdef OP_STATS
#include <stdio.h>
static long op_st_roll, op_st_dec, op_st_worlds; static double op_st_ms;
__attribute__((destructor)) static void op_st_print(void) { if (op_st_dec) fprintf(stderr, "decisions %ld rollouts %ld worlds/dec %.1f rollouts/ms %.1f\n", op_st_dec, op_st_roll, (double)op_st_worlds / op_st_dec, op_st_roll / op_st_ms); }
#define OP_ST(x) x
#else
#define OP_ST(x)
#endif

// ---------------------------------------------------------------- tunables
#ifndef OP_BUDGET_MS
#define OP_BUDGET_MS 80.0
#endif
#ifndef OP_TARGET_MEAN
#define OP_TARGET_MEAN 28.0
#endif
#ifndef OP_MAX_WORLDS
#define OP_MAX_WORLDS 100000
#endif
#ifndef OP_MULTI
#define OP_MULTI 5.0
#endif
#ifndef OP_THROW_V
#define OP_THROW_V 13
#endif
#ifndef OP_THROW_TV
#define OP_THROW_TV 0
#endif
#ifndef OP_THROW_TE
#define OP_THROW_TE 13
#endif
#ifndef OP_PASS
#define OP_PASS 1
#endif
#ifndef OP_PASS1
#define OP_PASS1 0
#endif
#ifndef OP_PASST
#define OP_PASST 100
#endif
#ifndef OP_SMART
#define OP_SMART 0
#endif
#ifndef OP_SMART_DL
#define OP_SMART_DL 0
#endif
#ifndef OP_SMART_W
#define OP_SMART_W 4.0
#endif
#ifndef OP_TAKE
#define OP_TAKE 1
#endif
#ifndef OP_CHECK0
#define OP_CHECK0 16
#endif
#ifndef OP_PRUNE_Z
#define OP_PRUNE_Z 3.0
#endif
#ifndef OP_INFER
#define OP_INFER 1
#endif
#ifndef OP_INFER_Q
#define OP_INFER_Q 0.85
#endif
#ifndef OP_USE_MC
#define OP_USE_MC 1
#endif

// ---------------------------------------------------------------- rng
static uint64_t op_rs = 0x9E3779B97F4A7C15ull;
static inline uint32_t op_rand(void) {
    op_rs ^= op_rs << 13; op_rs ^= op_rs >> 7; op_rs ^= op_rs << 17;
    return (uint32_t)(op_rs >> 16);
}
static inline int op_randn(int n) { return (int)(op_rand() % (uint32_t)n); }

static double op_now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
}

// ---------------------------------------------------------------- cards
static inline int op_cost(Card c, int trump) {
    return (c.value - 5) + (c.suit == trump ? 10 : 0);
}

// A move built by the policy (no MAX_MOVE_CARDS cap).
typedef struct {
    int  type;
    int  n;
    Card cards[MAX_BATTLES];
    Card att[MAX_BATTLES];
} OpMove;

#ifdef OP_DIFFTEST
#include <stdio.h>
static long op_dt_n, op_dt_bad;
__attribute__((destructor)) static void op_dt_print(void) { fprintf(stderr, "difftest rollouts %ld mismatches %ld\n", op_dt_n, op_dt_bad); }
// Engine-side copy of the policy, kept only for the differential test.
static inline int op_deck_left(const Game *g) {
    return g->deck_count + (g->has_flipped ? 1 : 0);
}
// ---------------------------------------------------------------- cover search
// Minimum-cost assignment of distinct hand cards to the uncovered attacks.
typedef struct {
    int  k;
    Card att[MAX_BATTLES];
    int  nopt[MAX_BATTLES];
    int8_t opt[MAX_BATTLES][MAX_HAND_SIZE];   // hand indices, cheapest first
    int  cost[MAX_HAND_SIZE];
    int  lb[MAX_BATTLES + 1];                 // sum of min costs of attacks d..k-1
    int  best, cur, nodes;
    int  pick[MAX_BATTLES], bestpick[MAX_BATTLES];
    bool used[MAX_HAND_SIZE];
} OpCov;

static void op_cov_dfs(OpCov *x, int d) {
    if (x->cur + x->lb[d] >= x->best) return;
    if (d == x->k) {
        x->best = x->cur;
        memcpy(x->bestpick, x->pick, sizeof(int) * (size_t)x->k);
        return;
    }
    if (++x->nodes > 3000) return;
    for (int i = 0; i < x->nopt[d]; i++) {
        int j = x->opt[d][i];
        if (x->used[j]) continue;
        if (x->cur + x->cost[j] + x->lb[d + 1] >= x->best) break;
        x->used[j] = true; x->pick[d] = j; x->cur += x->cost[j];
        op_cov_dfs(x, d + 1);
        x->cur -= x->cost[j]; x->used[j] = false;
    }
}

// Minimum-cost full cover of the uncovered attacks. Returns cost or -1.
static int op_min_cover(const Game *g, int seat, OpMove *mv) {
    OpCov x;
    const Player *p = &g->players[seat];
    int trump = g->power_suit;
    x.k = 0;
    for (int i = 0; i < g->num_battles; i++)
        if (card_is_none(g->table_battles[i].defense)) x.att[x.k++] = g->table_battles[i].attack;
    if (x.k == 0 || x.k > p->hand_count) return -1;
    for (int j = 0; j < p->hand_count; j++) x.cost[j] = op_cost(p->hand[j], trump);
    // hardest (highest) attacks first
    for (int i = 1; i < x.k; i++) {
        Card c = x.att[i]; int ci = op_cost(c, trump); int t = i;
        while (t > 0 && op_cost(x.att[t - 1], trump) < ci) { x.att[t] = x.att[t - 1]; t--; }
        x.att[t] = c;
    }
    for (int d = 0; d < x.k; d++) {
        int n = 0;
        for (int j = 0; j < p->hand_count; j++) {
            if (!can_cover(x.att[d], p->hand[j], trump)) continue;
            int t = n++;
            while (t > 0 && x.cost[x.opt[d][t - 1]] > x.cost[j]) { x.opt[d][t] = x.opt[d][t - 1]; t--; }
            x.opt[d][t] = (int8_t)j;
        }
        if (n == 0) return -1;
        x.nopt[d] = n;
    }
    x.lb[x.k] = 0;
    for (int d = x.k - 1; d >= 0; d--) x.lb[d] = x.lb[d + 1] + x.cost[x.opt[d][0]];
    memset(x.used, 0, sizeof(bool) * (size_t)p->hand_count);
    // greedy bound
    x.best = 1 << 30;
    {
        int c = 0; bool ok = true;
        for (int d = 0; d < x.k && ok; d++) {
            int i = 0;
            while (i < x.nopt[d] && x.used[x.opt[d][i]]) i++;
            if (i == x.nopt[d]) { ok = false; break; }
            int j = x.opt[d][i];
            x.used[j] = true; x.pick[d] = j; c += x.cost[j];
        }
        memset(x.used, 0, sizeof(bool) * (size_t)p->hand_count);
        if (ok) { x.best = c; memcpy(x.bestpick, x.pick, sizeof(int) * (size_t)x.k); }
    }
    if (x.best != x.lb[0]) { x.cur = 0; x.nodes = 0; op_cov_dfs(&x, 0); }
    if (x.best >= (1 << 30)) return -1;
    if (mv) {
        mv->type = MOVE_COVER; mv->n = x.k;
        for (int i = 0; i < x.k; i++) { mv->cards[i] = p->hand[x.bestpick[i]]; mv->att[i] = x.att[i]; }
    }
    return x.best;
}

// ---------------------------------------------------------------- policy
static void op_policy_first(const Game *g, int seat, OpMove *mv) {
    const Player *p = &g->players[seat];
    int trump = g->power_suit;
    int cap = g->players[g->defender].hand_count;
    int dl = op_deck_left(g);
    // whole hand same value and fits: go out
    bool same = p->hand_count > 0;
    for (int i = 1; i < p->hand_count; i++) if (p->hand[i].value != p->hand[0].value) same = false;
    if (same && p->hand_count <= cap) {
        mv->type = MOVE_ATTACK; mv->n = p->hand_count;
        for (int i = 0; i < p->hand_count; i++) mv->cards[i] = p->hand[i];
        return;
    }
    double best = 1e9; int bestv = -1; bool best_trump_only = false;
    for (int v = 1; v <= ACE_VALUE; v++) {
        int nnt = 0;
        for (int i = 0; i < p->hand_count; i++) if (p->hand[i].value == v) {
            if (p->hand[i].suit != trump) nnt++;
        }
        if (nnt == 0) continue;
        int n = nnt < cap ? nnt : cap;
        double s = (v - 5) - OP_MULTI * (n - 1);
        if (dl == 0) s -= 0.5 * (n - 1);
        if (OP_SMART && dl <= OP_SMART_DL) {
            const Player *d = &g->players[g->defender];
            int unbeat = 0;
            for (int i = 0; i < p->hand_count; i++) if (p->hand[i].value == v && p->hand[i].suit != trump) {
                bool cov = false;
                for (int j = 0; j < d->hand_count; j++) if (!botcup_is_hidden(d->hand[j]) && can_cover(p->hand[i], d->hand[j], trump)) { cov = true; break; }
                if (!cov) unbeat++;
            }
            s -= OP_SMART_W * unbeat;
        }
        if (s < best) { best = s; bestv = v; }
    }
    if (bestv < 0) {
        // only trumps: lowest trump
        int bi = 0;
        for (int i = 1; i < p->hand_count; i++) if (p->hand[i].value < p->hand[bi].value) bi = i;
        mv->type = MOVE_ATTACK; mv->n = 1; mv->cards[0] = p->hand[bi];
        best_trump_only = true;
        (void)best_trump_only;
        return;
    }
    mv->type = MOVE_ATTACK; mv->n = 0;
    for (int i = 0; i < p->hand_count && mv->n < cap; i++)
        if (p->hand[i].value == bestv && p->hand[i].suit != trump) mv->cards[mv->n++] = p->hand[i];
}

// Returns false for "good".
static bool op_policy_throw(const Game *g, int seat, OpMove *mv) {
    const Player *p = &g->players[seat];
    int trump = g->power_suit;
    int dl = op_deck_left(g);
    bool tv[16] = { false };
    int unc = 0;
    for (int i = 0; i < g->num_battles; i++) {
        const Battle *b = &g->table_battles[i];
        card_mark_value(tv, b->attack.value);
        if (!card_is_none(b->defense)) card_mark_value(tv, b->defense.value); else unc++;
    }
    int room = g->players[g->defender].hand_count - unc;
    if (room <= 0) return false;
    Card cand[MAX_HAND_SIZE]; int nc = 0; int nall = 0;
    for (int i = 0; i < p->hand_count; i++) {
        Card c = p->hand[i];
        if (!card_has_value(tv, c.value)) continue;
        nall++;
        bool ok;
        if (dl > 0) ok = (c.suit != trump && c.value <= OP_THROW_V + (dl < 6 ? 1 : 0)) || (c.suit == trump && c.value <= OP_THROW_TV);
        else ok = c.suit != trump || c.value <= OP_THROW_TE;
        if (ok) cand[nc++] = c;
    }
    // can empty hand entirely in the endgame: dump everything
    if (dl == 0 && nall == p->hand_count && nall <= room) {
        nc = 0;
        for (int i = 0; i < p->hand_count; i++) cand[nc++] = p->hand[i];
    }
    if (nc == 0) return false;
    // cheapest first
    for (int i = 1; i < nc; i++) {
        Card c = cand[i]; int ci = op_cost(c, trump); int t = i;
        while (t > 0 && op_cost(cand[t - 1], trump) > ci) { cand[t] = cand[t - 1]; t--; }
        cand[t] = c;
    }
    if (nc > room) nc = room;
    mv->type = MOVE_ATTACK; mv->n = nc;
    for (int i = 0; i < nc; i++) mv->cards[i] = cand[i];
    return true;
}

static void op_policy_defend(const Game *g, int seat, OpMove *mv) {
    const Player *p = &g->players[seat];
    int trump = g->power_suit;
    int dl = op_deck_left(g);
    // pass?
    bool any_cov = false, same = g->num_battles > 0;
    int v0 = g->table_battles[0].attack.value;
    for (int i = 0; i < g->num_battles; i++) {
        if (!card_is_none(g->table_battles[i].defense)) any_cov = true;
        if (g->table_battles[i].attack.value != v0) same = false;
    }
    OpMove cov;
    int ccost = op_min_cover(g, seat, &cov);
    if (game_pass_allowed(g) && !any_cov && same) {
        int next = get_next_player_index(g, seat);
        int cap = g->players[next].hand_count - g->num_battles;
        if (cap > 0) {
            Card nt[8]; int nnt = 0; Card tr[8]; int ntr = 0;
            for (int i = 0; i < p->hand_count; i++) if (p->hand[i].value == v0) {
                if (p->hand[i].suit == trump) { if (ntr < 8) tr[ntr++] = p->hand[i]; }
                else if (nnt < 8) nt[nnt++] = p->hand[i];
            }
            if (OP_PASS && nnt > 0) {
                mv->type = MOVE_PASS; mv->n = nnt < cap ? nnt : cap;
                if (OP_PASS1) mv->n = 1;
                for (int i = 0; i < mv->n; i++) mv->cards[i] = nt[i];
                return;
            }
            if (OP_PASS && ntr > 0 && (ccost < 0 || ccost >= op_cost(tr[0], trump) + OP_PASST)) {
                mv->type = MOVE_PASS; mv->n = 1; mv->cards[0] = tr[0];
                return;
            }
        }
    }
    if (ccost >= 0) {
        bool take = false;
        if (dl > 0) {
            int ntr = 0, trv = 0;
            for (int i = 0; i < cov.n; i++) if (cov.cards[i].suit == trump) { ntr++; trv += cov.cards[i].value - 5; }
            int tab = 0;
            for (int i = 0; i < g->num_battles; i++) tab += 1 + !card_is_none(g->table_battles[i].defense);
            // taking a few low cards early can be cheaper than burning high trumps
            if (OP_TAKE && dl > 10 && ntr >= 2 && tab <= 3) take = true;
            if (OP_TAKE && dl > 14 && ntr >= 1 && trv >= 6 && tab <= 2) take = true;
        }
        if (!take) { *mv = cov; return; }
    }
    mv->type = MOVE_PICKUP; mv->n = 0;
}

static bool op_apply(Game *g, int seat, const OpMove *mv) {
    switch (mv->type) {
        case MOVE_ATTACK: return handle_attack(g, seat, mv->cards, mv->n);
        case MOVE_COVER:  return handle_cover(g, seat, mv->cards, mv->att, mv->n);
        case MOVE_PASS:   return handle_pass(g, seat, mv->cards, mv->n);
        case MOVE_PICKUP: return handle_pickup(g, seat);
        case MOVE_GOOD:   return handle_good(g, seat);
    }
    return false;
}

// One policy step for `seat` (which must be eligible to act).
static bool op_step(Game *g, int seat) {
    OpMove mv;
    if (g->num_battles == 0) {
        op_policy_first(g, seat, &mv);
        if (op_apply(g, seat, &mv)) return true;
        Card c = g->players[seat].hand[0];
        return handle_attack(g, seat, &c, 1);
    }
    if (seat == g->defender) {
        op_policy_defend(g, seat, &mv);
        if (op_apply(g, seat, &mv)) return true;
        return handle_pickup(g, seat);
    }
    if (op_policy_throw(g, seat, &mv) && op_apply(g, seat, &mv)) return true;
    return handle_good(g, seat);
}

// Points for `me` once the rollout stops (n-1 for first out .. 0 for fool).
static double op_score(const Game *g, int me) {
    int n = g->num_players;
    for (int i = 0; i < g->num_eliminated; i++)
        if (g->elimination_order[i] == me) return (double)(n - 1 - i);
    if (game_done(g) == me) return 0.0;
    // unfinished: rank remaining by hand count
    int rem = n - g->num_eliminated, better = 0;
    for (int p = 0; p < n; p++) {
        if (p == me || g->players[p].status != PLAYER_STATUS_IN) continue;
        if (g->players[p].hand_count < g->players[me].hand_count) better++;
    }
    return (double)(rem - 1 - better) * 0.9;
}

static double op_rollout(Game *g, int me) {
    for (int it = 0; it < 800; it++) {
        if (game_done(g) >= 0) break;
        if (g->players[me].status != PLAYER_STATUS_IN) break;
        int el[MAX_PLAYERS], ne = 0;
        for (int i = 0; i < g->num_players; i++) if (should_bot_act(g, i)) el[ne++] = i;
        if (ne == 0) break;
        int s = el[ne == 1 ? 0 : op_randn(ne)];
        for (int q = 0; q < g->num_players; q++) {
            Player *pl = &g->players[q];
            for (int i = 1; i < pl->hand_count; i++) {
                Card c = pl->hand[i]; int j = i;
                while (j > 0 && card_to_id(pl->hand[j - 1]) > card_to_id(c)) { pl->hand[j] = pl->hand[j - 1]; j--; }
                pl->hand[j] = c;
            }
        }
        if (!op_step(g, s)) break;
    }
    return op_score(g, me);
}

#endif

// ---------------------------------------------------------------- fast simulator
// A bitmask copy of the engine rules (game.c) for rollouts. It mirrors
// handle_attack / handle_cover / handle_pass / handle_pickup / handle_good,
// the refill order and get_next_player_index exactly; a differential test
// (OP_DIFFTEST) plays engine and simulator side by side and compares.
//
// Cards live in "rank positions": bit k of a mask is the k-th cheapest card
// of the deck by (cost, engine id), so the lowest set bit of any mask is its
// cheapest card and iterating a mask walks it in cost order.
#define SIM_MAXB 40
typedef struct {
    int8_t   n, trump, pass_ok;
    int8_t   fa, def;
    int8_t   nb, nunc;
    int8_t   nelim;
    int8_t   in[MAX_PLAYERS];
    int8_t   cnt[MAX_PLAYERS];
    int8_t   elim[MAX_PLAYERS];
    int8_t   deck_n, dpos;
    uint32_t good;
    uint64_t hand[MAX_PLAYERS];
    int8_t   att[SIM_MAXB], dfn[SIM_MAXB];   // positions; dfn = -1 when uncovered
    int8_t   deck[64];
} Sim;

// Per-trump tables.
typedef struct {
    int      trump, minv, ready, npos;
    int8_t   pos2id[64], id2pos[64];
    int8_t   cost[64], val[64];
    uint64_t beat[64];               // cards that cover the card at this position
    uint64_t valmask[16];
    uint64_t trumps;
} SimTab;
static SimTab sim_tab;

static void sim_tab_init(int trump, int minv) {
    SimTab *t = &sim_tab;
    if (t->ready && t->trump == trump && t->minv == minv) return;
    memset(t, 0, sizeof *t);
    t->trump = trump; t->minv = minv; t->ready = 1;
    int8_t cid[64]; bool valid[64] = { false };
    for (int s = 0; s < NUM_SUITS; s++) for (int v = minv; v <= ACE_VALUE; v++) {
        int id = s * 13 + v - 1;
        valid[id] = true;
        cid[id] = (int8_t)((v - 5) + (s == trump ? 10 : 0));
    }
    int n = 0;
    for (int c = -10; c <= 30; c++)
        for (int id = 0; id < 52; id++)
            if (valid[id] && cid[id] == c) { t->pos2id[n] = (int8_t)id; t->id2pos[id] = (int8_t)n; n++; }
    t->npos = n;
    for (int k = 0; k < n; k++) {
        int id = t->pos2id[k];
        t->cost[k] = cid[id];
        t->val[k] = (int8_t)(id % 13 + 1);
        t->valmask[t->val[k]] |= 1ull << k;
        if (id / 13 == trump) t->trumps |= 1ull << k;
    }
    for (int a = 0; a < n; a++) {
        Card ca = card_of_id(t->pos2id[a]);
        for (int d = 0; d < n; d++)
            if (can_cover(ca, card_of_id(t->pos2id[d]), trump)) t->beat[a] |= 1ull << d;
    }
}

static inline int sim_left(const Sim *s) { return s->deck_n - s->dpos; }
static inline int sim_lowest(uint64_t m) { return __builtin_ctzll(m); }

static int sim_next(const Sim *s, int cur) {
    int inc = 0;
    for (int i = 0; i < s->n; i++) inc += s->in[i];
    if (inc <= 1) return cur;
    int nx = (cur + 1) % s->n;
    while (!s->in[nx]) nx = (nx + 1) % s->n;
    return nx;
}

static inline void sim_out(Sim *s, int p) {
    s->in[p] = 0;
    s->elim[s->nelim++] = (int8_t)p;
}

static void sim_draw6(Sim *s, int p) {
    while (s->cnt[p] < CARDS_PER_PLAYER && s->dpos < s->deck_n) {
        s->hand[p] |= 1ull << s->deck[s->dpos++];
        s->cnt[p]++;
    }
    if (s->cnt[p] == 0 && s->in[p]) sim_out(s, p);
}

static void sim_refill(Sim *s) {
    if (sim_left(s) == 0) {
        for (int i = 0; i < s->n; i++) if (s->cnt[i] == 0 && s->in[i]) sim_out(s, i);
        return;
    }
    int def = s->def, p = s->fa;
    bool visited[MAX_PLAYERS] = { false };
    do {
        if (visited[p]) break;
        visited[p] = true;
        if (p != def) sim_draw6(s, p);
        p = sim_next(s, p);
    } while (p != s->fa);
    sim_draw6(s, def);
}

static void sim_attack(Sim *s, int p, const int8_t *c, int k) {
    for (int i = 0; i < k; i++) {
        s->hand[p] &= ~(1ull << c[i]);
        s->att[s->nb] = c[i]; s->dfn[s->nb] = -1; s->nb++;
    }
    s->cnt[p] = (int8_t)(s->cnt[p] - k);
    s->nunc = (int8_t)(s->nunc + k);
    s->good = 0;
    if (s->cnt[p] == 0 && sim_left(s) == 0) sim_out(s, p);
}

// cov[i] covers the uncovered battle whose attack is atk[i].
static void sim_cover(Sim *s, int p, const int8_t *cov, const int8_t *atk, int k) {
    s->good = 0;
    for (int i = 0; i < k; i++) {
        for (int j = 0; j < s->nb; j++) if (s->dfn[j] < 0 && s->att[j] == atk[i]) { s->dfn[j] = cov[i]; break; }
        s->hand[p] &= ~(1ull << cov[i]);
    }
    s->cnt[p] = (int8_t)(s->cnt[p] - k);
    s->nunc = (int8_t)(s->nunc - k);
    if (s->cnt[p] == 0) {
        s->nb = 0; s->nunc = 0;
        sim_refill(s);
        s->fa = s->def;
        s->good = 0;
        if (s->cnt[s->fa] == 0) {
            bool was_in = s->in[s->fa];
            s->in[s->fa] = 0;
            if (was_in) s->elim[s->nelim++] = s->fa;
            s->fa = (int8_t)sim_next(s, s->fa);
        }
        s->def = (int8_t)sim_next(s, s->fa);
    }
}

static void sim_pass(Sim *s, int p, const int8_t *c, int k) {
    int nx = sim_next(s, s->def);
    for (int i = 0; i < k; i++) {
        s->hand[p] &= ~(1ull << c[i]);
        s->att[s->nb] = c[i]; s->dfn[s->nb] = -1; s->nb++;
    }
    s->cnt[p] = (int8_t)(s->cnt[p] - k);
    s->nunc = (int8_t)(s->nunc + k);
    s->good = 0;
    if (sim_left(s) == 0 && s->cnt[p] == 0) sim_out(s, p);
    s->def = (int8_t)nx;
}

static void sim_pickup(Sim *s, int p) {
    for (int i = 0; i < s->nb; i++) {
        s->hand[p] |= 1ull << s->att[i]; s->cnt[p]++;
        if (s->dfn[i] >= 0) { s->hand[p] |= 1ull << s->dfn[i]; s->cnt[p]++; }
    }
    s->nb = 0; s->nunc = 0;
    sim_refill(s);
    s->fa = (int8_t)sim_next(s, s->def);
    s->def = (int8_t)sim_next(s, s->fa);
    s->good = 0;
}

static void sim_good(Sim *s, int p) {
    s->good |= 1u << p;
    bool all_good = true; int na = 0;
    for (int i = 0; i < s->n; i++) if (i != s->def && s->in[i]) {
        na++;
        if (!(s->good >> i & 1)) all_good = false;
    }
    if (na == 0) all_good = false;
    if (all_good && s->nb > 0 && s->nunc == 0) {
        s->nb = 0; s->nunc = 0;
        sim_refill(s);
        s->fa = s->def;
        s->def = (int8_t)sim_next(s, s->fa);
        s->good = 0;
    }
}

static inline bool sim_may_act(const Sim *s, int i) {
    if (!s->in[i]) return false;
    if (s->nb == 0) return i == s->fa;
    if (i == s->def) return s->nunc > 0;
    return !(s->good >> i & 1);
}

static inline int sim_done(const Sim *s) {
    int inc = 0, last = -1;
    for (int i = 0; i < s->n; i++) if (s->in[i]) { inc++; last = i; }
    return inc == 1 ? last : -1;
}

static inline int8_t sim_pos(Card c) { return sim_tab.id2pos[card_to_id(c)]; }

// Build from a Game (a full world, or a view: then other hands are empty
// masks with their real counts, and the deck is unknown).
static void sim_from_game(Sim *s, const Game *g) {
    s->n = g->num_players; s->trump = g->power_suit; s->pass_ok = game_pass_allowed(g);
    s->fa = g->first_attacker; s->def = g->defender;
    s->good = g->good_players_mask;
    s->nelim = g->num_eliminated;
    for (int i = 0; i < g->num_eliminated; i++) s->elim[i] = g->elimination_order[i];
    for (int p = 0; p < g->num_players; p++) {
        const Player *pl = &g->players[p];
        s->in[p] = pl->status == PLAYER_STATUS_IN;
        s->cnt[p] = pl->hand_count;
        uint64_t m = 0;
        for (int i = 0; i < pl->hand_count; i++)
            if (!botcup_is_hidden(pl->hand[i])) m |= 1ull << sim_pos(pl->hand[i]);
        s->hand[p] = m;
    }
    s->nb = 0; s->nunc = 0;
    for (int i = 0; i < g->num_battles && i < SIM_MAXB; i++) {
        s->att[i] = sim_pos(g->table_battles[i].attack);
        if (card_is_none(g->table_battles[i].defense)) { s->dfn[i] = -1; s->nunc++; }
        else s->dfn[i] = sim_pos(g->table_battles[i].defense);
        s->nb++;
    }
    int k = 0;
    for (int i = 0; i < g->deck_count; i++)
        s->deck[k++] = botcup_is_hidden(g->deck[i]) ? 0 : sim_pos(g->deck[i]);
    if (g->has_flipped) s->deck[k++] = sim_pos(g->flipped);
    s->deck_n = (int8_t)k; s->dpos = 0;
}

// ---- simulator policy (the same decisions as the engine policy above)
typedef struct {
    int    type, n;
    int8_t c[SIM_MAXB], a[SIM_MAXB];
} SimMove;

static void sim_pol_first(const Sim *s, int p, SimMove *mv) {
    const SimTab *t = &sim_tab;
    uint64_t h = s->hand[p];
    int cap = s->cnt[s->def];
    int dl = sim_left(s);
    mv->type = MOVE_ATTACK; mv->n = 0;
    if (!h) return;
    int v0 = t->val[sim_lowest(h)];
    if (s->cnt[p] <= cap && (h & ~t->valmask[v0]) == 0) {
        for (uint64_t m = h; m; m &= m - 1) mv->c[mv->n++] = (int8_t)sim_lowest(m);
        return;
    }
    double best = 1e9; int bestv = -1;
    uint64_t nth = h & ~t->trumps;
    for (int v = t->minv; v <= ACE_VALUE; v++) {
        uint64_t nt = nth & t->valmask[v];
        if (!nt) continue;
        int nnt = __builtin_popcountll(nt);
        int n = nnt < cap ? nnt : cap;
        double sc = (v - 5) - OP_MULTI * (n - 1);
        if (dl == 0) sc -= 0.5 * (n - 1);
        if (sc < best) { best = sc; bestv = v; }
    }
    if (bestv < 0) { mv->c[mv->n++] = (int8_t)sim_lowest(h); return; }
    for (uint64_t m = nth & t->valmask[bestv]; m && mv->n < cap; m &= m - 1)
        mv->c[mv->n++] = (int8_t)sim_lowest(m);
}

static bool sim_pol_throw(const Sim *s, int p, SimMove *mv) {
    const SimTab *t = &sim_tab;
    int room = s->cnt[s->def] - s->nunc;
    if (room <= 0) return false;
    uint64_t tv = 0;
    for (int i = 0; i < s->nb; i++) {
        tv |= t->valmask[t->val[s->att[i]]];
        if (s->dfn[i] >= 0) tv |= t->valmask[t->val[s->dfn[i]]];
    }
    uint64_t h = s->hand[p];
    uint64_t all = h & tv;
    if (!all) return false;
    int dl = sim_left(s);
    uint64_t ok;
    if (dl > 0) {
        int lim = OP_THROW_V + (dl < 6 ? 1 : 0);
        ok = 0;
        for (uint64_t m = all; m; m &= m - 1) {
            int k = sim_lowest(m), v = t->val[k];
            bool tr = (t->trumps >> k) & 1;
            if ((!tr && v <= lim) || (tr && v <= OP_THROW_TV)) ok |= 1ull << k;
        }
    } else {
        ok = all & ~t->trumps;
        for (uint64_t m = all & t->trumps; m; m &= m - 1)
            if (t->val[sim_lowest(m)] <= OP_THROW_TE) ok |= m & -m;
        if (__builtin_popcountll(all) == s->cnt[p] && s->cnt[p] <= room) ok = h;
    }
    if (!ok) return false;
    mv->type = MOVE_ATTACK; mv->n = 0;
    for (uint64_t m = ok; m && mv->n < room; m &= m - 1) mv->c[mv->n++] = (int8_t)sim_lowest(m);
    return true;
}

typedef struct {
    int k;
    int8_t att[SIM_MAXB];
    uint64_t opt[SIM_MAXB];
    int lb[SIM_MAXB + 1];
    int best, cur, nodes;
    int8_t pick[SIM_MAXB], bestpick[SIM_MAXB];
} SimCov;

static void sim_cov_dfs(SimCov *x, int d, uint64_t used) {
    if (x->cur + x->lb[d] >= x->best) return;
    if (d == x->k) {
        x->best = x->cur;
        memcpy(x->bestpick, x->pick, (size_t)x->k);
        return;
    }
    if (++x->nodes > 3000) return;
    for (uint64_t m = x->opt[d] & ~used; m; m &= m - 1) {
        int j = sim_lowest(m);
        int cj = sim_tab.cost[j];
        if (x->cur + cj + x->lb[d + 1] >= x->best) break;
        x->pick[d] = (int8_t)j; x->cur += cj;
        sim_cov_dfs(x, d + 1, used | (1ull << j));
        x->cur -= cj;
    }
}

static int sim_min_cover(const Sim *s, int p, SimMove *mv) {
    const SimTab *t = &sim_tab;
    SimCov x;
    x.k = 0;
    for (int i = 0; i < s->nb; i++) if (s->dfn[i] < 0) x.att[x.k++] = s->att[i];
    if (x.k == 0 || x.k > s->cnt[p]) return -1;
    for (int i = 1; i < x.k; i++) {
        int8_t c = x.att[i]; int ci = t->cost[c]; int j = i;
        while (j > 0 && t->cost[x.att[j - 1]] < ci) { x.att[j] = x.att[j - 1]; j--; }
        x.att[j] = c;
    }
    uint64_t h = s->hand[p];
    x.lb[x.k] = 0;
    for (int d = x.k - 1; d >= 0; d--) {
        x.opt[d] = h & t->beat[x.att[d]];
        if (!x.opt[d]) return -1;
        x.lb[d] = x.lb[d + 1] + t->cost[sim_lowest(x.opt[d])];
    }
    x.best = 1 << 30;
    {
        int c = 0; bool ok = true; uint64_t used = 0;
        for (int d = 0; d < x.k; d++) {
            uint64_t m = x.opt[d] & ~used;
            if (!m) { ok = false; break; }
            int j = sim_lowest(m);
            used |= 1ull << j; x.pick[d] = (int8_t)j; c += t->cost[j];
        }
        if (ok) { x.best = c; memcpy(x.bestpick, x.pick, (size_t)x.k); }
    }
    if (x.best != x.lb[0]) { x.cur = 0; x.nodes = 0; sim_cov_dfs(&x, 0, 0); }
    if (x.best >= (1 << 30)) return -1;
    if (mv) {
        mv->type = MOVE_COVER; mv->n = x.k;
        for (int i = 0; i < x.k; i++) { mv->c[i] = x.bestpick[i]; mv->a[i] = x.att[i]; }
    }
    return x.best;
}

static void sim_pol_defend(const Sim *s, int p, SimMove *mv) {
    const SimTab *t = &sim_tab;
    int dl = sim_left(s);
    bool can_pass = s->pass_ok && s->nunc == s->nb;
    int v0 = t->val[s->att[0]];
    if (can_pass) for (int i = 1; i < s->nb; i++) if (t->val[s->att[i]] != v0) { can_pass = false; break; }
    uint64_t tr = 0;
    int cap = 0;
    if (can_pass) {
        int nx = sim_next(s, p);
        cap = s->cnt[nx] - s->nb;
        uint64_t vm = s->hand[p] & t->valmask[v0];
        if (cap > 0 && vm) {
            uint64_t nt = vm & ~t->trumps;
            tr = vm & t->trumps;
            if (OP_PASS && nt) {
                mv->type = MOVE_PASS; mv->n = 0;
                int lim = OP_PASS1 ? 1 : cap;
                for (uint64_t m = nt; m && mv->n < lim; m &= m - 1) mv->c[mv->n++] = (int8_t)sim_lowest(m);
                return;
            }
        }
    }
    SimMove cov;
    int ccost = sim_min_cover(s, p, &cov);
    if (OP_PASS && tr && (ccost < 0 || ccost >= t->cost[sim_lowest(tr)] + OP_PASST)) {
        mv->type = MOVE_PASS; mv->n = 1; mv->c[0] = (int8_t)sim_lowest(tr);
        return;
    }
    if (ccost >= 0) {
        bool take = false;
        if (OP_TAKE && dl > 10) {
            int ntr = 0, trv = 0;
            for (int i = 0; i < cov.n; i++) if ((t->trumps >> cov.c[i]) & 1) { ntr++; trv += t->val[cov.c[i]] - 5; }
            int tab = s->nb + (s->nb - s->nunc);
            if (ntr >= 2 && tab <= 3) take = true;
            if (dl > 14 && ntr >= 1 && trv >= 6 && tab <= 2) take = true;
        }
        if (!take) { *mv = cov; return; }
    }
    mv->type = MOVE_PICKUP; mv->n = 0;
}

static void sim_apply(Sim *s, int p, const SimMove *mv) {
    switch (mv->type) {
        case MOVE_ATTACK: sim_attack(s, p, mv->c, mv->n); break;
        case MOVE_COVER:  sim_cover(s, p, mv->c, mv->a, mv->n); break;
        case MOVE_PASS:   sim_pass(s, p, mv->c, mv->n); break;
        case MOVE_PICKUP: sim_pickup(s, p); break;
        case MOVE_GOOD:   sim_good(s, p); break;
    }
}

static void sim_policy(const Sim *s, int p, SimMove *mv) {
    if (s->nb == 0) { sim_pol_first(s, p, mv); return; }
    if (p == s->def) { sim_pol_defend(s, p, mv); return; }
    if (!sim_pol_throw(s, p, mv)) { mv->type = MOVE_GOOD; mv->n = 0; }
}

static double sim_score(const Sim *s, int me) {
    int n = s->n;
    for (int i = 0; i < s->nelim; i++) if (s->elim[i] == me) return (double)(n - 1 - i);
    if (sim_done(s) == me) return 0.0;
    int rem = n - s->nelim, better = 0;
    for (int p = 0; p < n; p++) {
        if (p == me || !s->in[p]) continue;
        if (s->cnt[p] < s->cnt[me]) better++;
    }
    return (double)(rem - 1 - better) * 0.9;
}

static double sim_rollout(Sim *s, int me) {
    for (int it = 0; it < 800; it++) {
        if (sim_done(s) >= 0) break;
        if (!s->in[me]) break;
        int el[MAX_PLAYERS], ne = 0;
        for (int i = 0; i < s->n; i++) if (sim_may_act(s, i)) el[ne++] = i;
        if (ne == 0) break;
        int p = el[ne == 1 ? 0 : op_randn(ne)];
        SimMove mv;
        sim_policy(s, p, &mv);
        sim_apply(s, p, &mv);
    }
    return sim_score(s, me);
}

// ---------------------------------------------------------------- tracking
#define OP_LOC_UNK   (-1)
#define OP_LOC_TABLE (-2)
#define OP_LOC_DISC  (-3)

// Builds a sampled full-information world from the view.
typedef struct {
    int  n_opp_known[MAX_PLAYERS];
    Card opp_known[MAX_PLAYERS][MAX_HAND_SIZE];
    int  n_pool;
    Card pool[64];
    int  ncons[MAX_PLAYERS];          // endgame pickups of a single attack:
    Card cons[MAX_PLAYERS][16];       // the player holds nothing that beats these
} OpInfo;

static void op_track(const Game *v, int me, OpInfo *inf) {
    int8_t loc[64];
    for (int i = 0; i < 64; i++) loc[i] = OP_LOC_UNK;
    for (int p = 0; p < MAX_PLAYERS; p++) inf->ncons[p] = 0;
    int minv0 = min_value_for(v->num_players);
    int rem = 4 * (ACE_VALUE - minv0 + 1) - CARDS_PER_PLAYER * v->num_players;
    Card tab_att[64]; bool tab_cov[64]; int ntab = 0;
    for (int k = 0; k < v->num_logs; k++) {
        const GameLog *l = &v->logs[k];
        int p = l->player_idx;
        switch (l->log_type) {
            case LOG_ATTACK: case LOG_PASS:
                for (int i = 0; i < l->num_pairs; i++) {
                    Card c = l->pairs[i].primary;
                    if (c.suit >= 0 && c.value >= 1) {
                        loc[card_to_id(c)] = OP_LOC_TABLE;
                        if (ntab < 64) { tab_att[ntab] = c; tab_cov[ntab] = false; ntab++; }
                    }
                }
                break;
            case LOG_COVER:
                for (int i = 0; i < l->num_pairs; i++) {
                    Card c = l->pairs[i].primary;
                    if (c.suit >= 0 && c.value >= 1) loc[card_to_id(c)] = OP_LOC_TABLE;
                    for (int j = 0; j < ntab; j++) if (card_eq(tab_att[j], l->pairs[i].target)) tab_cov[j] = true;
                }
                break;
            case LOG_PICKUP:
                if (OP_INFER && p >= 0 && p != me && rem <= 0 && p < MAX_PLAYERS) {
                    int nu = 0; Card x = { 0, 0 };
                    for (int j = 0; j < ntab; j++) if (!tab_cov[j]) { nu++; x = tab_att[j]; }
                    if (nu == 1 && inf->ncons[p] < 16) {
                        bool known_beater = false;
                        for (int i = 0; i < 52; i++)
                            if (loc[i] == p && can_cover(x, card_of_id(i), v->power_suit)) known_beater = true;
                        if (!known_beater) inf->cons[p][inf->ncons[p]++] = x;
                    }
                }
                if (p >= 0) for (int i = 0; i < 52; i++) if (loc[i] == OP_LOC_TABLE) loc[i] = (int8_t)p;
                ntab = 0;
                break;
            case LOG_DISCARD:
                ntab = 0;
                for (int i = 0; i < 52; i++) if (loc[i] == OP_LOC_TABLE) loc[i] = OP_LOC_DISC;
                for (int i = 0; i < l->num_pairs; i++) {
                    Card c = l->pairs[i].primary;
                    if (c.suit >= 0 && c.value >= 1) loc[card_to_id(c)] = OP_LOC_DISC;
                }
                break;
            case LOG_DRAW:
                rem -= l->num_pairs;
                if (p >= 0) for (int i = 0; i < l->num_pairs; i++) {
                    Card c = l->pairs[i].primary;
                    if (!botcup_is_hidden(c) && c.suit >= 0 && c.value >= 1) loc[card_to_id(c)] = (int8_t)p;
                }
                break;
            default: break;
        }
    }
    // Ground truth overrides from the current view.
    bool vis[64] = { false };
    const Player *mp = &v->players[me];
    for (int i = 0; i < mp->hand_count; i++) vis[card_to_id(mp->hand[i])] = true;
    for (int i = 0; i < v->num_battles; i++) {
        vis[card_to_id(v->table_battles[i].attack)] = true;
        if (!card_is_none(v->table_battles[i].defense)) vis[card_to_id(v->table_battles[i].defense)] = true;
    }
    if (v->has_flipped) vis[card_to_id(v->flipped)] = true;
    for (int p = 0; p < MAX_PLAYERS; p++) inf->n_opp_known[p] = 0;
    inf->n_pool = 0;
    int minv = min_value_for(v->num_players);
    for (int s = 0; s < NUM_SUITS; s++) for (int val = minv; val <= ACE_VALUE; val++) {
        Card c; c.suit = (int8_t)s; c.value = (int8_t)val;
        int id = card_to_id(c);
        if (vis[id]) continue;
        int L = loc[id];
        if (L == OP_LOC_DISC) continue;
        if (L >= 0 && L != me && L < v->num_players && v->players[L].status == PLAYER_STATUS_IN
            && inf->n_opp_known[L] < v->players[L].hand_count) {
            inf->opp_known[L][inf->n_opp_known[L]++] = c;
            continue;
        }
        inf->pool[inf->n_pool++] = c;
    }
}

// Fills `w` (a copy of the view) with a sampled assignment of hidden cards.
static void op_sample(Game *w, const Game *v, int me, const OpInfo *inf) {
    memcpy(w, v, offsetof(Game, logs));
    Card pool[64]; int np = inf->n_pool;
    memcpy(pool, inf->pool, sizeof(Card) * (size_t)np);
    for (int i = np - 1; i > 0; i--) { int j = op_randn(i + 1); Card t = pool[i]; pool[i] = pool[j]; pool[j] = t; }
    int pi = 0;
    // Constrained players first: their unknown cards come from what they may hold.
    bool soft = OP_INFER && (op_rand() % 1000) < (uint32_t)(OP_INFER_Q * 1000);
    for (int pass = 0; pass < 2; pass++) {
        for (int p = 0; p < w->num_players; p++) {
            if (p == me) continue;
            bool constrained = soft && inf->ncons[p] > 0;
            if ((pass == 0) != constrained) continue;
            Player *pl = &w->players[p];
            int k = 0;
            for (int i = 0; i < inf->n_opp_known[p] && k < pl->hand_count; i++) pl->hand[k++] = inf->opp_known[p][i];
            if (constrained) {
                for (int i = pi; i < np && k < pl->hand_count; i++) {
                    bool ok = true;
                    for (int c = 0; c < inf->ncons[p] && ok; c++) if (can_cover(inf->cons[p][c], pool[i], w->power_suit)) ok = false;
                    if (ok) { Card t = pool[i]; pool[i] = pool[pi]; pool[pi] = t; pl->hand[k++] = pool[pi++]; }
                }
            }
            while (k < pl->hand_count) {
                if (pi < np) pl->hand[k++] = pool[pi++];
                else { pl->hand_count = (int8_t)k; break; }
            }
        }
    }
    int dk = 0;
    while (dk < w->deck_count && pi < np) w->deck[dk++] = pool[pi++];
    w->deck_count = (int16_t)dk;
    w->deterministic_deck = true;
    w->num_logs = 0;
    w->log_cap = 1;
    w->log_virt = 0;
}

// ---------------------------------------------------------------- root
static Game op_view_copy, op_world, op_sim;
static LegalMoves op_scratch;

static int op_move_cards_cost(const LegalMove *m, int trump) {
    int s = 0;
    for (int i = 0; i < m->n_cards; i++) s += op_cost(m->cards[i], trump);
    return s;
}

// Index of the menu entry equal to a simulator move, or -1.
static int op_find_sim(const LegalMoves *moves, const SimMove *mv) {
    uint64_t want = 0;
    for (int i = 0; i < mv->n; i++) want |= 1ull << sim_tab.pos2id[mv->c[i]];
    for (int i = 0; i < moves->n; i++) {
        const LegalMove *m = &moves->moves[i];
        if (m->type != mv->type) continue;
        if (mv->type == MOVE_PICKUP || mv->type == MOVE_GOOD) return i;
        if (m->n_cards != mv->n) continue;
        uint64_t have = 0;
        for (int b = 0; b < m->n_cards; b++) have |= 1ull << card_to_id(m->cards[b]);
        if (have != want) continue;
        if (mv->type == MOVE_COVER) {
            bool ok = true;
            for (int a = 0; a < mv->n && ok; a++)
                for (int b = 0; b < m->n_cards; b++)
                    if (card_to_id(m->cards[b]) == sim_tab.pos2id[mv->c[a]]) { ok = card_to_id(m->attack_cards[b]) == sim_tab.pos2id[mv->a[a]]; break; }
            if (!ok) continue;
        }
        return i;
    }
    return -1;
}

static int op_policy_index(const Game *v, int seat, const LegalMoves *moves) {
    static Sim s;
    sim_from_game(&s, v);
    SimMove mv;
    sim_policy(&s, seat, &mv);
    int i = op_find_sim(moves, &mv);
    return i >= 0 ? i : 0;
}

// Wall-clock accounting across every decision of this process, so the mean
// stays well under the limit even on a loaded machine.
static double op_tot_ms;
static long   op_tot_n;

static int op_choose_inner(const Game *view, int seat, const LegalMoves *moves, double budget);

static int op_choose(const Game *view, int seat, const LegalMoves *moves) {
    double t0 = op_now_ms();
    double budget = OP_BUDGET_MS;
    if (op_tot_n >= 20) {
        double mean = op_tot_ms / (double)op_tot_n;
        if (mean > OP_TARGET_MEAN) {
            double f = OP_TARGET_MEAN / mean;
            f = f * f;
            if (f < 0.1) f = 0.1;
            budget *= f;
        }
    }
    int r = op_choose_inner(view, seat, moves, budget);
    op_tot_ms += op_now_ms() - t0;
    op_tot_n++;
    return r;
}

static int op_choose_inner(const Game *view, int seat, const LegalMoves *moves, double budget) {
    if (moves->n <= 1) return 0;
    sim_tab_init(view->power_suit, min_value_for(view->num_players));
    int pol = op_policy_index(view, seat, moves);
#if !OP_USE_MC
    return pol;
#endif
    double t0 = op_now_ms();
    int trump = view->power_suit;
    // candidate set
    int cand[64], nc = 0;
    int unc = 0;
    for (int i = 0; i < view->num_battles; i++) if (card_is_none(view->table_battles[i].defense)) unc++;
    bool is_def = (seat == view->defender && view->num_battles > 0);
    // collect, keeping the cheapest covers
    int covi[8], covc[8], ncov = 0;
    for (int i = 0; i < moves->n && nc < 40; i++) {
        const LegalMove *m = &moves->moves[i];
        if (is_def && m->type == MOVE_COVER) {
            if (m->n_cards != unc) continue;
            int c = op_move_cards_cost(m, trump);
            if (ncov < 3) { covi[ncov] = i; covc[ncov] = c; ncov++; }
            else {
                int w = 0; for (int k = 1; k < ncov; k++) if (covc[k] > covc[w]) w = k;
                if (c < covc[w]) { covi[w] = i; covc[w] = c; }
            }
            continue;
        }
        cand[nc++] = i;
    }
    for (int k = 0; k < ncov; k++) cand[nc++] = covi[k];
    // always include the policy move
    bool have = false;
    for (int k = 0; k < nc; k++) if (cand[k] == pol) have = true;
    if (!have && nc < 64) cand[nc++] = pol;
    if (nc <= 1) return nc == 1 ? cand[0] : pol;

    static double sum[64], dsum[64], dsq[64];
    static bool active[64];
    int refk = 0;
    for (int k = 0; k < nc; k++) { sum[k] = dsum[k] = dsq[k] = 0; active[k] = true; if (cand[k] == pol) refk = k; }
    int nactive = nc;
    OpInfo inf;
    op_track(view, seat, &inf);
    op_rs ^= (uint64_t)view->num_logs * 0x9E3779B97F4A7C15ull + (uint64_t)seat;
OP_ST(op_st_dec++;)
    int worlds = 0, next_check = OP_CHECK0;
    static double val[64];
    while (worlds < OP_MAX_WORLDS && nactive > 1) {
        op_sample(&op_world, view, seat, &inf);
        uint64_t rs_save = op_rs;
        for (int k = 0; k < nc; k++) {
            if (!active[k] && k != refk) continue;
            op_rs = rs_save; // common random numbers across candidates
            memcpy(&op_sim, &op_world, offsetof(Game, logs));
            if (!legal_move_apply(&op_sim, seat, &moves->moves[cand[k]])) { val[k] = -100; }
            else {
                static Sim ss;
                sim_from_game(&ss, &op_sim);
#ifdef OP_DIFFTEST
                uint64_t rs0 = op_rs;
                static Game eg; memcpy(&eg, &op_sim, offsetof(Game, logs));
                double ev = op_rollout(&eg, seat);
                uint64_t rs1 = op_rs; op_rs = rs0;
#endif
                val[k] = sim_rollout(&ss, seat);
#ifdef OP_DIFFTEST
                op_dt_n++;
                bool same = ev == val[k] && rs1 == op_rs && eg.num_eliminated == ss.nelim;
                for (int q = 0; same && q < ss.nelim; q++) same = eg.elimination_order[q] == ss.elim[q];
                if (!same) op_dt_bad++;
#endif
            }
            OP_ST(op_st_roll++;)
        }
        for (int k = 0; k < nc; k++) {
            if (!active[k] && k != refk) continue;
            sum[k] += val[k];
            double d = val[k] - val[refk];
            dsum[k] += d; dsq[k] += d * d;
        }
        op_rand();
        worlds++;
        OP_ST(op_st_worlds++;)
        if (worlds >= next_check) {
            next_check *= 2;
            int b = -1;
            for (int k = 0; k < nc; k++) if (active[k] && (b < 0 || dsum[k] > dsum[b])) b = k;
            double n = worlds;
            double mb = dsum[b] / n, vb = dsq[b] / n - mb * mb;
            for (int k = 0; k < nc; k++) {
                if (!active[k] || k == b) continue;
                double mk = dsum[k] / n, vk = dsq[k] / n - mk * mk;
                double se = sqrt((vb + vk) / n) + 1e-6;
                if (mk < mb - OP_PRUNE_Z * se) { active[k] = false; nactive--; }
            }
        }
        if (op_now_ms() - t0 > budget) break;
    }
    OP_ST(op_st_ms += op_now_ms() - t0;)
    int best = -1;
    for (int k = 0; k < nc; k++) if (active[k] && (best < 0 || sum[k] > sum[best] + 1e-9)) best = k;
    // tie / near-tie bias towards the policy move
    if (active[refk] && sum[refk] >= sum[best] - 1e-9) return pol;
    (void)op_view_copy; (void)op_scratch;
    return cand[best];
}

const BotEntry my_bot = { "sapper", op_choose, 0 };
