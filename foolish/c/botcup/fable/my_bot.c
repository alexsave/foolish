// Bot Cup entry for team fable: "kestrel".
//
// Determinized Monte Carlo over a hand-written policy. Every decision with
// more than one sensible option is settled by sampling hidden worlds that
// agree with everything public (my hand, the table, the logs of attacks,
// covers, passes, pickups, discards, the flipped trump) and rolling each world
// to the end with the same heuristic policy for every seat, mirroring the
// referee's random first-come-first-served ordering. The policy alone is the
// fallback whenever the search has nothing to compare.
#include "botcup.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

// ---------- tunables -------------------------------------------------------

#ifndef FB_BUDGET_MS
#define FB_BUDGET_MS      28.0   // per non-trivial decision
#endif
#ifndef FB_MAX_WORLDS
#define FB_MAX_WORLDS     400
#endif
#define FB_MIN_WORLDS     6
#define FB_MAX_CANDS      12
#define FB_ROLLOUT_STEPS  600
#define FB_COVER_NODES    6000
// Policy knobs (overridable for experiments).
#ifndef FB_PICKUP_BASE
#define FB_PICKUP_BASE     14    // cost of a pickup beyond its cards
#endif
#ifndef FB_PICKUP_PER_CARD
#define FB_PICKUP_PER_CARD 7     // per card taken
#endif
#ifndef FB_PICKUP_ENDGAME
#define FB_PICKUP_ENDGAME  60    // extra once the stock is gone
#endif
#ifndef FB_THROW_MAX_V
#define FB_THROW_MAX_V     13    // highest non-trump value thrown in while the stock lasts
#endif
#ifndef FB_THROW_TRUMP_MAX
#define FB_THROW_TRUMP_MAX 13    // trumps up to this value are thrown in once the stock is gone (0 = none)
#endif
#ifndef FB_LEAD_PAIR_BONUS
#define FB_LEAD_PAIR_BONUS 12
#endif
#ifndef FB_PASS_SLACK
#define FB_PASS_SLACK      2     // a trump pass is taken when its cost <= cover cost + this
#endif
#ifndef FB_LEAD_END_HIGH
#define FB_LEAD_END_HIGH   0     // 1: once the stock is gone, lead the highest non-trump group instead
#endif
#ifndef FB_EPS
#define FB_EPS             200   // per-mille chance a rollout seat picks a random candidate instead of the policy move
#endif
#ifndef FB_EPS_SELF
#define FB_EPS_SELF        1     // 0: the searching seat plays its rollouts without noise
#endif
#ifndef FB_NO_HALVING
#define FB_NO_HALVING      0
#endif
#ifndef FB_INFER
#define FB_INFER           1     // 1: a seat that said good with room to throw in holds no matching card
#endif
#ifndef FB_PASS_ONLY_CHEAP
#define FB_PASS_ONLY_CHEAP 0     // 1: a non-trump pass is taken only when the cover costs more than pass + slack
#endif
#ifndef FB_PASS_ONE
#define FB_PASS_ONE        0     // 1: pass a single card even when several match
#endif

// ---------- small utilities ---------------------------------------------------

typedef struct { uint64_t s; } FbRng;

static inline uint64_t fb_rng_u64(FbRng *r) {
    uint64_t x = r->s;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    r->s = x;
    return x * 2685821657736338717ULL;
}
static inline int fb_rng_below(FbRng *r, int n) {
    return n <= 1 ? 0 : (int)((fb_rng_u64(r) >> 33) % (uint64_t)n);
}

static double fb_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
}

static inline bool fb_is_trump(const Game *g, Card c) { return c.suit == g->power_suit; }

// How much a card is worth keeping: 0..8 for a non-trump, 10..18 for a trump.
static inline int fb_keep(const Game *g, Card c) {
    return (c.value - MIN_VALUE_SMALL) + (fb_is_trump(g, c) ? 10 : 0);
}

static inline uint64_t fb_bit(Card c) { return 1ull << card_to_id(c); }

static inline int fb_popcount(uint64_t x) { return __builtin_popcountll(x); }

// A rollout clone: everything but the logs, which no handler reads back.
static void fb_copy(Game *dst, const Game *src) {
    memcpy(dst, src, offsetof(Game, logs));
    dst->num_logs = 0;
    dst->log_cap = 0;
    dst->log_virt = 0;
}

static bool fb_deck_empty(const Game *g) { return g->deck_count == 0 && !g->has_flipped; }

static int fb_uncovered(const Game *g, Card *atk) {
    int n = 0;
    for (int i = 0; i < g->num_battles; i++) {
        if (card_is_none(g->table_battles[i].defense)) {
            if (atk) atk[n] = g->table_battles[i].attack;
            n++;
        }
    }
    return n;
}

static int fb_table_cards(const Game *g) {
    int n = 0;
    for (int i = 0; i < g->num_battles; i++) n += card_is_none(g->table_battles[i].defense) ? 1 : 2;
    return n;
}

static void fb_table_values(const Game *g, bool *tv) {
    for (int i = 0; i < CARD_VALUE_MARKS; i++) tv[i] = false;
    for (int i = 0; i < g->num_battles; i++) {
        card_mark_value(tv, g->table_battles[i].attack.value);
        if (!card_is_none(g->table_battles[i].defense)) card_mark_value(tv, g->table_battles[i].defense.value);
    }
}

