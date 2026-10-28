// Bot Cup entry "sonnet": determinized Monte Carlo over a fast private
// simulator of the passing game, with a hand-written rollout policy.
//
//  1. Rebuild what is public (hand counts, table, trump, every log) into a
//     belief: which cards are known to sit in which hand (pickups) and which
//     cards are simply unseen.
//  2. Sample worlds (unseen cards dealt to the unknown slots and the deck).
//  3. For every candidate move on the menu, play the world out to the end of
//     the game with the policy in all seats (same worlds, same random stream
//     for every candidate) and keep the mean points of our seat.
//  4. Play the best candidate.
//
// The simulator is a bitmask re-implementation of the engine rules (checked
// against the engine move by move in test_sim.c). Every symbol is static.
#include "botcup.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

typedef uint64_t u64;
#define POP(x) ((int)__builtin_popcountll(x))
#define CTZ(x) ((int)__builtin_ctzll(x))
#define BIT(c) (1ull << (c))

// ---------------------------------------------------------------- tables
static int8_t T_SUIT[64], T_VAL[64];
static u64 VM[16];   // all cards of a value
static u64 SM[4];    // all cards of a suit
static int tables_ready;

static void tables(void) {
    if (tables_ready) return;
    tables_ready = 1;
    for (int i = 0; i < 52; i++) {
        T_SUIT[i] = (int8_t)(i / 13);
        T_VAL[i] = (int8_t)(i % 13 + 1);
        VM[T_VAL[i]] |= BIT(i);
        SM[T_SUIT[i]] |= BIT(i);
    }
}

// ---------------------------------------------------------------- params
enum {
    P_NW, P_TW0, P_TW1, P_R0, P_R1, P_B0, P_B1, P_THROW, P_DUMP, P_PAIR,
    P_PASSTR, P_THROWE, P_MS, P_MINW, P_MAXW, P_PASSALL, P_THROWALL, P_LEADE, P_ELIM, P_PASSMODE, P_NCOV, P_NATT, P_COUNT
};
static double PR[P_COUNT] = {
    0.20,  // NW: worth of a non-trump per value above 4
    2.00,  // TW0: worth of a trump (base)
    0.60,  // TW1: trump worth per value above 4
    100.0, // R0: relief per card covered (deck non-empty)
    2.50,  // R1: extra relief when the deck is empty
    1.00,  // B0: burden per card picked up
    1.50,  // B1: extra burden when the deck is empty
    14.0,  // THROW: highest value thrown in while the deck lasts
    0.60,  // DUMP: chance an attacker dumps before a pickup
    4.00,  // PAIR: lead preference for shedding pairs
    1.0,   // PASSTR: pass with a trump when a cover would cost a trump
    13.0,  // THROWE: highest value thrown in with an empty deck
    40.0,  // MS: milliseconds budget for a contested decision
    8.0,   // MINW: min worlds
    3000.0, // MAXW: max worlds
    1.0,   // PASSALL: pass every non-trump card of the rank
    1.0,   // THROWALL: throw the whole low group (else one card)
    1.5,   // LEADE: shedding bonus per card when the deck is empty
    3.0,   // ELIM: z-score for dropping a candidate (0 = never)
    0.0,   // PASSMODE: 0 pass whenever possible, 1 only if the cover needs a trump, 2 only if no cover
    5.0,   // NCOV: covers kept as root candidates
    16.0   // NATT: attacks kept as root candidates when the menu is long
};
#ifndef PARENV
#define PARENV "SB_PAR"
#endif
static int params_ready;
static void params(void) {
    if (params_ready) return;
    params_ready = 1;
    const char *e = getenv(PARENV);
    if (!e) return;
    // "idx=val,idx=val"
    while (*e) {
        char *end;
        long i = strtol(e, &end, 10);
        if (*end != '=') break;
        double v = strtod(end + 1, &end);
        if (i >= 0 && i < P_COUNT) PR[i] = v;
        e = (*end == ',') ? end + 1 : end;
        if (*end != ',') break;
    }
}

// ---------------------------------------------------------------- sim
#define MAXB 40
typedef struct {
    int8_t np, trump, first, def, nb, unc, nout, nin, nopass, minv;
    uint8_t in[8];
    int8_t outo[8];
    int16_t ndeck;
    int8_t flip;
    uint8_t deck[56];
    u64 hand[8];
    uint8_t ta[MAXB], td[MAXB];
    uint32_t good;
    uint16_t rmask;
    uint32_t rng;
} Sim;

static inline uint32_t rnd(Sim *S) {
    uint32_t x = S->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    S->rng = x;
    return x;
}
static inline double rndf(Sim *S) { return (rnd(S) >> 8) * (1.0 / 16777216.0); }

