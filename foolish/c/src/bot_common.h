// What the bots share: the helpers the cordite lineage (cordite, astrolite,
// octogen, blackpowder), the espresso and handwritten families and the analysis
// tools each carried a copy of.
//
// THE SMALL ONES ARE static inline, so every bot compiles them into its own
// hot loops exactly as it did when each file had its own copy: the -O3 Monte
// Carlo core in the wasm builds keeps its codegen, and a fix made here is made
// in every bot at once. The solver and the belief self-check, which are not
// small, are bot_common.c's.
//
// A BELIEF is one seat's public-information picture of everybody else's hand
// (BotBelief): the unseen pool, the cards publicly located in a hand ("pinned"),
// the void constraints a pickup proved, and the soft rank floor a lowest-first
// attacker reveals, with the per-seat distrust that switches a floor or a void
// off once that seat's play contradicts it. Cordite and astrolite use it as it
// is, octogen embeds it with its own behavioural tells, blackpowder uses its
// pool, pins and voids and leaves the floors at zero.
#ifndef CNITRO_BOT_COMMON_H
#define CNITRO_BOT_COMMON_H

#include "game.h"
#include "card.h"
#include "strategy.h"
#include "cordite_sim.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// ---------- small utils ------------------------------------------------

static inline bool bot_set_contains(const Card *arr, int n, Card c) {
    for (int i = 0; i < n; i++) if (card_eq(arr[i], c)) return true;
    return false;
}

// The cordite lineage's seed mixer: a world's, a rollout's or a shuffle's
// stream from two words. Never 0, so an xorshift-style consumer never sticks.
static inline uint32_t bot_mix(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u);
    h ^= h >> 16; h *= 0x85EBCA77u; h ^= h >> 13;
    return h ? h : 1;
}

// The analysis tools' mixer (cnitro_analyse, cnitro_winprob): a hash-combine
// step and murmur3's 32-bit finaliser. A different function from bot_mix, kept
// so every seeded analysis reproduces; never 0.
static inline uint32_t bot_mix_combine(uint32_t a, uint32_t b) {
    uint32_t h = a ^ 0x9E3779B9u;
    h ^= b + 0x7F4A7C15u + (h << 6) + (h >> 2);
    h *= 0x85EBCA77u; h ^= h >> 13; h *= 0xC2B2AE3Du; h ^= h >> 16;
    return h ? h : 1u;
}

// Seats still in the game.
static inline int bot_in_count(const Game *g) {
    int n = 0;
    for (int i = 0; i < g->num_players; i++)
        if (g->players[i].status == PLAYER_STATUS_IN) n++;
    return n;
}

// Copy only the live prefix of a Game (header + first num_logs entries).
// logs[] is the final member, so everything else is inside the prefix.
static inline void bot_lite_clone(Game *dst, const Game *src) {
    size_t base = offsetof(Game, logs);
    memcpy(dst, src, base + (size_t)src->num_logs * sizeof(GameLog));
}

// A sampled world's board: the whole Game but its log, which keeps only the
// LOG_DISCARD entries (the one log type any rollout policy reads, espresso's
// discard memory), so a trial clone in the hot loop is a fraction of the size.
// Short slots (WORLD_LOG_CAP > 0, the wasm builds) are marked so log_alloc
// applies the same discard filter to rollout appends (game.h); 0 on native
// keeps full capacity.
static inline void bot_clone_world(Game *dst, const Game *src) {
    memcpy(dst, src, offsetof(Game, logs));
    int nl = 0;
    for (int i = 0; i < src->num_logs; i++) {
        if (src->logs[i].log_type != LOG_DISCARD) continue;
        if (WORLD_LOG_CAP > 0 && nl >= WORLD_LOG_CAP) break;  // crafted states only
        dst->logs[nl++] = src->logs[i];
    }
    dst->num_logs = nl;
    dst->log_cap  = WORLD_LOG_CAP;
    dst->log_virt = (int16_t)nl;
}

// A move's cards summed: each card's value, plus `trump_bonus` for a trump
// (the TS getAttackPreference / getCardValue at 20, getDefensePreference at 10).
static inline int bot_card_sum(const LegalMove *m, int power_suit, int trump_bonus) {
    int total = 0;
    for (int i = 0; i < m->n_cards; i++) {
        total += m->cards[i].value + (m->cards[i].suit == power_suit ? trump_bonus : 0);
    }
    return total;
}

// Every card in the game (the TS computeTotalCardCount): deck + discard +
// table (1 per attack + 1 per defense) + every hand, OUT players' empty ones
// included, + the face-up trump if present.
static inline int bot_total_card_count(const Game *g) {
    int table = 0;
    for (int i = 0; i < g->num_battles; i++) {
        table += 1 + (!card_is_none(g->table_battles[i].defense) ? 1 : 0);
    }
    int hands = 0;
    for (int i = 0; i < g->num_players; i++) hands += g->players[i].hand_count;
    return g->deck_count + g->discard_pile_length + table + hands + (g->has_flipped ? 1 : 0);
}

// How likely an opponent is to attack with a trump (the TS
// getTrumpAttackProbability, RNG-free): 0.02 while the deck is
// alive, then 0.65 rising with the share of the game already discarded, held
// to [0.5, 0.95]. (handwritten_prod keeps its own: it tests the face-up trump
// alone, the TS port's stale deck field.)
static inline double bot_trump_attack_probability(const Game *g) {
    if (g->deck_count > 0 || g->has_flipped) return 0.02;
    int total = bot_total_card_count(g);
    if (total < 1) total = 1;
    double ratio = (double)g->discard_pile_length / (double)total;
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;
    double p = 0.65 + 0.35 * ratio;
    if (p < 0.5) p = 0.5;
    if (p > 0.95) p = 0.95;
    return p;
}