static bool fb_same_move(const LegalMove *a, const LegalMove *b) {
    if (a->type != b->type) return false;
    if (a->type == MOVE_PICKUP || a->type == MOVE_GOOD) return true;
    if (a->n_cards != b->n_cards) return false;
    for (int i = 0; i < a->n_cards; i++) {
        bool found = false;
        for (int j = 0; j < b->n_cards && !found; j++) {
            if (!card_eq(a->cards[i], b->cards[j])) continue;
            if (a->type == MOVE_COVER && !card_eq(a->attack_cards[i], b->attack_cards[j])) continue;
            found = true;
        }
        if (!found) return false;
    }
    return true;
}

static int fb_find_in_menu(const LegalMoves *moves, const LegalMove *m) {
    for (int i = 0; i < moves->n; i++) if (fb_same_move(&moves->moves[i], m)) return i;
    return -1;
}

// ---------- the cheapest full cover -------------------------------------------

typedef struct {
    const Game   *g;
    const Player *p;
    int    na;
    Card   atk[MAX_BATTLES];           // hardest first
    int    order[MAX_HAND_SIZE];       // hand indices, cheapest first
    bool   no_trump;
    int    best_cost;
    int8_t best_idx[MAX_BATTLES];
    int8_t cur_idx[MAX_BATTLES];
    bool   used[MAX_HAND_SIZE];
    long   nodes;
} FbCover;

static void fb_cover_dfs(FbCover *c, int d, int cost) {
    if (cost >= c->best_cost) return;
    if (++c->nodes > FB_COVER_NODES) return;
    if (d == c->na) {
        c->best_cost = cost;
        memcpy(c->best_idx, c->cur_idx, sizeof c->cur_idx);
        return;
    }
    for (int k = 0; k < c->p->hand_count; k++) {
        int j = c->order[k];
        if (c->used[j]) continue;
        Card h = c->p->hand[j];
        if (c->no_trump && fb_is_trump(c->g, h)) continue;
        if (!can_cover(c->atk[d], h, c->g->power_suit)) continue;
        c->used[j] = true;
        c->cur_idx[d] = (int8_t)j;
        fb_cover_dfs(c, d + 1, cost + fb_keep(c->g, h));
        c->used[j] = false;
    }
}

// Min keep-cost assignment of hand cards to the given attacks; -1 when no full
// cover exists. cov_out[i] is the cover of atk_in[i].
static int fb_best_cover(const Game *g, const Player *p, const Card *atk_in, int na,
                         bool no_trump, Card *cov_out) {
    if (na <= 0 || na > MAX_BATTLES) return -1;
    FbCover c;
    c.g = g; c.p = p; c.na = na; c.no_trump = no_trump;
    c.best_cost = 1 << 20; c.nodes = 0;
    // Attacks hardest first: trumps before non-trumps, high before low.
    int perm[MAX_BATTLES];
    for (int i = 0; i < na; i++) perm[i] = i;
    for (int i = 1; i < na; i++) {
        int x = perm[i], j = i;
        int kx = fb_keep(g, atk_in[x]);
        while (j > 0 && fb_keep(g, atk_in[perm[j - 1]]) < kx) { perm[j] = perm[j - 1]; j--; }
        perm[j] = x;
    }
    for (int i = 0; i < na; i++) c.atk[i] = atk_in[perm[i]];
    // Hand cheapest first.
    int hn = p->hand_count;
    for (int i = 0; i < hn; i++) c.order[i] = i;
    for (int i = 1; i < hn; i++) {
        int x = c.order[i], j = i;
        int kx = fb_keep(g, p->hand[x]);
        while (j > 0 && fb_keep(g, p->hand[c.order[j - 1]]) > kx) { c.order[j] = c.order[j - 1]; j--; }
        c.order[j] = x;
    }
    memset(c.used, 0, sizeof c.used);
    // Greedy first, so a big problem has an answer before the node cap hits.
    {
        int cost = 0; bool ok = true;
        for (int d = 0; d < na && ok; d++) {
            ok = false;
            for (int k = 0; k < hn; k++) {
                int j = c.order[k];
                if (c.used[j]) continue;
                Card h = p->hand[j];
                if (no_trump && fb_is_trump(g, h)) continue;
                if (!can_cover(c.atk[d], h, g->power_suit)) continue;
                c.used[j] = true; c.cur_idx[d] = (int8_t)j; cost += fb_keep(g, h); ok = true;
                break;
            }
        }
        if (ok) { c.best_cost = cost; memcpy(c.best_idx, c.cur_idx, sizeof c.cur_idx); }
        memset(c.used, 0, sizeof c.used);
    }
    fb_cover_dfs(&c, 0, 0);
    if (c.best_cost >= (1 << 20)) return -1;
    for (int i = 0; i < na; i++) cov_out[perm[i]] = p->hand[c.best_idx[i]];
    return c.best_cost;
}

// ---------- the policy (rollouts and fallback) --------------------------------

static void fb_set_attack(LegalMove *m, const Card *cards, int n) {
    m->type = MOVE_ATTACK;
    m->n_cards = (int8_t)n;
    for (int i = 0; i < n; i++) m->cards[i] = cards[i];
}