static inline int hcnt(const Sim *S, int p) { return POP(S->hand[p]); }
static inline int is_tr(const Sim *S, int c) { return T_SUIT[c] == S->trump; }
static inline int beats(const Sim *S, int a, int d) {
    return T_SUIT[d] == T_SUIT[a] ? T_VAL[d] > T_VAL[a] : T_SUIT[d] == S->trump;
}

static int s_next(const Sim *S, int x) {
    if (S->nin <= 1) return x;
    int y = x;
    do { y = (y + 1) % S->np; } while (!S->in[y]);
    return y;
}
static void s_out(Sim *S, int p) {
    if (!S->in[p]) return;
    S->in[p] = 0;
    S->nin--;
    S->outo[S->nout++] = (int8_t)p;
}
static inline int s_nocards(const Sim *S) { return S->ndeck == 0 && S->flip < 0; }

static void s_draw6(Sim *S, int p) {
    while (POP(S->hand[p]) < 6) {
        int c;
        if (S->ndeck > 0) c = S->deck[--S->ndeck];
        else if (S->flip >= 0) { c = S->flip; S->flip = -1; }
        else break;
        S->hand[p] |= BIT(c);
    }
    if (S->hand[p] == 0 && S->in[p]) s_out(S, p);
}

static void s_refill(Sim *S) {
    if (s_nocards(S)) {
        for (int i = 0; i < S->np; i++)
            if (S->hand[i] == 0 && S->in[i]) s_out(S, i);
        return;
    }
    int p = S->first;
    uint32_t visited = 0;
    do {
        if (visited & (1u << p)) break;
        visited |= 1u << p;
        if (p != S->def) s_draw6(S, p);
        p = s_next(S, p);
    } while (p != S->first);
    s_draw6(S, S->def);
}

static inline void s_clear_table(Sim *S) { S->nb = 0; S->unc = 0; S->rmask = 0; S->good = 0; }

static void s_attack(Sim *S, int seat, const int *c, int n) {
    for (int i = 0; i < n; i++) {
        S->hand[seat] &= ~BIT(c[i]);
        S->ta[S->nb] = (uint8_t)c[i];
        S->td[S->nb] = 255;
        S->nb++; S->unc++;
        S->rmask |= (uint16_t)(1u << T_VAL[c[i]]);
    }
    S->good = 0;
    if (S->hand[seat] == 0 && s_nocards(S)) s_out(S, seat);
}

// returns 1 if the bout ended (defender emptied the hand)
static int s_cover(Sim *S, const int *cc, const int *aa, int n) {
    int d = S->def;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < S->nb; j++) {
            if (S->ta[j] == aa[i] && S->td[j] == 255) { S->td[j] = (uint8_t)cc[i]; break; }
        }
        S->unc--;
        S->hand[d] &= ~BIT(cc[i]);
        S->rmask |= (uint16_t)(1u << T_VAL[cc[i]]);
    }
    S->good = 0;
    if (S->hand[d] == 0) {
        s_clear_table(S);
        s_refill(S);
        S->first = (int8_t)d;
        if (S->hand[d] == 0) {
            if (S->in[d]) s_out(S, d);
            S->first = (int8_t)s_next(S, S->first);
        }
        S->def = (int8_t)s_next(S, S->first);
        return 1;
    }
    return 0;
}

static void s_pass(Sim *S, const int *c, int n) {
    int d = S->def;
    int nxt = s_next(S, d);
    for (int i = 0; i < n; i++) {
        S->hand[d] &= ~BIT(c[i]);
        S->ta[S->nb] = (uint8_t)c[i];
        S->td[S->nb] = 255;
        S->nb++; S->unc++;
        S->rmask |= (uint16_t)(1u << T_VAL[c[i]]);
    }
    S->good = 0;
    if (s_nocards(S) && S->hand[d] == 0) s_out(S, d);
    S->def = (int8_t)nxt;
}

static void s_pickup(Sim *S) {
    int d = S->def;
    for (int j = 0; j < S->nb; j++) {
        S->hand[d] |= BIT(S->ta[j]);
        if (S->td[j] != 255) S->hand[d] |= BIT(S->td[j]);
    }
    s_clear_table(S);
    s_refill(S);
    S->first = (int8_t)s_next(S, d);
    S->def = (int8_t)s_next(S, S->first);
}

static void s_roundend(Sim *S) {
    s_clear_table(S);
    s_refill(S);
    S->first = S->def;
    S->def = (int8_t)s_next(S, S->first);
}

// ---------------------------------------------------------------- policy
typedef struct { int type, n; int c[10]; int a[10]; } Act;
enum { A_ATTACK, A_COVER, A_PASS, A_PICKUP, A_GOOD };