// A move plays some trump (cards.some(c => c.suit === trump)) / plays none
// (cards.every(c => c.suit !== trump), vacuously true when empty).
static inline bool bot_move_has_trump(const LegalMove *m, int trump) {
    for (int i = 0; i < m->n_cards; i++) {
        if (m->cards[i].suit == trump) return true;
    }
    return false;
}
static inline bool bot_move_no_trump(const LegalMove *m, int trump) {
    return !bot_move_has_trump(m, trump);
}

// ---------- belief state ------------------------------------------------

#define BOT_MAX_VOIDS 6

typedef struct {
    Card pool[80];                  // unseen pool (deck + opponents' unknowns)
    int  n;
    Card pinned[MAX_PLAYERS][MAX_HAND_SIZE];  // publicly located in p's hand
    int  pinned_n[MAX_PLAYERS];
    // Hard-ish void constraints: attack cards a defender demonstrably
    // couldn't cover at pickup time. Cleared on their draw.
    Card voids[MAX_PLAYERS][BOT_MAX_VOIDS];
    int  void_n[MAX_PLAYERS];
    // Soft rank floor: lowest non-trump value p can hold (0 = none). Set by
    // a single-card non-trump first attack at 3+ players in (lowest-first
    // attacker policies reveal their minimum). Cleared when p gains cards.
    int  floor_v[MAX_PLAYERS];
    // Trust flags, from observed behavior this game.
    bool distrust_floor[MAX_PLAYERS];
    bool distrust_void[MAX_PLAYERS];
} BotBelief;

// p cannot hold c: c covers one of p's void cards.
static inline bool bot_void_forbidden(const BotBelief *B, const Game *g, int p, Card c) {
    for (int k = 0; k < B->void_n[p]; k++) {
        if (can_cover(B->voids[p][k], c, g->power_suit)) return true;
    }
    return false;
}

// p's floor says p holds no non-trump below it.
static inline bool bot_floor_forbidden(const BotBelief *B, const Game *g, int p, Card c) {
    return B->floor_v[p] > 0 && c.suit != g->power_suit && c.value < B->floor_v[p];
}

// One seat's row of pinned cards (MAX_HAND_SIZE wide): drop c, the last card
// taking its place; add c once, when there is room.
static inline void bot_pin_remove(Card *row, int *n, Card c) {
    for (int q = 0; q < *n; q++) {
        if (card_eq(row[q], c)) {
            row[q] = row[*n - 1];
            (*n)--;
            return;
        }
    }
}
static inline void bot_pin_add(Card *row, int *n, Card c) {
    if (*n >= MAX_HAND_SIZE) return;
    if (bot_set_contains(row, *n, c)) return;
    row[(*n)++] = c;
}

static inline void bot_pinned_remove(BotBelief *B, int p, Card c) {
    bot_pin_remove(B->pinned[p], &B->pinned_n[p], c);
}
static inline void bot_pinned_add(BotBelief *B, int p, Card c) {
    bot_pin_add(B->pinned[p], &B->pinned_n[p], c);
}

// A floor contradiction (p plays a non-trump card below their inferred
// floor) means p is not a lowest-first attacker: distrust their floors.
static inline void bot_floor_check(BotBelief *B, const Game *g, int p, Card c) {
    if (p < 0 || B->floor_v[p] <= 0) return;
    if (c.suit != g->power_suit && c.value < B->floor_v[p]) {
        B->floor_v[p] = 0;
        B->distrust_floor[p] = true;
    }
}

// The oracle self-check (CD_VERIFY / OG_VERIFY, test-only): reads the real
// hands to validate the public-information belief, never to play, and says
// on stderr, under `tag`, every pinned card a hand lacks, every unknown card
// a void or a floor forbids, and every unknown card missing from the pool.
void bot_verify_belief(const char *tag, const Game *g, int bot_idx, const BotBelief *B);

// ---------- the exact solver -------------------------------------------

#define BOT_SOLVE_MAX_DEPTH 48
#define BOT_SOLVE_MAX_MOVES 96

typedef struct {
    long budget;
    bool aborted;
    int  me;
} BotSolver;

// The shared solver scratch is there (cordite_sim.c's static slots).
bool bot_solver_ready(void);

// Value in [-1000, 1000] from S->me's perspective: positive = me escaping,
// negative = me as durak. Magnitude prefers faster wins / slower losses.
// Defender-priority sequential abstraction of the real loop; S->aborted on a
// spent budget, a node past BOT_SOLVE_MAX_DEPTH or one with more than
// BOT_SOLVE_MAX_MOVES moves.
int bot_solve(BotSolver *S, const Game *g, int alpha, int beta, int depth);

// Exact full-info resolution of a rollout leaf (2 players in, deck gone, few
// cards) within `budget` nodes. The loser's index, or -1 when unresolved.
int bot_leaf_solve(const Game *g, long budget);

// The rollout policy for a sampled world: handwritten while the deck is alive
// or the game is heads-up, espresso for multi-player endgames (gunpowder's
// rule). `policy` is CD_ROLLOUT's research override: 1 espresso everywhere,
// 2 handwritten everywhere, 0 the rule.
StrategyFn bot_rollout_for(const Game *g, int policy);

#endif