static void fb_set_cover(LegalMove *m, const Card *atk, const Card *cov, int n) {
    if (n > MAX_MOVE_CARDS) n = MAX_MOVE_CARDS;
    m->type = MOVE_COVER;
    m->n_cards = (int8_t)n;
    for (int i = 0; i < n; i++) { m->cards[i] = cov[i]; m->attack_cards[i] = atk[i]; }
}

// The first attacker's lead.
static bool fb_policy_lead(const Game *g, int s, LegalMove *m) {
    const Player *p = &g->players[s];
    int D = g->players[g->defender].hand_count;
    bool deck_empty = fb_deck_empty(g);
    if (p->hand_count == 0 || D <= 0) return false;

    // Empty the hand in one go when that is legal and the stock is gone.
    if (deck_empty && p->hand_count <= D) {
        bool same = true;
        for (int i = 1; i < p->hand_count; i++) if (p->hand[i].value != p->hand[0].value) same = false;
        if (same) { fb_set_attack(m, p->hand, p->hand_count); return true; }
    }

    int best_v = -1, best_score = -1000000;
    for (int v = MIN_VALUE_SMALL; v <= ACE_VALUE; v++) {
        int nt = 0, tr = 0;
        for (int i = 0; i < p->hand_count; i++) {
            if (p->hand[i].value != v) continue;
            if (fb_is_trump(g, p->hand[i])) tr++; else nt++;
        }
        if (nt + tr == 0) continue;
        int score;
        if (nt > 0) score = 100 + (FB_LEAD_END_HIGH && deck_empty ? 4 * v : -4 * v) + (nt > 1 ? FB_LEAD_PAIR_BONUS : 0);
        else score = -v;   // trump-only groups: last resort, lowest first
        if (score > best_score) { best_score = score; best_v = v; }
    }
    Card pick[MAX_MOVE_CARDS];
    int n = 0;
    // Non-trumps of the value first.
    for (int i = 0; i < p->hand_count && n < MAX_MOVE_CARDS && n < D; i++)
        if (p->hand[i].value == best_v && !fb_is_trump(g, p->hand[i])) pick[n++] = p->hand[i];
    if (n == 0) {
        // Lowest trump of that value (one card, or every one when the stock is gone).
        for (int i = 0; i < p->hand_count && n < MAX_MOVE_CARDS && n < D; i++) {
            if (p->hand[i].value != best_v) continue;
            pick[n++] = p->hand[i];
            if (!deck_empty) break;
        }
    }
    if (n == 0) return false;
    fb_set_attack(m, pick, n);
    return true;
}

// An attacker's throw-in, or good.
static bool fb_policy_throw(const Game *g, int s, LegalMove *m) {
    const Player *p = &g->players[s];
    bool tv[CARD_VALUE_MARKS];
    fb_table_values(g, tv);
    int D = g->players[g->defender].hand_count;
    int cap = D - fb_uncovered(g, NULL);
    m->type = MOVE_GOOD; m->n_cards = 0;
    if (cap <= 0) return true;
    bool deck_empty = fb_deck_empty(g);

    Card match[MAX_HAND_SIZE];
    int nm = 0;
    for (int i = 0; i < p->hand_count; i++)
        if (card_has_value(tv, p->hand[i].value)) match[nm++] = p->hand[i];
    if (nm == 0) return true;
    // Going out at once beats everything.
    if (deck_empty && nm == p->hand_count && nm <= cap && nm <= MAX_MOVE_CARDS) {
        fb_set_attack(m, match, nm);
        return true;
    }
    // Non-trumps, lowest first, as many as fit.
    Card pick[MAX_MOVE_CARDS];
    int n = 0;
    int max_v = deck_empty ? ACE_VALUE : FB_THROW_MAX_V;
    for (int v = MIN_VALUE_SMALL; v <= max_v && n < cap && n < MAX_MOVE_CARDS; v++)
        for (int i = 0; i < nm && n < cap && n < MAX_MOVE_CARDS; i++)
            if (match[i].value == v && !fb_is_trump(g, match[i])) pick[n++] = match[i];
    if (deck_empty && FB_THROW_TRUMP_MAX > 0)
        for (int v = MIN_VALUE_SMALL; v <= FB_THROW_TRUMP_MAX && n < cap && n < MAX_MOVE_CARDS; v++)
            for (int i = 0; i < nm && n < cap && n < MAX_MOVE_CARDS; i++)
                if (match[i].value == v && fb_is_trump(g, match[i])) pick[n++] = match[i];
    if (n == 0) return true;   // only trumps match: keep them
    fb_set_attack(m, pick, n);
    return true;
}