static inline double worth(const Sim *S, int c) {
    // Was T_VAL - 4 (36-card only: 4 is one below the 6): with the 52-card deck at 6-8
    // players a 2..4 was worth less than nothing and a trump 2..5 less than a non-trump ace.
    // Measured from one below the deck's lowest value, as at 36 cards.
    int v = T_VAL[c] - (S->minv - 1);
    return is_tr(S, c) ? PR[P_TW0] + PR[P_TW1] * v : PR[P_NW] * v;
}
static inline int ccost(const Sim *S, int c) { return T_VAL[c] + (is_tr(S, c) ? 20 : 0); }

// cards of hand sorted by ascending cost
static int sorted_hand(const Sim *S, u64 h, int *out) {
    int n = 0;
    while (h) {
        int c = CTZ(h); h &= h - 1;
        int k = ccost(S, c);
        int i = n++;
        while (i > 0 && ccost(S, out[i - 1]) > k) { out[i] = out[i - 1]; i--; }
        out[i] = c;
    }
    return n;
}

static int cover_dfs(const Sim *S, const int *ua, int nu, int k, const int *hs, int nh,
                     u64 used, int *asg, int *nodes) {
    if (k == nu) return 1;
    if (++*nodes > 3000) return 0;
    for (int i = 0; i < nh; i++) {
        if (used & BIT(i)) continue;
        if (!beats(S, ua[k], hs[i])) continue;
        asg[k] = hs[i];
        if (cover_dfs(S, ua, nu, k + 1, hs, nh, used | BIT(i), asg, nodes)) return 1;
    }
    return 0;
}

// Cheapest-first full cover of every uncovered attack. Returns 1 and fills a.
static int plan_cover(const Sim *S, int seat, Act *a) {
    int ua[MAXB], nu = 0;
    for (int j = 0; j < S->nb; j++) if (S->td[j] == 255) ua[nu++] = S->ta[j];
    if (nu == 0 || nu > 10) return 0;
    // hardest first
    for (int i = 1; i < nu; i++) {
        int x = ua[i], kx = ccost(S, x), j = i;
        while (j > 0 && ccost(S, ua[j - 1]) < kx) { ua[j] = ua[j - 1]; j--; }
        ua[j] = x;
    }
    int hs[64];
    int nh = sorted_hand(S, S->hand[seat], hs);
    if (nh < nu) return 0;
    int asg[MAXB], nodes = 0;
    if (!cover_dfs(S, ua, nu, 0, hs, nh, 0, asg, &nodes)) return 0;
    a->type = A_COVER; a->n = nu;
    for (int i = 0; i < nu; i++) { a->c[i] = asg[i]; a->a[i] = ua[i]; }
    return 1;
}

static u64 match_mask(const Sim *S) {
    u64 m = 0;
    for (int v = 1; v <= 13; v++) if (S->rmask & (1u << v)) m |= VM[v];
    return m;
}

// Lead: low non-trump groups, shedding pairs.
static void pol_lead(const Sim *S, int seat, Act *a) {
    u64 h = S->hand[seat];
    u64 TR = SM[S->trump];
    int cap = hcnt(S, S->def);
    int empty = s_nocards(S);
    double best = 1e9; u64 bm = 0;
    for (int v = S->minv; v <= 13; v++) {
        u64 g = h & VM[v];
        if (!g) continue;
        u64 nt = g & ~TR;
        u64 pick;
        double sc;
        if (nt) {
            pick = nt;
            int k = POP(pick);
            if (k > cap) { while (POP(pick) > cap) pick &= pick - 1; k = POP(pick); }
            sc = v - PR[P_PAIR] * (k - 1) - (empty ? PR[P_LEADE] * k : 0);
        } else {
            pick = g & TR;
            if (!pick) continue;
            pick &= -pick;
            sc = 30 + v;
        }
        if (sc < best) { best = sc; bm = pick; }
    }
    if (!bm) { bm = h & -h; }
    a->type = A_ATTACK; a->n = 0;
    while (bm && a->n < 10) { a->c[a->n++] = CTZ(bm); bm &= bm - 1; }
}

static void pol_defend(const Sim *S, int seat, Act *a) {
    u64 h = S->hand[seat];
    u64 TR = SM[S->trump];
    int nh = POP(h);
    int empty = s_nocards(S);
    // pass?
    int can_pass = !S->nopass;
    if (can_pass) for (int j = 0; j < S->nb; j++) if (S->td[j] != 255) { can_pass = 0; break; }
    int pv = T_VAL[S->ta[0]];
    if (can_pass) for (int j = 1; j < S->nb; j++) if (T_VAL[S->ta[j]] != pv) { can_pass = 0; break; }
    Act cv; int has_cov = plan_cover(S, seat, &cv);
    if (can_pass) {
        u64 pm = h & VM[pv];
        int nxt = s_next(S, S->def);
        int room = hcnt(S, nxt) - S->nb;
        if (pm && room >= 1 && nxt != seat) {
            u64 nt = pm & ~TR;
            if (PR[P_PASSMODE] > 0.5 && has_cov) {
                int tc = 0;
                for (int i = 0; i < cv.n; i++) tc += is_tr(S, cv.c[i]);
                if (PR[P_PASSMODE] > 1.5 || tc == 0) nt = 0;
            }
            u64 use = nt;
            if (!use && PR[P_PASSTR] > 0 && has_cov) {
                int tc = 0;
                for (int i = 0; i < cv.n; i++) tc += is_tr(S, cv.c[i]);
                if (tc > 0) use = pm & -pm;
            }
            if (!use && PR[P_PASSTR] > 1.5) use = pm & -pm;
            if (!use && !has_cov) use = pm & -pm;
            if (use) {
                if (PR[P_PASSALL] < 0.5) use &= -use;
                while (POP(use) > room) use &= use - 1;
                // never pass away the last cards if that leaves nothing
                a->type = A_PASS; a->n = 0;
                while (use && a->n < 10) { a->c[a->n++] = CTZ(use); use &= use - 1; }
                return;
            }
        }
    }
    if (!has_cov) { a->type = A_PICKUP; a->n = 0; return; }
    // cover or pick up?
    double covv = 0, pickv = 0;
    double r = PR[P_R0] + (empty ? PR[P_R1] : 0);
    double b = PR[P_B0] + (empty ? PR[P_B1] : 0);
    for (int i = 0; i < cv.n; i++) covv += r - worth(S, cv.c[i]);
    for (int j = 0; j < S->nb; j++) pickv += worth(S, S->ta[j]) - b;
    if (nh == cv.n) { *a = cv; return; }   // covering the last cards wins
    if (covv >= pickv) *a = cv;
    else { a->type = A_PICKUP; a->n = 0; }
}

// one attacker's throw-in. Returns number of cards (0 = say good).
static int pol_throw(const Sim *S, int seat, Act *a) {
    if (S->nb == 0) return 0;
    int cap = hcnt(S, S->def) - S->unc;
    int empty = s_nocards(S);
    if (empty) cap -= 1;
    if (cap <= 0) return 0;
    u64 TR = SM[S->trump];
    u64 cand = S->hand[seat] & match_mask(S);
    u64 nt = cand & ~TR;
    if (!nt) return 0;
    int lo = 99;
    u64 t = nt;
    while (t) { int c = CTZ(t); t &= t - 1; if (T_VAL[c] < lo) lo = T_VAL[c]; }
    double lim = empty ? PR[P_THROWE] : PR[P_THROW];
    if (lo > lim) return 0;
    u64 pick = nt & VM[lo];
    if (empty) pick = nt;      // shed everything that fits
    else if (PR[P_THROWALL] < 0.5) pick &= -pick;
    while (POP(pick) > cap) pick &= pick - 1;
    a->type = A_ATTACK; a->n = 0;
    while (pick && a->n < 10) { a->c[a->n++] = CTZ(pick); pick &= pick - 1; }
    return a->n;
}

// attackers dump matching non-trump cards onto a defender who is picking up
static void pol_dump(Sim *S) {
    int d = S->def;
    int order = S->first;
    u64 TR = SM[S->trump];
    for (int k = 0; k < S->np; k++) {
        int a = (order + k) % S->np;
        if (a == d || !S->in[a]) continue;
        if (PR[P_DUMP] < 1.0 && rndf(S) >= PR[P_DUMP]) continue;
        int cap = hcnt(S, d) - S->unc;
        if (cap <= 0) break;
        u64 cand = S->hand[a] & match_mask(S);
        u64 pick = cand & ~TR;
        if (s_nocards(S) && (cand & TR) && POP(cand) == POP(S->hand[a])) pick = cand;
        if (!pick) continue;
        while (POP(pick) > cap) {
            // drop the highest
            int hi = -1, hv = -1; u64 t = pick;
            while (t) { int c = CTZ(t); t &= t - 1; if (T_VAL[c] > hv) { hv = T_VAL[c]; hi = c; } }
            pick &= ~BIT(hi);
        }
        int cs[10], n = 0;
        while (pick && n < 10) { cs[n++] = CTZ(pick); pick &= pick - 1; }
        s_attack(S, a, cs, n);
        if (S->nin <= 1) return;
    }
}

// ---------------------------------------------------------------- driver
static void apply_act(Sim *S, int seat, const Act *a) {
    switch (a->type) {
    case A_ATTACK: s_attack(S, seat, a->c, a->n); break;
    case A_COVER:  s_cover(S, a->c, a->a, a->n); break;
    case A_PASS:   s_pass(S, a->c, a->n); break;
    case A_PICKUP: pol_dump(S); s_pickup(S); break;
    default: break;
    }
}