// The defender: pass, cover, or pick up.
static bool fb_policy_defend(const Game *g, int s, LegalMove *m) {
    const Player *p = &g->players[s];
    Card atk[MAX_BATTLES], cov[MAX_BATTLES];
    int na = fb_uncovered(g, atk);
    if (na == 0) return false;
    bool deck_empty = fb_deck_empty(g);
    int ntable = fb_table_cards(g);

    int cover_cost = fb_best_cover(g, p, atk, na, false, cov);

    // A pass: nothing covered, one value, a card of it, and room at the next seat.
    if (game_pass_allowed(g) && na == g->num_battles) {
        int v = atk[0].value;
        bool same = true;
        for (int i = 1; i < na; i++) if (atk[i].value != v) same = false;
        if (same) {
            int next = get_next_player_index(g, s);
            int room = g->players[next].hand_count - na;
            if (next != s && room >= 1) {
                Card pass[MAX_MOVE_CARDS];
                int np = 0, cost = 0;
                int pass_room = FB_PASS_ONE ? 1 : room;
                for (int i = 0; i < p->hand_count && np < pass_room && np < MAX_MOVE_CARDS; i++)
                    if (p->hand[i].value == v && !fb_is_trump(g, p->hand[i])) { pass[np++] = p->hand[i]; cost += fb_keep(g, p->hand[i]); }
                bool trump_pass = false;
                if (np == 0) {
                    for (int i = 0; i < p->hand_count; i++)
                        if (p->hand[i].value == v) { pass[np++] = p->hand[i]; cost = fb_keep(g, p->hand[i]); trump_pass = true; break; }
                }
                if (np > 0) {
                    bool take_pass = cover_cost < 0 || cost <= cover_cost + FB_PASS_SLACK
                                  || (!trump_pass && !FB_PASS_ONLY_CHEAP);
                    if (take_pass) {
                        m->type = MOVE_PASS;
                        m->n_cards = (int8_t)np;
                        for (int i = 0; i < np; i++) m->cards[i] = pass[i];
                        return true;
                    }
                }
            }
        }
    }

    if (cover_cost >= 0) {
        int pickup_cost = FB_PICKUP_PER_CARD * ntable + FB_PICKUP_BASE + (deck_empty ? FB_PICKUP_ENDGAME : 0);
        if (cover_cost <= pickup_cost) {
            fb_set_cover(m, atk, cov, na);
            return true;
        }
    }
    m->type = MOVE_PICKUP; m->n_cards = 0;
    return true;
}

static bool fb_policy(const Game *g, int s, LegalMove *m) {
    if (g->num_battles == 0) return s == g->first_attacker && fb_policy_lead(g, s, m);
    if (s == g->defender) return fb_policy_defend(g, s, m);
    return fb_policy_throw(g, s, m);
}

// ---------- rollouts -----------------------------------------------------------

static double fb_score(const Game *g, int me) {
    int n = g->num_players;
    for (int i = 0; i < g->num_eliminated; i++)
        if (g->elimination_order[i] == me) return (double)(n - (i + 1));
    if (game_done(g) >= 0) return 0.0;           // the fool
    // Unfinished: rank the seats still in by hand size, then by trump strength.
    int my_hand = g->players[me].hand_count, my_tr = 0;
    for (int j = 0; j < my_hand; j++) if (fb_is_trump(g, g->players[me].hand[j])) my_tr += g->players[me].hand[j].value;
    int better = 0, equal = 0;
    for (int s = 0; s < n; s++) {
        if (s == me || g->players[s].status != PLAYER_STATUS_IN) continue;
        int hc = g->players[s].hand_count, tr = 0;
        for (int j = 0; j < hc; j++) if (fb_is_trump(g, g->players[s].hand[j])) tr += g->players[s].hand[j].value;
        if (hc < my_hand || (hc == my_hand && tr > my_tr)) better++;
        else if (hc == my_hand && tr == my_tr) equal++;
    }
    double place = g->num_eliminated + 1 + better + 0.5 * equal;
    return (double)n - place;
}

static long fb_policy_failures;   // development counter: policy moves the engine refused
static int fb_candidates(const Game *v, int me, LegalMove *cand);

static double fb_rollout(Game *g, int me, FbRng *r) {
    for (int step = 0; step < FB_ROLLOUT_STEPS; step++) {
        if (game_done(g) >= 0) break;
        int elig[MAX_PLAYERS], ne = 0;
        for (int s = 0; s < g->num_players; s++) if (should_bot_act(g, s)) elig[ne++] = s;
        if (ne == 0) break;
        for (int i = ne - 1; i > 0; i--) {
            int j = fb_rng_below(r, i + 1);
            int t = elig[i]; elig[i] = elig[j]; elig[j] = t;
        }
        bool acted = false;
        for (int k = 0; k < ne && !acted; k++) {
            int s = elig[k];
            LegalMove m;
#if FB_EPS > 0
            if ((FB_EPS_SELF || s != me) && fb_rng_below(r, 1000) < FB_EPS) {
                LegalMove cs[FB_MAX_CANDS];
                int n = fb_candidates(g, s, cs);
                if (n > 0 && legal_move_apply(g, s, &cs[fb_rng_below(r, n)])) { acted = true; break; }
            }
#endif
            if (fb_policy(g, s, &m) && legal_move_apply(g, s, &m)) { acted = true; break; }
            fb_policy_failures++;
            static LegalMoves lm;
            calculate_legal_moves_lite(g, s, &lm);
            if (lm.n > 0 && legal_move_apply(g, s, &lm.moves[0])) acted = true;
        }
        if (!acted) break;
    }
    return fb_score(g, me);
}

// ---------- what I know about hidden cards --------------------------------------

typedef struct {
    uint64_t unseen;             // cards whose place I do not know
    uint64_t known[MAX_PLAYERS]; // cards I know sit in that seat's hand
    uint64_t forbid[MAX_PLAYERS];// cards that seat would have thrown in and did not
} FbKnow;