// Play the game out from the current state. Returns when over (or capped).
static void sim_run(Sim *S) {
    int guard = 0;
    while (S->nin > 1 && guard++ < 4000) {
        Act a;
        if (S->nb == 0) {
            if (!S->in[S->first]) S->first = (int8_t)s_next(S, S->first);
            if (S->first == S->def) S->def = (int8_t)s_next(S, S->first);
            pol_lead(S, S->first, &a);
            s_attack(S, S->first, a.c, a.n);
            continue;
        }
        if (S->unc > 0) {
            int d = S->def;
            pol_defend(S, d, &a);
            apply_act(S, d, &a);
            continue;
        }
        int thrown = 0;
        for (int k = 0; k < S->np && !thrown; k++) {
            int at = (S->first + k) % S->np;
            if (at == S->def || !S->in[at] || (S->good & (1u << at))) continue;
            if (pol_throw(S, at, &a)) { s_attack(S, at, a.c, a.n); thrown = 1; }
            else S->good |= 1u << at;
        }
        if (!thrown) s_roundend(S);
    }
}

// points for `me` at the end (approximate if capped)
static int final_points(const Sim *S, int me) {
    int place[8];
    int np = S->np;
    int k = 0;
    for (int i = 0; i < S->nout; i++) place[(int)S->outo[i]] = ++k;
    if (S->nin == 1) {
        for (int i = 0; i < np; i++) if (S->in[i]) place[i] = np;
    } else {
        // rank the remaining by hand size
        int rem[8], nr = 0;
        for (int i = 0; i < np; i++) if (S->in[i]) rem[nr++] = i;
        for (int i = 0; i < nr; i++)
            for (int j = i + 1; j < nr; j++)
                if (hcnt(S, rem[j]) < hcnt(S, rem[i])) { int t = rem[i]; rem[i] = rem[j]; rem[j] = t; }
        for (int i = 0; i < nr; i++) place[rem[i]] = ++k;
    }
    return np - place[me];
}

// ---------------------------------------------------------------- belief
typedef struct {
    int np, me, trump, minv;
    u64 known[8];     // cards known to be in a seat's hand
    int unk[8];       // unknown cards in that seat's hand
    int ndeck;        // unknown cards in the deck (flip excluded)
    int flip;         // -1 if none
    u64 pool;         // unseen cards
    u64 myhand;
} Bel;

static u64 cardmask_log(const GameLog *l) {
    u64 m = 0;
    for (int i = 0; i < l->num_pairs; i++) {
        Card c = l->pairs[i].primary;
        if (c.suit == 0 && c.value == 1) { /* may be a real spade ace-of-nothing; checked by caller */ }
        m |= BIT(card_to_id(c));
    }
    return m;
}

static void build_belief(const Game *v, int me, Bel *B) {
    memset(B, 0, sizeof *B);
    B->np = v->num_players;
    B->me = me;
    B->trump = v->power_suit;
    B->minv = min_value_for(v->num_players);
    int hasflip = v->has_flipped;
    B->flip = hasflip ? card_to_id(v->flipped) : -1;
    u64 all = 0;
    for (int s = 0; s < 4; s++)
        for (int val = B->minv; val <= 13; val++) all |= BIT(s * 13 + val - 1);
    u64 mine = 0;
    for (int i = 0; i < v->players[me].hand_count; i++) mine |= BIT(card_to_id(v->players[me].hand[i]));
    B->myhand = mine;
    u64 seen = mine;
    u64 known[8] = {0};
    for (int k = 0; k < v->num_logs; k++) {
        const GameLog *l = &v->logs[k];
        int p = l->player_idx;
        switch (l->log_type) {
        case LOG_PICKUP:
            if (p >= 0 && p < 8) known[p] |= cardmask_log(l);
            break;
        case LOG_DRAW:
            if (p >= 0 && p < 8 && hasflip == 0 && 0) {}
            if (p >= 0 && p < 8) {
                for (int i = 0; i < l->num_pairs; i++) {
                    Card c = l->pairs[i].primary;
                    if (p == me) continue;
                    // the flipped card is public when somebody takes it
                    if (c.suit == v->flipped.suit && c.value == v->flipped.value && !hasflip)
                        known[p] |= BIT(card_to_id(c));
                }
            }
            break;
        case LOG_ATTACK: case LOG_PASS:
            if (p >= 0 && p < 8) known[p] &= ~cardmask_log(l);
            break;
        case LOG_COVER:
            if (p >= 0 && p < 8) known[p] &= ~cardmask_log(l);
            break;
        case LOG_DISCARD:
            seen |= cardmask_log(l);
            break;
        default: break;
        }
    }
    // table
    for (int j = 0; j < v->num_battles; j++) {
        seen |= BIT(card_to_id(v->table_battles[j].attack));
        if (!card_is_none(v->table_battles[j].defense)) seen |= BIT(card_to_id(v->table_battles[j].defense));
    }
    if (hasflip) seen |= BIT(B->flip);
    u64 kn_all = 0;
    for (int p = 0; p < B->np; p++) {
        if (p == me) { B->known[p] = mine; continue; }
        // keep only plausible known cards, never more than the hand holds
        u64 k = known[p] & all & ~seen;
        int hc = v->players[p].hand_count;
        while (POP(k) > hc) k &= k - 1;
        B->known[p] = k;
        kn_all |= k;
        B->unk[p] = hc - POP(k);
    }
    B->pool = all & ~seen & ~kn_all;
    B->ndeck = v->deck_count;
    int slots = B->ndeck;
    for (int p = 0; p < B->np; p++) if (p != me) slots += B->unk[p];
    int have = POP(B->pool);
    // reconcile truncated logs: drop surplus unseen cards (they were discarded)
    while (have > slots) { B->pool &= B->pool - 1; have--; }
    if (have < slots) {
        // too few unseen cards: shrink the deck first
        int d = slots - have;
        if (d > B->ndeck) d = B->ndeck;
        B->ndeck -= d;
        slots -= d;
        for (int p = 0; p < B->np && have < slots; p++) {
            if (p == me) continue;
            while (have < slots && B->unk[p] > 0) { B->unk[p]--; slots--; }
        }
    }
}