// Replay the public log to learn what each seat declined to throw in: a good
// said with room on the table, by a seat that has drawn nothing since, means
// its unknown cards hold no non-trump of a value that was on the table (and,
// once the stock is gone, no card of that value at all).
static void fb_infer_goods(const Game *v, int me, FbKnow *k) {
    int np = v->num_players;
    int hand[MAX_PLAYERS];
    for (int p = 0; p < np; p++) { hand[p] = CARDS_PER_PLAYER; k->forbid[p] = 0; }
    const int min_v = min_value_for(np);
    int drawable = NUM_SUITS * (ACE_VALUE - min_v + 1) - CARDS_PER_PLAYER * np;
    int drawn = 0, uncovered = 0, defender = -1;
    bool tv[CARD_VALUE_MARKS];
    for (int i = 0; i < CARD_VALUE_MARKS; i++) tv[i] = false;
    for (int i = 0; i < v->num_logs; i++) {
        const GameLog *l = &v->logs[i];
        int p = l->player_idx, n = l->num_pairs;
        bool seat_ok = p >= 0 && p < np;
        switch (l->log_type) {
        case LOG_ATTACK: case LOG_PASS:
            if (!seat_ok) break;
            if (l->log_type == LOG_ATTACK && uncovered == 0 && defender < 0) defender = (p + 1) % np;
            hand[p] -= n; uncovered += n;
            for (int j = 0; j < n; j++) card_mark_value(tv, l->pairs[j].primary.value);
            break;
        case LOG_COVER:
            if (!seat_ok) break;
            hand[p] -= n; uncovered -= n;
            for (int j = 0; j < n; j++) card_mark_value(tv, l->pairs[j].primary.value);
            break;
        case LOG_PICKUP:
            if (seat_ok) hand[p] += n;
            uncovered = 0;
            for (int j = 0; j < CARD_VALUE_MARKS; j++) tv[j] = false;
            break;
        case LOG_DISCARD:
            uncovered = 0;
            for (int j = 0; j < CARD_VALUE_MARKS; j++) tv[j] = false;
            break;
        case LOG_DRAW:
            if (!seat_ok) break;
            hand[p] += n; drawn += n; k->forbid[p] = 0;
            break;
        case LOG_DEFENDER_CHANGE:
            defender = l->defender_index;
            break;
        case LOG_GOOD: {
            if (!seat_ok || p == me || defender < 0 || defender >= np) break;
            if (hand[defender] - uncovered <= 0) break;
            bool stock_gone = drawn >= drawable;
            for (int val = min_v; val <= ACE_VALUE; val++) {
                if (!card_has_value(tv, val)) continue;
                for (int su = 0; su < NUM_SUITS; su++) {
                    if (su == v->power_suit && !stock_gone) continue;
                    k->forbid[p] |= 1ull << (su * 13 + val - 1);
                }
            }
            break;
        }
        default: break;
        }
    }
}

static Card fb_flip[MAX_PLAYERS];
static bool fb_flip_known[MAX_PLAYERS];

static void fb_know(const Game *v, int me, FbKnow *k) {
    const int min_v = min_value_for(v->num_players);
    uint64_t all = 0;
    for (int s = 0; s < NUM_SUITS; s++)
        for (int val = min_v; val <= ACE_VALUE; val++) all |= 1ull << (s * 13 + val - 1);
    uint64_t seen = 0;
    memset(k->known, 0, sizeof k->known);
    int last_draw_seat = -1;
    for (int i = 0; i < v->num_logs; i++) {
        const GameLog *l = &v->logs[i];
        int p = l->player_idx;
        bool seat_ok = p >= 0 && p < v->num_players;
        switch (l->log_type) {
        case LOG_ATTACK: case LOG_PASS: case LOG_COVER:
            if (!seat_ok) break;
            for (int j = 0; j < l->num_pairs; j++) {
                Card c = l->pairs[j].primary;
                if (card_in_range(c, min_v, ACE_VALUE)) k->known[p] &= ~fb_bit(c);
            }
            break;
        case LOG_PICKUP:
            if (!seat_ok || p == me) break;
            for (int j = 0; j < l->num_pairs; j++) {
                Card c = l->pairs[j].primary;
                if (card_in_range(c, min_v, ACE_VALUE)) k->known[p] |= fb_bit(c);
            }
            break;
        case LOG_DRAW:
            if (!seat_ok) break;
            last_draw_seat = p;
            if (p == me) break;
            for (int j = 0; j < l->num_pairs; j++) {
                Card c = l->pairs[j].primary;
                if (!botcup_is_hidden(c) && card_in_range(c, min_v, ACE_VALUE)) k->known[p] |= fb_bit(c);
            }
            break;
        case LOG_DISCARD:
            for (int j = 0; j < l->num_pairs; j++) {
                Card c = l->pairs[j].primary;
                if (card_in_range(c, min_v, ACE_VALUE)) seen |= fb_bit(c);
            }
            break;
        default: break;
        }
    }
    const Player *mine = &v->players[me];
    for (int j = 0; j < mine->hand_count; j++) seen |= fb_bit(mine->hand[j]);
    for (int i = 0; i < v->num_battles; i++) {
        seen |= fb_bit(v->table_battles[i].attack);
        if (!card_is_none(v->table_battles[i].defense)) seen |= fb_bit(v->table_battles[i].defense);
    }
    if (v->has_flipped) seen |= fb_bit(v->flipped);
    // The flipped trump is the last card anyone draws, so once it is gone the
    // last draw in the log took it, unless that card has shown up since.
    if (!v->has_flipped && fb_flip_known[me] && last_draw_seat >= 0 && last_draw_seat != me) {
        uint64_t fbit = fb_bit(fb_flip[me]);
        if (!(seen & fbit)) {
            bool elsewhere = false;
            for (int p = 0; p < v->num_players; p++) if (k->known[p] & fbit) elsewhere = true;
            if (!elsewhere) k->known[last_draw_seat] |= fbit;
        }
    }
    for (int p = 0; p < v->num_players; p++) {
        if (p == me) { k->known[p] = 0; continue; }
        k->known[p] &= ~seen;
        // Never claim more than the seat holds (a truncated log could).
        while (fb_popcount(k->known[p]) > v->players[p].hand_count) k->known[p] &= k->known[p] - 1;
        seen |= k->known[p];
    }
    k->unseen = all & ~seen;
    for (int p = 0; p < v->num_players; p++) k->forbid[p] = 0;
    if (FB_INFER) fb_infer_goods(v, me, k);
}

// One hidden world: the view with every hidden card replaced by a sample.
// Seats with inferred constraints draw from the allowed part of the pool first;
// when the constraints cannot all hold, they are relaxed rather than refused.
static void fb_sample_world(Game *w, const Game *v, int me, const FbKnow *k, FbRng *r) {
    fb_copy(w, v);
    Card pool[64];
    int np = 0;
    for (uint64_t u = k->unseen; u; u &= u - 1) pool[np++] = card_of_id(__builtin_ctzll(u));
    // A log that lost records can leave more slots than cards; the filler then
    // repeats pool cards (a wrong world, never a wild one).
    if (np == 0) { pool[0].suit = 0; pool[0].value = (int8_t)min_value_for(v->num_players); }
    for (int i = np - 1; i > 0; i--) {
        int j = fb_rng_below(r, i + 1);
        Card t = pool[i]; pool[i] = pool[j]; pool[j] = t;
    }
    int need[MAX_PLAYERS];
    for (int p = 0; p < v->num_players; p++) {
        Player *pl = &w->players[p];
        need[p] = 0;
        if (p == me) continue;
        int hc = pl->hand_count, n = 0;
        for (uint64_t u = k->known[p]; u && n < hc; u &= u - 1) pl->hand[n++] = card_of_id(__builtin_ctzll(u));
        need[p] = hc - n;
    }
    // Constrained seats first, most constrained first.
    for (int round = 0; round < v->num_players; round++) {
        int best = -1;
        for (int p = 0; p < v->num_players; p++)
            if (p != me && need[p] > 0 && k->forbid[p] && (best < 0 || fb_popcount(k->forbid[p]) > fb_popcount(k->forbid[best]))) best = p;
        if (best < 0) break;
        Player *pl = &w->players[best];
        int hc = pl->hand_count;
        // Pull allowed cards to the front of the pool, as many as needed.
        int front = 0;
        for (int i = 0; i < np && front < need[best]; i++) {
            if (k->forbid[best] & fb_bit(pool[i])) continue;
            Card t = pool[front]; pool[front] = pool[i]; pool[i] = t; front++;
        }
        for (int i = 0; i < need[best]; i++) pl->hand[hc - need[best] + i] = pool[i];
        // Remove the used cards from the pool.
        for (int i = need[best]; i < np; i++) pool[i - need[best]] = pool[i];
        np -= need[best];
        need[best] = 0;
    }
    int pi = 0;
    for (int p = 0; p < v->num_players; p++) {
        if (p == me || need[p] == 0) continue;
        Player *pl = &w->players[p];
        int hc = pl->hand_count;
        for (int i = 0; i < need[p]; i++) pl->hand[hc - need[p] + i] = pi < np ? pool[pi++] : pool[np > 0 ? (pi++ % np) : 0];
    }
    for (int i = 0; i < w->deck_count; i++) w->deck[i] = pi < np ? pool[pi++] : pool[np > 0 ? (pi++ % np) : 0];
    w->deterministic_deck = true;
}

// ---------- candidate moves --------------------------------------------------------

static int fb_add_cand(LegalMove *cand, int nc, const LegalMove *m) {
    if (nc >= FB_MAX_CANDS) return nc;
    for (int i = 0; i < nc; i++) if (fb_same_move(&cand[i], m)) return nc;
    cand[nc] = *m;
    return nc + 1;
}