// public part of a sim from a view
static void sim_public(const Game *v, Sim *S) {
    memset(S, 0, sizeof *S);
    S->np = v->num_players;
    S->trump = v->power_suit;
    S->minv = (int8_t)min_value_for(v->num_players);
    S->first = v->first_attacker;
    S->def = v->defender;
    S->nopass = (v->rules & GAME_RULE_NO_PASS) ? 1 : 0;
    S->good = v->good_players_mask;
    S->nb = (int8_t)v->num_battles;
    for (int j = 0; j < v->num_battles; j++) {
        S->ta[j] = (uint8_t)card_to_id(v->table_battles[j].attack);
        if (card_is_none(v->table_battles[j].defense)) { S->td[j] = 255; S->unc++; }
        else S->td[j] = (uint8_t)card_to_id(v->table_battles[j].defense);
        S->rmask |= (uint16_t)(1u << T_VAL[S->ta[j]]);
        if (S->td[j] != 255) S->rmask |= (uint16_t)(1u << T_VAL[S->td[j]]);
    }
    for (int p = 0; p < S->np; p++) {
        S->in[p] = (v->players[p].status == PLAYER_STATUS_IN);
        if (S->in[p]) S->nin++;
    }
    S->nout = 0;
    for (int i = 0; i < v->num_eliminated; i++) S->outo[S->nout++] = v->elimination_order[i];
    S->flip = v->has_flipped ? (int8_t)card_to_id(v->flipped) : -1;
}

static void sample_world(const Bel *B, const Game *v, Sim *S, uint32_t seed) {
    sim_public(v, S);
    S->rng = seed ? seed : 1;
    int ids[64], n = 0;
    u64 pl = B->pool;
    while (pl) { ids[n++] = CTZ(pl); pl &= pl - 1; }
    for (int i = n - 1; i > 0; i--) {
        int j = (int)(rnd(S) % (uint32_t)(i + 1));
        int t = ids[i]; ids[i] = ids[j]; ids[j] = t;
    }
    int k = 0;
    for (int p = 0; p < B->np; p++) {
        if (p == B->me) { S->hand[p] = B->myhand; continue; }
        u64 h = B->known[p];
        for (int i = 0; i < B->unk[p] && k < n; i++) h |= BIT(ids[k++]);
        S->hand[p] = h;
    }
    S->ndeck = 0;
    for (int i = 0; i < B->ndeck && k < n; i++) S->deck[S->ndeck++] = (uint8_t)ids[k++];
}

// ---------------------------------------------------------------- root
static long st_dec, st_worlds, st_cands;
static void st_print(void) { fprintf(stderr, "[sonnet] decisions %ld avg worlds %.1f avg cands %.1f\n", st_dec, st_dec ? (double)st_worlds / st_dec : 0.0, st_dec ? (double)st_cands / st_dec : 0.0); }
static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
}

static void apply_root(Sim *S, int seat, const LegalMove *m) {
    int c[10], a[10];
    int n = m->n_cards > 10 ? 10 : m->n_cards;
    for (int i = 0; i < n; i++) { c[i] = card_to_id(m->cards[i]); a[i] = card_to_id(m->attack_cards[i]); }
    switch (m->type) {
    case MOVE_ATTACK: s_attack(S, seat, c, n); break;
    case MOVE_COVER:  s_cover(S, c, a, n); break;
    case MOVE_PASS:   s_pass(S, c, n); break;
    case MOVE_PICKUP: pol_dump(S); s_pickup(S); break;
    case MOVE_GOOD:   S->good |= 1u << seat; break;
    default: break;
    }
}

static int move_cost(const LegalMove *m, int trump) {
    int s = 0;
    for (int i = 0; i < m->n_cards; i++) s += m->cards[i].value + (m->cards[i].suit == trump ? 20 : 0);
    return s;
}

static u64 act_mask(const Act *a) {
    u64 m = 0;
    for (int i = 0; i < a->n; i++) m |= BIT(a->c[i]);
    return m;
}

// policy-only decision (no search): map the policy's act onto the menu
static int policy_choose(const Game *v, int seat, const LegalMoves *moves) {
    Bel B;
    build_belief(v, seat, &B);
    Sim S;
    sample_world(&B, v, &S, 777u + (uint32_t)v->num_logs);
    Act a; a.type = A_GOOD; a.n = 0;
    if (S.nb == 0) { if (seat == S.first) pol_lead(&S, seat, &a); }
    else if (seat == S.def) pol_defend(&S, seat, &a);
    else { if (!pol_throw(&S, seat, &a)) a.type = A_GOOD; }
    int want = a.type == A_ATTACK ? MOVE_ATTACK : a.type == A_COVER ? MOVE_COVER :
               a.type == A_PASS ? MOVE_PASS : a.type == A_PICKUP ? MOVE_PICKUP : MOVE_GOOD;
    u64 am = act_mask(&a);
    for (int i = 0; i < moves->n; i++) {
        const LegalMove *m = &moves->moves[i];
        if (m->type != want) continue;
        if (want == MOVE_PICKUP || want == MOVE_GOOD) return i;
        u64 mm = 0;
        for (int k = 0; k < m->n_cards; k++) mm |= BIT(card_to_id(m->cards[k]));
        if (mm == am) return i;
    }
    for (int i = 0; i < moves->n; i++) if (moves->moves[i].type == MOVE_GOOD || moves->moves[i].type == MOVE_PICKUP) return i;
    return 0;
}