static int fb_candidates(const Game *v, int me, LegalMove *cand) {
    int nc = 0;
    LegalMove m;
    const Player *p = &v->players[me];
    if (fb_policy(v, me, &m)) nc = fb_add_cand(cand, nc, &m);

    if (v->num_battles == 0) {
        // Leads: for every value, one card and the whole group.
        int D = v->players[v->defender].hand_count;
        bool deck_empty = fb_deck_empty(v);
        for (int pass = 0; pass < 2; pass++) {
            for (int val = MIN_VALUE_SMALL; val <= ACE_VALUE; val++) {
                Card nt[MAX_MOVE_CARDS], tr[MAX_MOVE_CARDS];
                int nnt = 0, ntr = 0;
                for (int i = 0; i < p->hand_count; i++) {
                    if (p->hand[i].value != val) continue;
                    if (fb_is_trump(v, p->hand[i])) { if (ntr < MAX_MOVE_CARDS) tr[ntr++] = p->hand[i]; }
                    else if (nnt < MAX_MOVE_CARDS) nt[nnt++] = p->hand[i];
                }
                if (nnt + ntr == 0) continue;
                if (pass == 0) {
                    if (nnt > 0) { fb_set_attack(&m, nt, 1); nc = fb_add_cand(cand, nc, &m); }
                    else { fb_set_attack(&m, tr, 1); nc = fb_add_cand(cand, nc, &m); }
                } else {
                    if (nnt > 1 && nnt <= D) { fb_set_attack(&m, nt, nnt); nc = fb_add_cand(cand, nc, &m); }
                    if (deck_empty && nnt + ntr > 1 && nnt + ntr <= D && nnt + ntr <= MAX_MOVE_CARDS) {
                        Card all[MAX_MOVE_CARDS]; int n = 0;
                        for (int i = 0; i < nnt; i++) all[n++] = nt[i];
                        for (int i = 0; i < ntr; i++) all[n++] = tr[i];
                        fb_set_attack(&m, all, n); nc = fb_add_cand(cand, nc, &m);
                    }
                }
            }
        }
        return nc;
    }

    if (me == v->defender) {
        Card atk[MAX_BATTLES], cov[MAX_BATTLES];
        int na = fb_uncovered(v, atk);
        if (na == 0) return nc;
        if (fb_best_cover(v, p, atk, na, false, cov) >= 0) { fb_set_cover(&m, atk, cov, na); nc = fb_add_cand(cand, nc, &m); }
        if (fb_best_cover(v, p, atk, na, true, cov) >= 0)  { fb_set_cover(&m, atk, cov, na); nc = fb_add_cand(cand, nc, &m); }
        m.type = MOVE_PICKUP; m.n_cards = 0; nc = fb_add_cand(cand, nc, &m);
        if (game_pass_allowed(v) && na == v->num_battles) {
            int val = atk[0].value;
            bool same = true;
            for (int i = 1; i < na; i++) if (atk[i].value != val) same = false;
            int next = get_next_player_index(v, me);
            int room = v->players[next].hand_count - na;
            if (same && next != me && room >= 1) {
                Card all[MAX_MOVE_CARDS]; int n = 0;
                bool did_nt = false, did_tr = false;
                for (int i = 0; i < p->hand_count; i++) {
                    if (p->hand[i].value != val) continue;
                    if (n < MAX_MOVE_CARDS) all[n++] = p->hand[i];
                    bool t = fb_is_trump(v, p->hand[i]);
                    if ((t && did_tr) || (!t && did_nt)) continue;
                    if (t) did_tr = true; else did_nt = true;
                    m.type = MOVE_PASS; m.n_cards = 1; m.cards[0] = p->hand[i];
                    nc = fb_add_cand(cand, nc, &m);
                }
                if (n > 1 && n <= room) {
                    m.type = MOVE_PASS; m.n_cards = (int8_t)n;
                    for (int i = 0; i < n; i++) m.cards[i] = all[i];
                    nc = fb_add_cand(cand, nc, &m);
                }
            }
        }
        return nc;
    }

    // Thrower.
    m.type = MOVE_GOOD; m.n_cards = 0; nc = fb_add_cand(cand, nc, &m);
    {
        bool tv[CARD_VALUE_MARKS];
        fb_table_values(v, tv);
        int cap = v->players[v->defender].hand_count - fb_uncovered(v, NULL);
        if (cap <= 0) return nc;
        Card nt[MAX_HAND_SIZE], tr[MAX_HAND_SIZE];
        int nnt = 0, ntr = 0;
        for (int val = MIN_VALUE_SMALL; val <= ACE_VALUE; val++)
            for (int i = 0; i < p->hand_count; i++) {
                if (p->hand[i].value != val || !card_has_value(tv, val)) continue;
                if (fb_is_trump(v, p->hand[i])) tr[ntr++] = p->hand[i]; else nt[nnt++] = p->hand[i];
            }
        if (nnt > 0) { fb_set_attack(&m, nt, 1); nc = fb_add_cand(cand, nc, &m); }
        if (nnt > 1) { fb_set_attack(&m, nt, nnt < cap ? nnt : cap); nc = fb_add_cand(cand, nc, &m); }
        if (nnt > 2 && cap >= 2) { fb_set_attack(&m, nt, 2); nc = fb_add_cand(cand, nc, &m); }
        if (ntr > 0) { fb_set_attack(&m, tr, 1); nc = fb_add_cand(cand, nc, &m); }
        if (ntr > 0 && nnt + ntr <= cap && nnt + ntr <= MAX_MOVE_CARDS && nnt + ntr > 1) {
            Card all[MAX_MOVE_CARDS]; int n = 0;
            for (int i = 0; i < nnt; i++) all[n++] = nt[i];
            for (int i = 0; i < ntr; i++) all[n++] = tr[i];
            fb_set_attack(&m, all, n); nc = fb_add_cand(cand, nc, &m);
        }
    }
    return nc;
}

// ---------- the decision -------------------------------------------------------------

static FbRng fb_rng = { 0x9E3779B97F4A7C15ULL };