static int choose(const Game *v, int seat, const LegalMoves *moves) {
    tables();
    params();
    static int reg;
    if (!reg && getenv("SB_STAT")) { reg = 1; atexit(st_print); }
    if (moves->n <= 1) return 0;
    if (PR[P_MS] < 0) return policy_choose(v, seat, moves);
    // Was 2-5 seats only: at 6-8 players (the 52-card deck) it returned menu entry 0 on
    // every decision. The simulator and belief already handle 8 seats and 52 cards.
    if (v->num_players > MAX_PLAYERS || v->num_players < 2) return 0;
    // candidate pruning
    int cand[64], nc = 0;
    int uncovered = 0;
    for (int j = 0; j < v->num_battles; j++) if (card_is_none(v->table_battles[j].defense)) uncovered++;
    // dedupe full covers by card set
    u64 seen_sets[64]; int nseen = 0;
    int cov[256], ncov = 0;
    int others[256], nother = 0;
    for (int i = 0; i < moves->n && i < 4096; i++) {
        const LegalMove *m = &moves->moves[i];
        if (m->type == MOVE_COVER) {
            if (m->n_cards != uncovered) continue;
            u64 sm = 0;
            for (int k = 0; k < m->n_cards; k++) sm |= BIT(card_to_id(m->cards[k]));
            int dup = 0;
            for (int k = 0; k < nseen; k++) if (seen_sets[k] == sm) { dup = 1; break; }
            if (dup) continue;
            if (nseen < 64) seen_sets[nseen++] = sm;
            if (ncov < 256) cov[ncov++] = i;
        } else if (nother < 256) others[nother++] = i;
    }
    // keep the 5 cheapest covers
    for (int i = 1; i < ncov; i++) {
        int x = cov[i], kx = move_cost(&moves->moves[x], v->power_suit), j = i;
        while (j > 0 && move_cost(&moves->moves[cov[j - 1]], v->power_suit) > kx) { cov[j] = cov[j - 1]; j--; }
        cov[j] = x;
    }
    if (ncov > (int)PR[P_NCOV]) ncov = (int)PR[P_NCOV];
    for (int i = 0; i < ncov; i++) cand[nc++] = cov[i];
    // other moves: keep all if few, else the cheapest 14 plus GOOD/PICKUP/PASS
    if (nother > (int)PR[P_NATT] + 2) {
        int keep[256], nk = 0;
        for (int i = 0; i < nother; i++) {
            const LegalMove *m = &moves->moves[others[i]];
            if (m->type != MOVE_ATTACK) keep[nk++] = others[i];
        }
        int att[256], na = 0;
        for (int i = 0; i < nother; i++) {
            const LegalMove *m = &moves->moves[others[i]];
            if (m->type == MOVE_ATTACK) att[na++] = others[i];
        }
        for (int i = 1; i < na; i++) {
            int x = att[i], kx = move_cost(&moves->moves[x], v->power_suit), j = i;
            while (j > 0 && move_cost(&moves->moves[att[j - 1]], v->power_suit) > kx) { att[j] = att[j - 1]; j--; }
            att[j] = x;
        }
        for (int i = 0; i < na && i < (int)PR[P_NATT]; i++) keep[nk++] = att[i];
        for (int i = 0; i < nk && nc < 60; i++) cand[nc++] = keep[i];
    } else {
        for (int i = 0; i < nother && nc < 60; i++) cand[nc++] = others[i];
    }
    if (nc == 0) return 0;
    if (nc == 1) return cand[0];

    Bel B;
    build_belief(v, seat, &B);
    double t0 = now_ms();
    double budget = PR[P_MS];
    static uint8_t sc[4096][64];
    double sum[64] = {0};
    int alive[64], na = nc;
    for (int i = 0; i < nc; i++) alive[i] = 1;
    int nw = 0;
    int minw = (int)PR[P_MINW], maxw = (int)PR[P_MAXW];
    if (maxw > 4096) maxw = 4096;
    uint32_t wseed = 0x9E3779B9u * (uint32_t)(v->num_logs + 1) + (uint32_t)seat * 7919u + 12345u;
    while (nw < maxw) {
        if (nw >= minw && now_ms() - t0 > budget) break;
        Sim W;
        wseed = wseed * 1664525u + 1013904223u;
        sample_world(&B, v, &W, wseed);
        uint32_t rs = wseed ^ 0xA5A5A5A5u;
        for (int i = 0; i < nc; i++) {
            if (!alive[i]) continue;
            Sim S = W;
            S.rng = rs;
            apply_root(&S, seat, &moves->moves[cand[i]]);
            sim_run(&S);
            int p = final_points(&S, seat);
            sc[nw][i] = (uint8_t)p;
            sum[i] += p;
        }
        nw++;
        if (nw >= 96 && (nw & 31) == 0 && na > 1 && PR[P_ELIM] > 0) {
            int l = -1;
            for (int i = 0; i < nc; i++) if (alive[i] && (l < 0 || sum[i] > sum[l])) l = i;
            for (int i = 0; i < nc; i++) {
                if (!alive[i] || i == l) continue;
                double m = 0, q = 0;
                for (int w = 0; w < nw; w++) { double d = (double)sc[w][i] - sc[w][l]; m += d; q += d * d; }
                m /= nw;
                double var = q / nw - m * m;
                double se = __builtin_sqrt(var / nw) + 1e-9;
                if (m < -PR[P_ELIM] * se && m < -0.02) { alive[i] = 0; na--; }
            }
            if (na == 1) break;
        }
    }
    int bi = -1;
    for (int i = 0; i < nc; i++) if (alive[i] && (bi < 0 || sum[i] > sum[bi] + 1e-9)) bi = i;
    st_dec++; st_worlds += nw; st_cands += nc;
    if (getenv("SB_TRACE")) {
        static const char *tn[] = {"ATT","COV","PASS","PICK","GOOD"};
        static const char sn[] = "SHCD", vn[] = "??????6789TJQKA";
        fprintf(stderr, "seat %d trump %c hand:", seat, sn[v->power_suit]);
        for (int i = 0; i < v->players[seat].hand_count; i++) { Card c = v->players[seat].hand[i]; fprintf(stderr, " %c%c", vn[c.value + 1 > 14 ? 14 : c.value + 1 - 0], sn[c.suit]); }
        fprintf(stderr, " | table:");
        for (int j = 0; j < v->num_battles; j++) { Card a = v->table_battles[j].attack; fprintf(stderr, " %c%c", vn[a.value+1], sn[a.suit]); if (!card_is_none(v->table_battles[j].defense)) { Card d = v->table_battles[j].defense; fprintf(stderr, "/%c%c", vn[d.value+1], sn[d.suit]); } }
        fprintf(stderr, " deck %d counts", v->deck_count);
        for (int p = 0; p < v->num_players; p++) fprintf(stderr, " %d", v->players[p].hand_count);
        fprintf(stderr, " nw %d\n", nw);
        for (int i = 0; i < nc; i++) {
            const LegalMove *m = &moves->moves[cand[i]];
            fprintf(stderr, "   %s%s", i == bi ? "*" : " ", tn[m->type]);
            for (int k = 0; k < m->n_cards; k++) fprintf(stderr, " %c%c", vn[m->cards[k].value+1], sn[m->cards[k].suit]);
            fprintf(stderr, " : %.3f%s\n", sum[i] / (nw ? nw : 1), alive[i] ? "" : " (dropped)");
        }
    }
    return cand[bi];
}

const BotEntry my_bot = { "sonnet", choose, NULL };