#ifdef FB_STATS
#include <stdio.h>
#include <stdlib.h>
static long fb_st_decisions, fb_st_searched, fb_st_worlds, fb_st_rollouts, fb_st_cands;
static long fb_st_hist[8];   // worlds per searched decision: <10,<20,<40,<80,<160,<320,<600,600
static void fb_stats_dump(void) {
    fprintf(stderr, "kestrel stats: decisions %ld searched %ld worlds/searched %.1f cands/searched %.2f rollouts %ld policy_failures %ld\n",
            fb_st_decisions, fb_st_searched, fb_st_searched ? (double)fb_st_worlds / fb_st_searched : 0.0,
            fb_st_searched ? (double)fb_st_cands / fb_st_searched : 0.0, fb_st_rollouts, fb_policy_failures);
    fprintf(stderr, "  worlds hist <10:%ld <20:%ld <40:%ld <80:%ld <160:%ld <320:%ld <600:%ld =600:%ld\n",
            fb_st_hist[0], fb_st_hist[1], fb_st_hist[2], fb_st_hist[3], fb_st_hist[4], fb_st_hist[5], fb_st_hist[6], fb_st_hist[7]);
}
static void fb_stats_note(int worlds, int nc) {
    static bool armed;
    if (!armed) { armed = true; atexit(fb_stats_dump); }
    fb_st_searched++; fb_st_worlds += worlds; fb_st_rollouts += (long)worlds * nc; fb_st_cands += nc;
    int b = worlds < 10 ? 0 : worlds < 20 ? 1 : worlds < 40 ? 2 : worlds < 80 ? 3 : worlds < 160 ? 4 : worlds < 320 ? 5 : worlds < 600 ? 6 : 7;
    fb_st_hist[b]++;
}
#endif

static int fb_fallback(const Game *v, int me, const LegalMoves *moves) {
    LegalMove m;
    if (fb_policy(v, me, &m)) {
        int idx = fb_find_in_menu(moves, &m);
        if (idx >= 0) return idx;
        for (int i = 0; i < moves->n; i++) if (moves->moves[i].type == m.type) return i;
    }
    return 0;
}

static int fb_choose(const Game *view, int seat, const LegalMoves *moves) {
    if (moves->n <= 0) return 0;
#ifdef FB_STATS
    fb_st_decisions++;
#endif
    if (view->has_flipped) { fb_flip[seat] = view->flipped; fb_flip_known[seat] = true; }
    if (moves->n == 1) return 0;
    double t0 = fb_now_ms();

    LegalMove cand[FB_MAX_CANDS];
    int idx[FB_MAX_CANDS];
    int nc = fb_candidates(view, seat, cand);
    // Keep only what the menu offers.
    int kept = 0;
    for (int i = 0; i < nc; i++) {
        int k = fb_find_in_menu(moves, &cand[i]);
        if (k < 0) continue;
        cand[kept] = cand[i]; idx[kept] = k; kept++;
    }
    nc = kept;
    if (nc == 0) return fb_fallback(view, seat, moves);
    if (nc == 1) return idx[0];
#ifdef FB_HEURISTIC_ONLY
    return idx[0];
#endif

    FbKnow know;
    fb_know(view, seat, &know);

    static Game world, trial;
    double sum[FB_MAX_CANDS];
    bool alive[FB_MAX_CANDS];
    memset(sum, 0, sizeof sum);
    for (int c = 0; c < nc; c++) alive[c] = true;
    int worlds = 0, n_alive = nc;
    bool halved = false;
    fb_rng.s ^= (uint64_t)(t0 * 1000.0) | 1u;
    while (worlds < FB_MAX_WORLDS) {
        double spent = fb_now_ms() - t0;
        if (worlds >= FB_MIN_WORLDS && spent > FB_BUDGET_MS) break;
        // Successive halving: past 40% of the budget (or 40% of the worlds), drop
        // the weaker half and spend the rest on the contenders. The sums stay
        // paired because every survivor has seen the same worlds.
        if (!FB_NO_HALVING && !halved && n_alive > 2 && worlds >= FB_MIN_WORLDS
            && (spent > 0.4 * FB_BUDGET_MS || worlds >= (FB_MAX_WORLDS * 2) / 5)) {
            halved = true;
            int keep = (n_alive + 1) / 2;
            for (int k = n_alive; k > keep; k--) {
                int worst = -1;
                for (int c = 0; c < nc; c++) if (alive[c] && (worst < 0 || sum[c] < sum[worst])) worst = c;
                alive[worst] = false; n_alive--;
            }
        }
        fb_sample_world(&world, view, seat, &know, &fb_rng);
        uint64_t seed = fb_rng_u64(&fb_rng) | 1u;
        for (int c = 0; c < nc; c++) {
            if (!alive[c]) continue;
            fb_copy(&trial, &world);
            if (!legal_move_apply(&trial, seat, &cand[c])) { sum[c] -= 100.0; continue; }
            FbRng rr = { seed };
            sum[c] += fb_rollout(&trial, seat, &rr);
        }
        worlds++;
    }
#ifdef FB_STATS
    fb_stats_note(worlds, nc);
#endif
    int best = -1;
    for (int c = 0; c < nc; c++) if (alive[c] && (best < 0 || sum[c] > sum[best])) best = c;
    return idx[best];
}

static void fb_new_game(int seat, int num_players) {
    (void)num_players;
    if (seat >= 0 && seat < MAX_PLAYERS) { fb_flip_known[seat] = false; fb_flip[seat] = CARD_NONE; }
}

#ifndef FB_NAME
#define FB_NAME "kestrel"
#endif
const BotEntry my_bot = { FB_NAME, fb_choose, fb_new_game };
