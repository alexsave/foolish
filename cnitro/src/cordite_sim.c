// Compact bitboard rollout engine — see cordite_sim.h.
//
// Why only the handwritten policy is reimplemented:
//   cd_rollout_for(g) returns espresso ONLY when the deck is dead AND in_count
//   != 2 (i.e. 3+ players). But espresso_strategy_choose with in_count > 2
//   immediately defers to handwritten_strategy_choose. So espresso's 1v1 logic
//   (which needs in_count == 2) is never reached through cd_rollout_for: the
//   effective rollout policy is ALWAYS handwritten. We reproduce handwritten
//   exactly here, directly on the bitboard state (no move-list materialized).

#include "cordite_sim.h"
#include "game.h"
#include "card.h"
#include <string.h>
#include <stddef.h>   // offsetof (solver clones skip the dead deck[] tail)
#include <stdint.h>
#include <stdlib.h>   // calloc/free for the solver transposition table
#ifdef CD_TT_STATS
#include <stdio.h>    // measurement-only (native); wasm never sets CD_TT_STATS
#endif

// ---------- card-id helpers --------------------------------------------

#define ID(suit, value) ((suit) * 13 + ((value) - 1))
static inline int id_suit(int id)  { return id / 13; }
static inline int id_value(int id) { return id % 13 + 1; }
static inline int card_id(Card c)  { return c.suit * 13 + (c.value - 1); }

// Precomputed masks (id space 0..51).
static uint64_t VALUE_MASK[14];   // VALUE_MASK[v] = all ids with value v (1..13)
static uint64_t SUIT_MASK[4];
static int      g_masks_ready = 0;

static uint64_t HIGHER_MASK[52];  // same-suit ids with strictly higher value

static void ensure_masks(void) {
    if (g_masks_ready) return;
    for (int s = 0; s < 4; s++) {
        SUIT_MASK[s] = 0;
        for (int v = 1; v <= 13; v++) {
            int id = ID(s, v);
            SUIT_MASK[s] |= (1ull << id);
            VALUE_MASK[v] |= (1ull << id);
        }
    }
    for (int s = 0; s < 4; s++) {
        for (int v = 1; v <= 13; v++) {
            int id = ID(s, v);
            for (int w = v + 1; w <= 13; w++) HIGHER_MASK[id] |= (1ull << ID(s, w));
        }
    }
    g_masks_ready = 1;
}

static inline int popcnt64(uint64_t x) { return __builtin_popcountll(x); }
static inline int ctz64(uint64_t x)    { return __builtin_ctzll(x); }

// score = value + (trump ? 1000 : 0); used pervasively as a tie-break key.
static inline int id_score(int id, int power) {
    return id_value(id) + (id_suit(id) == power ? 1000 : 0);
}

// can_cover on ids (same rule as can_cover in game.c).
static inline int id_can_cover(int attack, int defense, int power) {
    int as = id_suit(attack), ds = id_suit(defense);
    if (ds != as) return ds == power && as != power;
    return id_value(defense) > id_value(attack);
}

// ---------- RNG (shared with the game engine) --------------------------
extern double game_random(void);

// ---------- build from Game --------------------------------------------

void cd_sim_from_game(SimState *s, const Game *g) {
    ensure_masks();
    memset(s, 0, sizeof(*s));
    s->num_players = g->num_players;
    s->power_suit  = g->power_suit;
    s->defender    = g->defender;
    s->first_attacker = g->first_attacker;
    s->status      = g->status;
    s->num_battles = g->num_battles;
    s->deck_count  = g->deck_count;
    s->discard_pile_length = g->discard_pile_length;
    s->has_flipped = g->has_flipped;
    s->flipped_id  = g->has_flipped ? (uint8_t)card_id(g->flipped) : 0;
    s->good_mask   = g->good_players_mask;
    s->num_eliminated = g->num_eliminated;

    for (int p = 0; p < g->num_players; p++) {
        uint64_t h = 0;
        const Player *pl = &g->players[p];
        for (int j = 0; j < pl->hand_count; j++) h |= (1ull << card_id(pl->hand[j]));
        s->hand[p] = h;
        s->status_p[p] = pl->status;
        if (pl->status == PLAYER_STATUS_IN)       s->in_mask  |= (1u << p);
        else if (pl->status == PLAYER_STATUS_OUT) s->out_mask |= (1u << p);
    }
    for (int i = 0; i < g->num_eliminated; i++) s->elim_order[i] = g->elimination_order[i];

    for (int i = 0; i < g->num_battles; i++) {
        s->atk[i] = (uint8_t)card_id(g->table_battles[i].attack);
        s->table_vmask |= VALUE_MASK[id_value(s->atk[i])];
        if (!card_is_none(g->table_battles[i].defense)) {
            s->def[i] = (uint8_t)card_id(g->table_battles[i].defense);
            s->covered_mask |= (1ull << i);
            s->table_vmask |= VALUE_MASK[id_value(s->def[i])];
        }
    }

    s->deck_n = g->deck_count;
    for (int i = 0; i < g->deck_count; i++) s->deck[i] = (uint8_t)card_id(g->deck[i]);
}

// ---------- low-level state ops ----------------------------------------

static inline int sim_hand_count(const SimState *s, int p) {
    return popcnt64(s->hand[p]);
}

static inline int sim_in_count(const SimState *s) {
    return __builtin_popcount(s->in_mask);
}

static inline int sim_next_player(const SimState *s, int cur) {
    // Mirrors get_next_player_index's <=1-IN guard (TS parity): with the
    // rotation collapsed the caller's seat is returned unchanged.
    if (__builtin_popcount(s->in_mask) <= 1) return cur;
    // First non-OUT seat cyclically after cur (the byte loop skipped only
    // OUT statuses, so non-IN-non-OUT seats are eligible stops — preserved).
    uint32_t notout = ~s->out_mask & ((1u << s->num_players) - 1u);
    uint32_t hi = notout & ~((2u << cur) - 1u);
    return __builtin_ctz(hi ? hi : notout);
}

// Returns the single IN player if exactly one remains (game over), else -1.
static int sim_done(const SimState *s) {
    if (__builtin_popcount(s->in_mask) != 1) return -1;
    if (__builtin_popcount(s->out_mask) != s->num_players - 1) return -1;
    return __builtin_ctz(s->in_mask);
}

static inline int sim_no_cards_left(const SimState *s) {
    return s->deck_n == 0 && !s->has_flipped;
}

static inline int sim_count_uncovered(const SimState *s) {
    return s->num_battles - popcnt64(s->covered_mask);
}

static inline int sim_all_covered(const SimState *s) {
    return s->num_battles > 0 && popcnt64(s->covered_mask) == s->num_battles;
}

// table values bitmask: which values (by VALUE_MASK) are present on the table.
// Cached in the state (see cordite_sim.h) — this was the hottest loop in the
// wasm profile when rebuilt per query.
static inline uint64_t sim_table_value_mask(const SimState *s) {
    return s->table_vmask;
}

// Forced-draw queue (see cordite_sim.h): pins the next draws to exact card
// ids. Used by novichok's refill pinning, where the true refill cards after a
// battle-ending root move are a deterministic function of the live RNG state.
static _Thread_local uint8_t sim_forced_q[32];
static _Thread_local int sim_forced_n = 0;
static _Thread_local int sim_forced_i = 0;

void cd_sim_set_forced_draws(const uint8_t *ids, int n) {
    if (n > (int)sizeof(sim_forced_q)) n = (int)sizeof(sim_forced_q);
    for (int i = 0; i < n; i++) sim_forced_q[i] = ids[i];
    sim_forced_n = n;
    sim_forced_i = 0;
}

// draw one card id, mirroring draw_card (deck array splice + flipped fallback).
static int sim_draw(SimState *s, int *out) {
    if (sim_forced_i < sim_forced_n) {
        uint8_t want = sim_forced_q[sim_forced_i];
        if (s->deck_n == 0) {
            // The engine's draw here is RNG-free (flipped fallback); the
            // pinned id is that same flipped card when states match.
            sim_forced_i = sim_forced_n;
            if (!s->has_flipped) return 0;
            *out = s->flipped_id;
            s->has_flipped = 0;
            return 1;
        }
        for (int i = 0; i < s->deck_n; i++) {
            if (s->deck[i] == want) {
                for (int j = i + 1; j < s->deck_n; j++) s->deck[j - 1] = s->deck[j];
                s->deck_n--;
                s->deck_count = s->deck_n;
                sim_forced_i++;
                *out = want;
                return 1;
            }
        }
        sim_forced_i = sim_forced_n;   // divergence: rest of queue is stale
    }
    if (s->deck_n == 0) {
        if (!s->has_flipped) return 0;
        *out = s->flipped_id;
        s->has_flipped = 0;
        return 1;
    }
    int idx = (int)(game_random() * s->deck_n);
    if (idx < 0) idx = 0;
    if (idx >= s->deck_n) idx = s->deck_n - 1;
    *out = s->deck[idx];
    for (int i = idx + 1; i < s->deck_n; i++) s->deck[i - 1] = s->deck[i];
    s->deck_n--;
    s->deck_count = s->deck_n;
    return 1;
}

static void sim_eliminate(SimState *s, int p) {
    s->status_p[p] = PLAYER_STATUS_OUT;
    s->in_mask  &= ~(1u << p);
    s->out_mask |= (1u << p);
    s->elim_order[s->num_eliminated++] = (int8_t)p;
}

// refill_player_hands port.
static void sim_refill(SimState *s) {
    if (sim_no_cards_left(s)) {
        for (int i = 0; i < s->num_players; i++) {
            if ((s->in_mask >> i & 1u) && sim_hand_count(s, i) == 0)
                sim_eliminate(s, i);
        }
        return;
    }
    int defender = s->defender;
    if (sim_hand_count(s, defender) == 0) {
        while (sim_hand_count(s, defender) < CARDS_PER_PLAYER) {
            int c;
            if (!sim_draw(s, &c)) break;
            s->hand[defender] |= (1ull << c);
        }
    }
    int p_idx = s->first_attacker;
    int visited = 0;
    do {
        if (visited & (1 << p_idx)) break;
        visited |= (1 << p_idx);
        while (sim_hand_count(s, p_idx) < CARDS_PER_PLAYER) {
            int c;
            if (!sim_draw(s, &c)) break;
            s->hand[p_idx] |= (1ull << c);
        }
        if (sim_hand_count(s, p_idx) == 0 && (s->in_mask >> p_idx & 1u))
            sim_eliminate(s, p_idx);
        p_idx = sim_next_player(s, p_idx);
    } while (p_idx != s->first_attacker);
}

// ---------- action handlers (bitboard) ---------------------------------
// These mirror handle_attack/cover/pass/pickup/good exactly, modulo logging
// (the rollout never reads logs). Each takes an already-validated move.

static void sim_apply_attack(SimState *s, int p_idx, const uint8_t *ids, int n) {
    for (int i = 0; i < n; i++) {
        s->hand[p_idx] &= ~(1ull << ids[i]);
        int b = s->num_battles++;
        s->atk[b] = ids[i];
        s->covered_mask &= ~(1ull << b);
        s->table_vmask |= VALUE_MASK[id_value(ids[i])];
    }
    s->good_mask = 0;
    // Attackers only leave when the stock is exhausted too (mirrors the
    // no_cards_left guard in handle_attack); with cards still in the deck
    // they sit out the bout and refill at round end.
    if (sim_hand_count(s, p_idx) == 0 && sim_no_cards_left(s)) {
        sim_eliminate(s, p_idx);
    }
}

// cover all uncovered battles with the given (cover,attack-battle) assignment.
static void sim_apply_cover(SimState *s, int p_idx,
                            const uint8_t *covers, const int *battle_idx, int n) {
    for (int i = 0; i < n; i++) {
        int b = battle_idx[i];
        s->def[b] = covers[i];
        s->covered_mask |= (1ull << b);
        s->hand[p_idx] &= ~(1ull << covers[i]);
        s->table_vmask |= VALUE_MASK[id_value(covers[i])];
    }

    if (sim_hand_count(s, p_idx) == 0) {
        s->discard_pile_length += s->num_battles * 2;
        s->num_battles = 0;
        s->covered_mask = 0;
        s->table_vmask = 0;
        sim_refill(s);
        s->first_attacker = s->defender;
        s->good_mask = 0;
        if (sim_hand_count(s, s->first_attacker) == 0) {
            int fa = s->first_attacker;
            int was_in = (s->in_mask >> fa) & 1u;
            if (was_in) sim_eliminate(s, fa);
            else { s->status_p[fa] = PLAYER_STATUS_OUT; s->out_mask |= (1u << fa); }
            s->first_attacker = sim_next_player(s, fa);
        }
        s->defender = sim_next_player(s, s->first_attacker);
        return;
    }
    s->good_mask = 0;
    // all_covered handled implicitly: attackers re-enabled via good_mask reset.
}

static void sim_apply_pass(SimState *s, int p_idx, const uint8_t *ids, int n) {
    int next = sim_next_player(s, s->defender);
    for (int i = 0; i < n; i++) {
        s->hand[p_idx] &= ~(1ull << ids[i]);
        int b = s->num_battles++;
        s->atk[b] = ids[i];
        s->covered_mask &= ~(1ull << b);
        s->table_vmask |= VALUE_MASK[id_value(ids[i])];
    }
    s->good_mask = 0;
    if (sim_no_cards_left(s) && sim_hand_count(s, p_idx) == 0) {
        sim_eliminate(s, p_idx);
    }
    s->defender = next;
}

static void sim_apply_pickup(SimState *s, int p_idx) {
    for (int i = 0; i < s->num_battles; i++) {
        s->hand[p_idx] |= (1ull << s->atk[i]);
        if (s->covered_mask & (1ull << i)) s->hand[p_idx] |= (1ull << s->def[i]);
    }
    s->num_battles = 0;
    s->covered_mask = 0;
    s->table_vmask = 0;
    sim_refill(s);
    s->first_attacker = sim_next_player(s, s->defender);
    s->defender = sim_next_player(s, s->first_attacker);
    s->good_mask = 0;
}

static void sim_round_transition(SimState *s) {
    s->discard_pile_length += s->num_battles * 2;
    s->num_battles = 0;
    s->covered_mask = 0;
    s->table_vmask = 0;
    sim_refill(s);
    s->first_attacker = s->defender;
    s->defender = sim_next_player(s, s->first_attacker);
    s->good_mask = 0;
}

static void sim_apply_good(SimState *s, int p_idx) {
    s->good_mask |= (1u << p_idx);
    uint32_t attackers = s->in_mask & ~(1u << s->defender);
    int all_good = attackers != 0 && (s->good_mask & attackers) == attackers;
    if (all_good && sim_all_covered(s)) sim_round_transition(s);
}

// ---------- should_act (bitboard) --------------------------------------

static int sim_should_act(const SimState *s, int p) {
    if (s->status != GAME_STATUS_PLAYING) return 0;
    if (!(s->in_mask >> p & 1u)) return 0;
    int first_attack = (s->num_battles == 0);
    if (first_attack) return p == s->first_attacker;
    if (p == s->defender) return !sim_all_covered(s);
    return !(s->good_mask & (1u << p));
}

// ---------- handwritten policy, computed directly ----------------------
//
// Reproduces handwritten_strategy_choose given the LITE legal-move set. We
// compute the chosen move inline rather than enumerating moves. Each branch
// fills an out-move; the caller then applies it.

typedef enum { MV_ATTACK, MV_COVER, MV_PASS, MV_PICKUP, MV_GOOD, MV_NONE } SimMoveType;

typedef struct {
    SimMoveType type;
    int n;
    uint8_t cards[SIM_MAX_BATTLES];
    int     battle[SIM_MAX_BATTLES];   // cover only: which battle each cover defends
} SimMove;

// trump_attack_probability (matches handwritten).
static double sim_trump_attack_prob(const SimState *s) {
    if (s->deck_n > 0 || s->has_flipped) return 0.02;
    int table = 0;
    for (int i = 0; i < s->num_battles; i++)
        table += 1 + ((s->covered_mask & (1ull << i)) ? 1 : 0);
    int hands = 0;
    for (int i = 0; i < s->num_players; i++) hands += sim_hand_count(s, i);
    int total = s->deck_n + s->discard_pile_length + table + hands + (s->has_flipped ? 1 : 0);
    if (total < 1) total = 1;
    double ratio = (double)s->discard_pile_length / total;
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;
    double p = 0.65 + 0.35 * ratio;
    if (p < 0.5) p = 0.5;
    if (p > 0.95) p = 0.95;
    return p;
}

// Among value groups present in `avail` (a hand bitmask), restricted to the
// candidate value mask `cand_vals` and an optional non-trump filter, pick the
// handwritten "max cards, lowest summed score" attack group. `cap` limits the
// number of cards taken (defender capacity for first attacks; for regular
// attacks the lite enumerator does not cap, so cap = large). Returns chosen
// card ids in out[] and count; 0 if none.
//
// handwritten's pick_max_cards_lowest_score operates over the enumerated attack
// moves. For the first-attack set, moves are all k-subsets of each value group
// (k=1..gn). For each value, the maximal-cards same-value move uses all gn
// cards, summing to gn*value (lowest score is the full group). Across values,
// the maximum n_cards wins; ties broken by lowest summed score. Since cards in
// a value group are identical in value, the maximal move per value is "take all
// gn", and that's the only move of size gn for that value, so the cross-value
// comparison is: largest group size, ties -> lowest (value*size). For regular
// attacks the enumerated moves are k-subsets of table-valued cards across mixed
// values, but handwritten still selects max n_cards then lowest score; the
// max-cards attack is "all table-valued (non-trump) cards", whose value spread
// makes the score the sum. We replicate that: take ALL candidate cards (subject
// to the non-trump filter) for the max-cards move.
//
// IMPORTANT subtlety for regular attacks: the enumerated max-cards move is the
// full candidate set, but only same... no — calc_regular_attack_moves emits
// arbitrary k-subsets of all table-valued cards (mixed values allowed). The
// maximum-cards move is the full set. handwritten picks it. So the regular
// attack reduces to "play every table-valued (non-trump if any non-trump
// exists) card". We honor that. For FIRST attacks, moves are same-value only,
// so the answer is the largest single-value group.

static int sim_first_attack_group(const SimState *s, int p, int power,
                                  int non_trump_only, uint8_t *out) {
    uint64_t h = s->hand[p];
    if (non_trump_only) h &= ~SUIT_MASK[power];
    // The lite enumerator emits same-value combos k=1..gn, but only those with
    // defender_cards >= uncovered(=0) + k survive (emit_attack). So the maximum
    // EMITTED size of a value group is min(group_size, defcap). handwritten's
    // pick_max_cards_lowest_score then takes the largest emitted size, ties ->
    // lowest summed score (= lowest value, since same-value group). So compare
    // groups by their *capped* size, tie -> lowest value.
    int defcap = sim_hand_count(s, s->defender);
    if (defcap <= 0 || !h) return 0;
    // Visit only the DISTINCT VALUES present in the hand (typically 4-6)
    // instead of all 13: pop the lowest id, take its whole value group,
    // clear the group. Group order is by lowest-id, not ascending value, so
    // the explicit v-tie-break below carries the ordering exactly as before
    // (strict > keeps any first winner; equal eff resolves to lowest v).
    // hh&(hh-1) guarantees progress even on an out-of-range id.
    int best_v = -1, best_eff = 0;
    uint64_t hh = h;
    while (hh) {
        int v = id_value(ctz64(hh));
        uint64_t g = h & VALUE_MASK[v];
        hh = (hh & (hh - 1)) & ~g;
        int sz = popcnt64(g);
        int eff = sz < defcap ? sz : defcap;
        if (eff > best_eff || (eff == best_eff && v < best_v)) {
            best_eff = eff; best_v = v;
        }
    }
    if (best_v < 0) return 0;
    uint64_t g = h & VALUE_MASK[best_v];
    int n = 0;
    while (g && n < best_eff) { int id = ctz64(g); out[n++] = id; g &= g - 1; }
    return n;
}

// Regular (non-first) attack: play all table-valued cards (non-trump if any),
// capped by defender capacity (defender_cards >= uncovered + k). The core
// takes the candidate mask and capacity precomputed — the policy evaluates a
// non-trump and an all-cards variant of the SAME inputs, and computing them
// once per ply was measurably cheaper in the wasm profile.
static int sim_attack_group_core(uint64_t h, int defcap, int power, uint8_t *out) {
    if (!h) return 0;
    if (defcap <= 0) return 0;
    // handwritten picks max n_cards (full set) then lowest summed score. The
    // full set IS the max-cards move; we just take it (capped). To match
    // "lowest score" among equal max-card moves, when capped we must drop the
    // highest-score cards. Take lowest-score cards first.
    // Collect ids, sort by score ascending, take up to defcap.
    uint8_t ids[64]; int m = 0;
    uint64_t hh = h;
    while (hh) { int id = ctz64(hh); ids[m++] = id; hh &= hh - 1; }
    // The lite enumerator emits arbitrary subsets, and pick_max_cards_lowest
    // takes max-cards; when the full set fits (defcap >= m) it's the whole set.
    if (m <= defcap) {
        for (int i = 0; i < m; i++) out[i] = ids[i];
        return m;
    }
    // capped: choose the defcap lowest-score cards (insertion sort, small m).
    for (int i = 1; i < m; i++) {
        uint8_t key = ids[i]; int ks = id_score(key, power);
        int j = i - 1;
        while (j >= 0 && id_score(ids[j], power) > ks) { ids[j + 1] = ids[j]; j--; }
        ids[j + 1] = key;
    }
    for (int i = 0; i < defcap; i++) out[i] = ids[i];
    return defcap;
}

// Greedy lowest-cost full cover (matches calc_cover_moves_greedy + handwritten
// always picking the full cover). Returns 1 if a full cover exists.
static int sim_greedy_full_cover(const SimState *s, int p, int power, SimMove *out) {
    uint64_t avail = s->hand[p];
    int n = 0;
    for (int i = 0; i < s->num_battles; i++) {
        if (s->covered_mask & (1ull << i)) continue;
        int atk = s->atk[i];
        // Lowest-id_score cover, O(1): candidates are same-suit-higher cards
        // (score = value) and, for a non-trump attack, any trump (score =
        // value + 1000). A same-suit candidate always outranks a trump one,
        // and within a single suit ascending id IS ascending value, so ctz
        // picks exactly the first strict-min the old per-card scan kept.
        int best = -1;
        uint64_t same = HIGHER_MASK[atk] & avail;
        if (same) best = ctz64(same);
        else if (id_suit(atk) != power) {
            uint64_t tr = SUIT_MASK[power] & avail;
            if (tr) best = ctz64(tr);
        }
        if (best < 0) return 0;
        avail &= ~(1ull << best);
        out->cards[n] = (uint8_t)best;
        out->battle[n] = i;
        n++;
    }
    out->type = MV_COVER;
    out->n = n;
    return 1;
}

// Pass: defender, all battles uncovered, all same value, defender has matching
// cards. handwritten picks lowest summed score among pass moves; the lite
// enumerator emits k-subsets (k=1..mn) and handwritten's pass branch picks the
// single lowest-score... actually it sums; lowest sum = fewest cards = k=1
// lowest card. Wait: pass branch iterates all pass moves and picks min summed
// score. k=1 of the lowest matching card has the smallest sum. We replicate.
static int sim_pass_move(const SimState *s, int p, int power, SimMove *out) {
    // require: num_battles>0, none covered, all same attack value
    if (s->num_battles == 0) return 0;
    if (s->covered_mask) return 0;
    int v0 = id_value(s->atk[0]);
    for (int i = 1; i < s->num_battles; i++)
        if (id_value(s->atk[i]) != v0) return 0;
    uint64_t matching = s->hand[p] & VALUE_MASK[v0];
    if (!matching) return 0;
    int next = sim_next_player(s, s->defender);
    int next_cards = sim_hand_count(s, next);
    // pass move must satisfy next_cards >= k + num_battles. Lowest-sum pass =
    // smallest valid k. k=1: need next_cards >= 1 + num_battles.
    // handwritten picks min summed score over emitted pass moves. Among emitted
    // (k=1..mn capped by capacity), k=1 lowest card has min sum. Pick lowest
    // matching card if k=1 is legal; else the smallest legal k of lowest cards.
    int mn = popcnt64(matching);
    // find smallest k with next_cards >= k + num_battles, k in [1..mn]
    int kmax = next_cards - s->num_battles;
    if (kmax < 1) return 0;
    if (kmax > mn) kmax = mn;
    // lowest summed score => take the k lowest-score matching cards with the
    // smallest k that's legal. Smallest legal k is 1. But handwritten compares
    // ALL emitted pass moves by sum; k=1 with the lowest card always wins
    // (positive scores). So k=1, lowest-score matching card. All matching
    // cards share one value, so score only splits trump vs non-trump; the
    // old first-strict-min scan kept the lowest non-trump id when one
    // exists, else the (single) trump — exactly ctz on those masks.
    uint64_t nt = matching & ~SUIT_MASK[power];
    int best = ctz64(nt ? nt : matching);
    out->type = MV_PASS;
    out->n = 1;
    out->cards[0] = (uint8_t)best;
    return 1;
}

// Compute the handwritten move for actor p. Returns 1 with out filled, or 0.
static int sim_handwritten_move(SimState *s, int p, SimMove *out) {
    int power = s->power_suit;
    int first_attack = (s->num_battles == 0);
    int is_def = (p == s->defender);

    // --- attacker role (first attack OR continuing attack) ---
    int can_attack = 0;
    if (first_attack) can_attack = (p == s->first_attacker);
    else can_attack = (!is_def && !(s->good_mask & (1u << p)));

    // Regular-attack inputs, computed once for the non-trump and all-cards
    // variants below (and reused by the forced fallback).
    uint64_t h_tab = 0;
    int defcap = 0;
    if (can_attack && !first_attack) {
        h_tab = s->hand[p] & sim_table_value_mask(s);
        defcap = sim_hand_count(s, s->defender) - sim_count_uncovered(s);
    }

    if (can_attack) {
        uint8_t buf[MAX_HAND_SIZE];
        int n_nt;
        if (first_attack) {
            n_nt = sim_first_attack_group(s, p, power, 1, buf);
        } else {
            n_nt = sim_attack_group_core(h_tab & ~SUIT_MASK[power], defcap, power, buf);
        }
        // Attack branch: prefer non-trump attacks.
        if (n_nt > 0) {
            out->type = MV_ATTACK; out->n = n_nt;
            for (int i = 0; i < n_nt; i++) out->cards[i] = buf[i];
            return 1;
        }
        // No non-trump attack: trump attack under probability gate.
        uint8_t tbuf[MAX_HAND_SIZE];
        int n_tr;
        if (first_attack) n_tr = sim_first_attack_group(s, p, power, 0, tbuf);
        else              n_tr = sim_attack_group_core(h_tab, defcap, power, tbuf);
        if (n_tr > 0) {
            if (game_random() < sim_trump_attack_prob(s)) {
                out->type = MV_ATTACK; out->n = n_tr;
                for (int i = 0; i < n_tr; i++) out->cards[i] = tbuf[i];
                return 1;
            }
            // decline: prefer GOOD if available (attacker, not first attack &
            // not already good). For first attack, no GOOD exists -> falls to
            // forced attack fallback below.
            if (!first_attack) { out->type = MV_GOOD; out->n = 0; return 1; }
        }
        // Fall through to non-attack cascade (forced fallback handled below).
    }

    // --- defender role: pass, then full cover, then pickup ---
    if (is_def && s->num_battles > 0) {
        // handwritten checks pass first (n_passes), then cover, then pickup.
        SimMove pm;
        if (sim_pass_move(s, p, power, &pm)) { *out = pm; return 1; }
        SimMove cm;
        if (sim_greedy_full_cover(s, p, power, &cm)) { *out = cm; return 1; }
        // can't fully cover -> pickup
        out->type = MV_PICKUP; out->n = 0;
        return 1;
    }

    // --- non-attack/cover/pass: GOOD if available (attacker said-good path) ---
    // handwritten picks GOOD here via game_random()*n_goods; with a single GOOD
    // move the index is always 0, but the random draw IS consumed, so we mirror
    // it to keep the RNG stream aligned with the struct engine.
    if (!is_def && s->num_battles > 0 && !(s->good_mask & (1u << p))) {
        (void)game_random();
        out->type = MV_GOOD; out->n = 0;
        return 1;
    }

    // --- forced attack fallback (only reached by attacker who declined) ---
    if (can_attack) {
        uint8_t buf[MAX_HAND_SIZE];
        if (s->deck_n > 0 || s->has_flipped) {
            int n;
            if (first_attack) n = sim_first_attack_group(s, p, power, 1, buf);
            else              n = sim_attack_group_core(h_tab & ~SUIT_MASK[power], defcap, power, buf);
            if (n > 0) {
                out->type = MV_ATTACK; out->n = n;
                for (int i = 0; i < n; i++) out->cards[i] = buf[i];
                return 1;
            }
            if (!first_attack) { out->type = MV_GOOD; out->n = 0; return 1; }
        }
        // most-cards lowest-score among all attacks (incl trump).
        int n;
        if (first_attack) n = sim_first_attack_group(s, p, power, 0, buf);
        else              n = sim_attack_group_core(h_tab, defcap, power, buf);
        if (n > 0) {
            out->type = MV_ATTACK; out->n = n;
            for (int i = 0; i < n; i++) out->cards[i] = buf[i];
            return 1;
        }
    }

    return 0;
}

// ---------- playout ----------------------------------------------------

static void sim_apply(SimState *s, int p, const SimMove *m) {
    switch (m->type) {
        case MV_ATTACK: sim_apply_attack(s, p, m->cards, m->n); break;
        case MV_COVER:  sim_apply_cover(s, p, m->cards, m->battle, m->n); break;
        case MV_PASS:   sim_apply_pass(s, p, m->cards, m->n); break;
        case MV_PICKUP: sim_apply_pickup(s, p); break;
        case MV_GOOD:   sim_apply_good(s, p); break;
        default: break;
    }
}

// ---------- apply a root LegalMove on the SimState ---------------------
// Mirrors handle_attack/cover/pass/pickup/good's validation + effect closely
// enough for the bot's own candidate move (the bot's hand is identical across
// sampled worlds; only world-dependent capacity checks can fail).

int cd_sim_apply_root_move(SimState *s, int p_idx, const LegalMove *m) {
    int power = s->power_suit;
    switch (m->type) {
        case MOVE_ATTACK: {
            // capacity: defender_cards >= uncovered + n
            int uncovered = sim_count_uncovered(s);
            if (sim_hand_count(s, s->defender) < uncovered + m->n_cards) return 0;
            uint8_t ids[8];
            for (int i = 0; i < m->n_cards; i++) {
                int id = card_id(m->cards[i]);
                if (!(s->hand[p_idx] & (1ull << id))) return 0;
                ids[i] = (uint8_t)id;
            }
            sim_apply_attack(s, p_idx, ids, m->n_cards);
            return 1;
        }
        case MOVE_PASS: {
            if (s->num_battles == 0) return 0;
            if (s->covered_mask) return 0;
            int next = sim_next_player(s, s->defender);
            if (sim_hand_count(s, next) < m->n_cards + s->num_battles) return 0;
            uint8_t ids[8];
            for (int i = 0; i < m->n_cards; i++) {
                int id = card_id(m->cards[i]);
                if (!(s->hand[p_idx] & (1ull << id))) return 0;
                ids[i] = (uint8_t)id;
            }
            sim_apply_pass(s, p_idx, ids, m->n_cards);
            return 1;
        }
        case MOVE_COVER: {
            uint8_t covers[8]; int battle[8];
            uint64_t used_b = 0;   // battles already assigned within this move
            for (int i = 0; i < m->n_cards; i++) {
                int cid = card_id(m->cards[i]);
                int aid = card_id(m->attack_cards[i]);
                if (!(s->hand[p_idx] & (1ull << cid))) return 0;
                // find an uncovered, not-yet-assigned battle whose attack matches
                // (handle_cover matches by value; prefer exact id first).
                int found = -1;
                for (int b = 0; b < s->num_battles; b++) {
                    if ((s->covered_mask & (1ull << b)) || (used_b & (1ull << b))) continue;
                    if (s->atk[b] == aid) { found = b; break; }
                }
                if (found < 0) {
                    for (int b = 0; b < s->num_battles; b++) {
                        if ((s->covered_mask & (1ull << b)) || (used_b & (1ull << b))) continue;
                        if (id_value(s->atk[b]) == id_value(aid)) { found = b; break; }
                    }
                }
                if (found < 0) return 0;
                if (!id_can_cover(s->atk[found], cid, power)) return 0;
                used_b |= (1ull << found);
                covers[i] = (uint8_t)cid;
                battle[i] = found;
            }
            sim_apply_cover(s, p_idx, covers, battle, m->n_cards);
            return 1;
        }
        case MOVE_PICKUP:
            if (s->num_battles == 0) return 0;
            sim_apply_pickup(s, p_idx);
            return 1;
        case MOVE_GOOD:
            sim_apply_good(s, p_idx);
            return 1;
        default: return 0;
    }
}

// ===================================================================
// Exact 2-player deck-empty endgame solver (bitboard).
//
// Mirrors cd_solve in cordite_strategy.c node-for-node (same actor
// selection, the SAME full legal-move set, the same alpha-beta and the
// same ±(1000-depth) value encoding) but on the compact SimState, so a
// node clones with a memcpy and moves are enumerated with bit ops. A
// transposition table memoizes resolved subtrees (endgames transpose
// heavily). The value, when fully resolved, is identical to the struct
// solver's — validated by tests/solver_difftest.c. Only used when the
// deck is empty and 2 players are IN (no draws/refills can add cards).
// ===================================================================

// A solver move. Covers carry per-card target battle indices.
typedef struct {
    uint8_t type;      // MV_ATTACK / MV_COVER / MV_PASS / MV_PICKUP / MV_GOOD
    uint8_t n;
    uint8_t cards[8];
    uint8_t battle[8]; // cover only
} SolMove;

// ---- transposition table -------------------------------------------
// Keyed on the salient state (both hands, table, roles). Stores the exact
// resolved value (only EXACT entries; alpha-beta bound entries are not
// stored — the endgames are tiny so exact memoization is the win and it
// keeps the value provably correct regardless of the window it was found
// under). Cleared per top-level solve call.

// Build parameter. 16 = 65,536 entries x 16 B = 1 MiB (the historical value).
// The table is a memoization cache of EXACT endgame values, but the solver is
// node-budget-limited (SimSolver.budget), so a smaller table can cause more
// recomputation, exhaust the budget sooner, and change which move is chosen —
// it is a bot-strength knob, validated by comparing cnitro_eval win-rate /
// mean-finish / histogram across sizes (see docs/WASM_L1_BUDGET.md).
#ifndef CD_TT_BITS
#define CD_TT_BITS  16
#endif
#define CD_TT_SIZE  (1u << CD_TT_BITS)
#define CD_TT_MASK  (CD_TT_SIZE - 1u)

typedef struct {
    uint64_t key;     // full 64-bit fingerprint (0 => empty)
    int16_t  value;   // exact value at this node (me-perspective, abs depth-relative)
    uint8_t  depth;   // ply depth this value was computed at (value is depth-relative)
    uint8_t  valid;
} CdTTEntry;

typedef struct {
    long budget;
    int  aborted;
    int  me;
    int  order;   // move ordering: 0 gen-order, 2 big-first (desc), 3 short-first (asc)
    CdTTEntry *tt;
} SimSolver;

static _Thread_local CdTTEntry *cd_tt = NULL;

// -------- occupancy instrumentation (-DCD_TT_STATS; compiled out otherwise) --
// Measures the distribution of I = distinct keys inserted per clear-window
// (between cd_sim_solve_reset calls). At the default CD_TT_BITS=16 the table is
// effectively collision-free, so occupancy == true distinct-key count. This
// distribution drives the direct-mapped birthday-collision model that sizes a
// smaller table with a stated confidence bound (docs/WASM_L1_BUDGET.md).
#ifdef CD_TT_STATS
#define CD_STAT_MAXB 65537
long cd_stat_hist[CD_STAT_MAXB];   // hist[I] = #windows that inserted I distinct keys
long cd_stat_windows = 0;          // total clear-windows seen
long cd_stat_max_I = 0;            // largest I observed
long cd_stat_collisions = 0;       // evictions at store (should be ~0 at TT16)
// Store-census (for the working-set-reduction plan, docs/SOLVER_TT_WORKING_SET_PLAN.md):
//   ins_cards[c] = distinct-key insertions at nodes with c cards across both hands
//                  (run at TT22 so occupancy ~= distinct keys) — locates the W mass.
//   failhi/faillo = node completions whose value fell OUTSIDE the node's window
//                  (fail-soft bounds). Today these store NOTHING — each is thrown-
//                  away work a bounds-storing TT (CD_TT_BOUNDS) could cache.
long cd_stat_ins_cards[25];
long cd_stat_failhi = 0, cd_stat_faillo = 0;
static _Thread_local long cd_stat_occ = 0;
static _Thread_local long cd_stat_game_max = 0;  // largest window this game
void cd_tt_stats_dump(void) {
    fprintf(stderr, "CD_TT_STATS windows=%ld max_I=%ld collisions=%ld\n",
            cd_stat_windows, cd_stat_max_I, cd_stat_collisions);
    fprintf(stderr, "CD_TT_STATS2 failhi=%ld faillo=%ld\n", cd_stat_failhi, cd_stat_faillo);
    for (int i = 0; i < 25; i++)
        if (cd_stat_ins_cards[i]) fprintf(stderr, "CD_TT_CARDS %d %ld\n", i, cd_stat_ins_cards[i]);
    for (long i = 0; i < CD_STAT_MAXB; i++)
        if (cd_stat_hist[i]) fprintf(stderr, "CD_TT_HIST %ld %ld\n", i, cd_stat_hist[i]);
}
static _Thread_local int cd_stat_atexit = 0;
#endif

// Per-game working set, keyed by seed. W = the largest key-set that had to
// coexist in the table during one game (one window for the persist bots, the
// max single-solve window for the reset bots) — the quantity that must fit
// under M for the game to play like an unbounded table. main_eval calls this at
// each game's end and emits "GW <seed> <W>", so accumulation dedups on seed and
// never double-counts a re-measured game. Returns -1 when built without stats.
long cd_sim_stats_game_flush(void) {
#ifdef CD_TT_STATS
    if (cd_stat_occ > 0) {                 // flush the still-open final window
        long b = cd_stat_occ < CD_STAT_MAXB ? cd_stat_occ : CD_STAT_MAXB - 1;
        cd_stat_hist[b]++;
        cd_stat_windows++;
        if (cd_stat_occ > cd_stat_game_max) cd_stat_game_max = cd_stat_occ;
    }
    long w = cd_stat_game_max;
    cd_stat_game_max = 0;
    cd_stat_occ = 0;
    return w;
#else
    return -1;
#endif
}

static CdTTEntry *cd_tt_get(void) {
    if (!cd_tt) cd_tt = (CdTTEntry *)calloc(CD_TT_SIZE, sizeof(CdTTEntry));
#ifdef CD_TT_STATS
    if (!cd_stat_atexit) { cd_stat_atexit = 1; atexit(cd_tt_stats_dump); }
#endif
    return cd_tt;
}

// 64-bit fingerprint of the value-relevant state. Two players a<b are IN.
static uint64_t sim_fingerprint(const SimState *s, int a, int b) {
    uint64_t h = s->hand[a] * 0x9E3779B97F4A7C15ull;
    h ^= (s->hand[b] + 0x7F4A7C15ull) * 0xC2B2AE3D27D4EB4Full;
    uint64_t t = 0;
    for (int i = 0; i < s->num_battles; i++) {
        uint64_t cell = s->atk[i];
        if (s->covered_mask & (1ull << i)) cell |= ((uint64_t)s->def[i] << 8) | (1ull << 16);
        t = t * 1099511628211ull + (cell + 1);
    }
    h ^= t * 0xFF51AFD7ED558CCDull;
    h ^= (uint64_t)s->defender << 1;
    h ^= (uint64_t)s->first_attacker << 9;
    h ^= (uint64_t)(s->good_mask & 0xff) << 17;
    h ^= (uint64_t)s->num_battles << 25;
    h ^= 0x94D049BB133111EBull;
    h ^= h >> 31;
    return h ? h : 1;
}

#ifdef CD_TT_SUITSYM
// Suit-symmetry canonical fingerprint: the three NON-trump suits are
// interchangeable (a card's suit only matters via trump + same-suit covering),
// so a position and any permutation of its non-trump suits have the SAME game
// value. We key the TT on the canonical orbit representative = the minimum
// fingerprint over the 6 permutations of the non-trump suits. Every position in
// an orbit yields the same set of 6 fingerprints, hence the same min -> the 6
// equivalent positions collapse to ONE table entry. The stored value is
// permutation-invariant, so reuse across the orbit is exact and the search
// returns identical values (move-preserving). Cards are suit*13+rank; hands are
// 52-bit masks with one 13-bit block per suit.
static uint64_t sim_fingerprint_canon(const SimState *s, int a, int b) {
    static const int P[6][3] = {{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
    int power = s->power_suit;
    int nt[3], k = 0;
    for (int su = 0; su < 4; su++) if (su != power) nt[k++] = su;
    uint64_t best = ~0ull;
    for (int p = 0; p < 6; p++) {
        int map[4]; map[power] = power;
        map[nt[0]] = nt[P[p][0]]; map[nt[1]] = nt[P[p][1]]; map[nt[2]] = nt[P[p][2]];
        // remap a 52-bit hand: move each 13-bit suit block to map[suit]
        uint64_t ha = 0, hb = 0;
        for (int su = 0; su < 4; su++) {
            ha |= ((s->hand[a] >> (su * 13)) & 0x1FFFull) << (map[su] * 13);
            hb |= ((s->hand[b] >> (su * 13)) & 0x1FFFull) << (map[su] * 13);
        }
        uint64_t h = ha * 0x9E3779B97F4A7C15ull;
        h ^= (hb + 0x7F4A7C15ull) * 0xC2B2AE3D27D4EB4Full;
        uint64_t t = 0;
        for (int i = 0; i < s->num_battles; i++) {
            int ac = s->atk[i]; uint64_t cell = (uint64_t)(map[ac / 13] * 13 + ac % 13);
            if (s->covered_mask & (1ull << i)) {
                int dc = s->def[i];
                cell |= ((uint64_t)(map[dc / 13] * 13 + dc % 13) << 8) | (1ull << 16);
            }
            t = t * 1099511628211ull + (cell + 1);
        }
        h ^= t * 0xFF51AFD7ED558CCDull;
        h ^= (uint64_t)s->defender << 1;
        h ^= (uint64_t)s->first_attacker << 9;
        h ^= (uint64_t)(s->good_mask & 0xff) << 17;
        h ^= (uint64_t)s->num_battles << 25;
        h ^= 0x94D049BB133111EBull;
        h ^= h >> 31;
        h = h ? h : 1;
        if (h < best) best = h;
    }
    return best;
}
#endif

// The single attacker (the non-defender IN player) in a 2-player node.
static inline int sim_other_in(const SimState *s, int p) {
    for (int i = 0; i < s->num_players; i++)
        if (i != p && s->status_p[i] == PLAYER_STATUS_IN) return i;
    return -1;
}

// ---- move enumeration (full legal set, mirrors calculate_legal_moves) ----

// Same-value k-subset enumeration over a value-group bitmask, recursively.
// Each distinct subset of the (suit-distinct) cards is a distinct move.
static void enum_subsets(uint64_t group, int cap_lo, int cap_hi,
                         SolMove *buf, int *n, int max_n, uint8_t type) {
    // Enumerate all non-empty subsets of `group` whose popcount is in
    // [cap_lo, cap_hi]. group has few bits (a single value group, <=4).
    int ids[8], gn = 0;
    uint64_t g = group;
    while (g) { ids[gn++] = ctz64(g); g &= g - 1; }
    int total = 1 << gn;
    for (int mask = 1; mask < total && *n < max_n; mask++) {
        int k = __builtin_popcount(mask);
        if (k < cap_lo || k > cap_hi) continue;
        SolMove *m = &buf[(*n)++];
        m->type = type;
        m->n = (uint8_t)k;
        int c = 0;
        for (int i = 0; i < gn; i++) if (mask & (1 << i)) m->cards[c++] = (uint8_t)ids[i];
    }
}

// First-attack moves: all same-value k-subsets across value groups, capped
// by defender capacity (defcap >= k). Mirrors calc_first_attack_moves.
static int sim_gen_first_attack(const SimState *s, int p, SolMove *buf, int max_n) {
    int n = 0;
    int defcap = sim_hand_count(s, s->defender);
    uint64_t h = s->hand[p];
    for (int v = 1; v <= 13 && n < max_n; v++) {
        uint64_t group = h & VALUE_MASK[v];
        if (!group) continue;
        enum_subsets(group, 1, defcap, buf, &n, max_n, MV_ATTACK);
    }
    return n;
}

// Regular-attack moves: all k-subsets of table-valued cards, capped by
// defcap = defender_cards - uncovered. Mirrors calc_regular_attack_moves.
static int sim_gen_regular_attack(const SimState *s, int p, SolMove *buf, int max_n) {
    uint64_t tv = sim_table_value_mask(s);
    uint64_t h = s->hand[p] & tv;
    if (!h) return 0;
    int uncovered = sim_count_uncovered(s);
    int defcap = sim_hand_count(s, s->defender) - uncovered;
    if (defcap <= 0) return 0;
    int n = 0;
    int ids[16], hn = 0;
    uint64_t hh = h;
    while (hh) { ids[hn++] = ctz64(hh); hh &= hh - 1; }
    if (defcap > hn) defcap = hn;
    // all non-empty subsets of size 1..defcap (mixed values allowed)
    int total = 1 << hn;
    for (int mask = 1; mask < total && n < max_n; mask++) {
        int k = __builtin_popcount(mask);
        if (k > defcap) continue;
        SolMove *m = &buf[n++];
        m->type = MV_ATTACK;
        m->n = (uint8_t)k;
        int c = 0;
        for (int i = 0; i < hn; i++) if (mask & (1 << i)) m->cards[c++] = (uint8_t)ids[i];
    }
    return n;
}

// Pass moves: all same-value k-subsets of matching cards, capped by the
// next player's capacity (next_cards >= k + num_battles). Mirrors
// calc_pass_moves (all battles uncovered, all same value).
static int sim_gen_pass(const SimState *s, int p, SolMove *buf, int max_n) {
    if (s->num_battles == 0) return 0;
    if (s->covered_mask) return 0;
    int v0 = id_value(s->atk[0]);
    for (int i = 1; i < s->num_battles; i++)
        if (id_value(s->atk[i]) != v0) return 0;
    uint64_t matching = s->hand[p] & VALUE_MASK[v0];
    if (!matching) return 0;
    int next = sim_next_player(s, s->defender);
    int kmax = sim_hand_count(s, next) - s->num_battles;
    if (kmax < 1) return 0;
    int n = 0;
    enum_subsets(matching, 1, kmax, buf, &n, max_n, MV_PASS);
    return n;
}

// Cover moves: choose a non-empty subset of uncovered battles and, for each,
// a distinct covering card. Mirrors calc_cover_moves (choose_attack_subset +
// emit_cover_combo). Returns count; battles index into the battle array.
// For the chosen battle subset, assign distinct covers per battle.
static void cover_assign(const SimState *s, int power, const int *bidx, int pn,
                         int depth, uint8_t *chosen_card, uint64_t used,
                         SolMove *buf, int *n, int max_n) {
    if (depth == pn) {
        if (*n >= max_n) return;
        SolMove *m = &buf[(*n)++];
        m->type = MV_COVER;
        m->n = (uint8_t)pn;
        for (int i = 0; i < pn; i++) { m->cards[i] = chosen_card[i]; m->battle[i] = (uint8_t)bidx[i]; }
        return;
    }
    int atk = s->atk[bidx[depth]];
    uint64_t avail = s->hand[s->defender] & ~used;
    uint64_t a = avail;
    while (a && *n < max_n) {
        int id = ctz64(a); a &= a - 1;
        if (!id_can_cover(atk, id, power)) continue;
        chosen_card[depth] = (uint8_t)id;
        cover_assign(s, power, bidx, pn, depth + 1, chosen_card,
                     used | (1ull << id), buf, n, max_n);
    }
}

static int sim_gen_cover(const SimState *s, int power, SolMove *buf, int max_n) {
    int ubat[SIM_MAX_BATTLES], nub = 0;
    for (int i = 0; i < s->num_battles; i++)
        if (!(s->covered_mask & (1ull << i))) ubat[nub++] = i;
    if (nub == 0) return 0;
    int n = 0;
    int bidx[SIM_MAX_BATTLES];
    uint8_t chosen[SIM_MAX_BATTLES];
    // choose k battles to cover, k = 1..nub, in combination order
    for (int k = 1; k <= nub && n < max_n; k++) {
        // iterate combinations of nub choose k
        int comb[SIM_MAX_BATTLES];
        for (int i = 0; i < k; i++) comb[i] = i;
        while (n < max_n) {
            for (int i = 0; i < k; i++) bidx[i] = ubat[comb[i]];
            cover_assign(s, power, bidx, k, 0, chosen, 0, buf, &n, max_n);
            // next combination
            int i = k - 1;
            while (i >= 0 && comb[i] == nub - k + i) i--;
            if (i < 0) break;
            comb[i]++;
            for (int j = i + 1; j < k; j++) comb[j] = comb[j - 1] + 1;
        }
    }
    return n;
}

// Apply a SolMove to a SimState (uses the same handlers as the rollout).
static void sim_apply_sol(SimState *s, int p, const SolMove *m) {
    switch (m->type) {
        case MV_ATTACK: sim_apply_attack(s, p, m->cards, m->n); break;
        case MV_PASS:   sim_apply_pass(s, p, m->cards, m->n); break;
        case MV_PICKUP: sim_apply_pickup(s, p); break;
        case MV_GOOD:   sim_apply_good(s, p); break;
        case MV_COVER: {
            int bi[8];
            for (int i = 0; i < m->n; i++) bi[i] = m->battle[i];
            sim_apply_cover(s, p, m->cards, bi, m->n);
            break;
        }
    }
}

#define CD_SIM_SOLVE_MAX_DEPTH 48    // match the struct solver (CD_SOLVE_MAX_DEPTH)
#define CD_SOLVE_MOVES_CAP     96    // struct's CD_SOLVE_MAX_MOVES: abort when a
                                     // node has > this many legal moves, so the
                                     // resolved/aborted position SET — hence
                                     // play — is identical to the struct solver
#define CD_SIM_SOLVE_MAX_MOVES 160   // generation buffer (slack above the cap)

// Generate the full legal-move set for `actor`, mirroring
// calculate_legal_moves' branch selection.
static int sim_gen_moves(const SimState *s, int actor, SolMove *buf, int max_n) {
    int power = s->power_suit;
    int first_attack = (s->num_battles == 0);
    int is_def = (actor == s->defender);
    if (first_attack && actor == s->first_attacker) {
        return sim_gen_first_attack(s, actor, buf, max_n);
    } else if (is_def && s->num_battles > 0) {
        int n = sim_gen_cover(s, power, buf, max_n);
        if (!sim_all_covered(s) && n < max_n) {
            buf[n].type = MV_PICKUP; buf[n].n = 0; n++;
        }
        n += sim_gen_pass(s, actor, buf + n, max_n - n);
        return n;
    } else if (!is_def && s->num_battles > 0) {
        if (s->good_mask & (1u << actor)) return 0;
        int n = sim_gen_regular_attack(s, actor, buf, max_n);
        if (n < max_n) { buf[n].type = MV_GOOD; buf[n].n = 0; n++; }
        return n;
    }
    return 0;
}

// ---- TT thrash trace (-DCD_TT_TRACE; compiled out otherwise) --------------
// Emits a machine-readable exploration trace for ONE selected endgame-solve
// group (CD_TRACE_GROUP=<n>, group counter bumped per cd_sim_solve_reset), and
// optionally only the root moves whose applied table matches CD_TRACE_ROOT (a
// substring; empty = all). Lines (stderr):
//   SOLVE   g=<grp> call=<i> me=<me> d0=<d> a=<al> b=<be> budget=<b> root=<tbl>
//   SOLRET  g=<grp> call=<i> v=<v> aborted=<a> nodes=<n> budgetleft=<b>
//   ENTER   <nid> <pid> d=<depth> a=<alpha> b=<beta> edge=<move>
//   KEY     <nid> key=<hex> actor=<a> max=<0|1> nm=<nmoves>
//   RET     <nid> v=<value> why=<reason>
//   HIT     <nid> key=<hex> depth=<d> stored_depth=<sd> value=<v>
//   STORE   <nid> slot=<s> key=<hex> depth=<d>
//   EVICT   <nid> slot=<s> oldkey=<hex> olddepth=<d> newkey=<hex> newdepth=<d>
#ifdef CD_TT_TRACE
#include <stdlib.h>
#include <string.h>
static _Thread_local long cd_tr_group  = -1;   // current group (bump on reset)
static _Thread_local long cd_tr_target = -2;   // group to trace (-2 uninit)
static _Thread_local const char *cd_tr_rootf = (const char*)-1; // CD_TRACE_ROOT
static _Thread_local int  cd_tr_active = 0;     // this solve_d call is traced
static _Thread_local long cd_tr_nid    = 0;     // node id counter (per group)
static _Thread_local long cd_tr_parent = 0;     // current parent node id
static _Thread_local long cd_tr_nodes  = 0;     // nodes expanded this solve_d
static _Thread_local int  cd_tr_call   = 0;     // solve_d index within group
static _Thread_local char cd_tr_edge[24] = "root"; // pending inbound-edge label
static const char CD_TR_SUIT[4] = {'S','H','C','D'};
static const char *CD_TR_VAL[14] = {"?","2","3","4","5","6","7","8","9","10","J","Q","K","A"};
static void cd_tr_card(char *b, int id) {
    int v = id % 13 + 1, s = id / 13;
    snprintf(b, 8, "%s%c", (v>=1&&v<=13)?CD_TR_VAL[v]:"?", (s>=0&&s<4)?CD_TR_SUIT[s]:'?');
}
static void cd_tr_table(char *out, size_t n, const SimState *s) {
    out[0]=0; char c[8];
    for (int i=0;i<s->num_battles;i++){
        cd_tr_card(c,s->atk[i]); strncat(out,c,n-strlen(out)-1);
        if (s->covered_mask&(1ull<<i)){ strncat(out,"/",n-strlen(out)-1);
            cd_tr_card(c,s->def[i]); strncat(out,c,n-strlen(out)-1);} }
}
static void cd_tr_move(char *out, size_t n, const SolMove *m) {
    const char *t = m->type==MV_ATTACK?"A":m->type==MV_COVER?"C":
                    m->type==MV_PASS?"P":m->type==MV_PICKUP?"PU":
                    m->type==MV_GOOD?"G":"?";
    out[0]=0; strncat(out,t,n-1);
    for (int i=0;i<m->n;i++){ char c[8]; cd_tr_card(c,m->cards[i]);
        size_t l=strlen(out); if(l+1<n){ out[l]=(i?',':':'); out[l+1]=0;
            strncat(out,c,n-strlen(out)-1);} }
}
static int cd_tr_grp_match(void) {
    if (cd_tr_target == -2) { const char *e=getenv("CD_TRACE_GROUP"); cd_tr_target=e?atol(e):-1; }
    return cd_tr_target>=0 && cd_tr_group==cd_tr_target;
}
#define TR_RET(v, why) do { if (cd_tr_active) { fprintf(stderr,"RET %ld v=%d why=%s\n",_nid,(int)(v),why); cd_tr_parent=_pid; } return (v); } while (0)
#else
#define TR_RET(v, why) return (v)
#endif

// Move-ordering key: lower = tried first. For the maximizer we want quick
// wins; for both sides emptying the hand / ending the round tends to resolve
// fast. We sort attacks/covers by card count desc then score, pickup/good
// last. Ordering does not affect the value (full search), only speed.
static int sim_solve_rec(SimSolver *S, SimState *s, int alpha, int beta, int depth) {
#ifdef CD_TT_TRACE
    long _nid = 0, _pid = cd_tr_parent;
    if (cd_tr_active) {
        _nid = ++cd_tr_nid; cd_tr_nodes++;
        fprintf(stderr,"ENTER %ld %ld d=%d a=%d b=%d edge=%s\n",
                _nid,_pid,depth,alpha,beta,cd_tr_edge);
    }
#endif
    int loser = sim_done(s);
    if (loser >= 0) TR_RET((loser == S->me) ? -(1000 - depth) : (1000 - depth), "term");
    int incount = sim_in_count(s);
    if (incount == 0) TR_RET(0, "draw");
    if (depth >= CD_SIM_SOLVE_MAX_DEPTH) { S->aborted = 1; TR_RET(0, "maxdepth"); }
    if (--S->budget <= 0) { S->aborted = 1; TR_RET(0, "budget"); }

    // actor: defender-priority, then first IN actor (mirrors cd_solve).
    int actor = -1;
    if (sim_should_act(s, s->defender)) actor = s->defender;
    else {
        for (int i = 0; i < s->num_players; i++)
            if (sim_should_act(s, i)) { actor = i; break; }
    }
    if (actor < 0) TR_RET(0, "noactor");

    // Two players for the fingerprint (the only IN pair in an endgame).
    int a = -1, b = -1;
    for (int i = 0; i < s->num_players; i++)
        if (s->status_p[i] == PLAYER_STATUS_IN) { if (a < 0) a = i; else b = i; }
    uint64_t key = 0;
    CdTTEntry *e = NULL;
    if (b >= 0) {
#ifdef CD_TT_SUITSYM
        key = sim_fingerprint_canon(s, a, b);
#else
        key = sim_fingerprint(s, a, b);
#endif
#ifdef CD_TT_2WAY
        // 2-way set associativity: each aligned slot pair is one bucket (both
        // entries share a 64-byte cache line at 16 B/entry). Probe both halves;
        // hit on either key. On a miss `e` is left pointing at the bucket's low
        // slot so the store path still fires — the real victim is re-chosen from
        // live bucket contents at the store site below.
        {
            CdTTEntry *bkt = &S->tt[key & CD_TT_MASK & ~1ull];
            if (bkt[0].valid && bkt[0].key == key)      e = &bkt[0];
            else if (bkt[1].valid && bkt[1].key == key) e = &bkt[1];
            else                                        e = &bkt[0];
        }
#else
        e = &S->tt[key & CD_TT_MASK];
#endif
        if (e->valid && e->key == key) {
            // stored value is depth-relative to e->depth; re-base to this depth.
            int v = e->value;
            if (v > 0) v = v - (1000 - e->depth) + (1000 - depth);
            else if (v < 0) v = v + (1000 - e->depth) - (1000 - depth);
#ifdef CD_TT_TRACE
            if (cd_tr_active)
                fprintf(stderr,"HIT %ld key=%llx depth=%d stored_depth=%d value=%d\n",
                        _nid,(unsigned long long)key,depth,(int)e->depth,v);
#endif
            TR_RET(v, "tthit");
        }
    }
#ifdef CD_TT_TRACE
    if (cd_tr_active)
        fprintf(stderr,"KEY %ld key=%llx actor=%d max=%d\n",
                _nid,(unsigned long long)key,actor,(actor==S->me));
#endif

    SolMove moves[CD_SIM_SOLVE_MAX_MOVES];
    int nm = sim_gen_moves(s, actor, moves, CD_SIM_SOLVE_MAX_MOVES);
    if (nm == 0) TR_RET(0, "nomoves");
    // Mirror the struct solver's `mv->n > CD_SOLVE_MAX_MOVES` bail: abort on
    // nodes with more than the cap legal moves, so we resolve/abort the exact
    // same position set as the struct solver (identical play). The buffer has
    // slack above the cap so a true count above it is still detected.
    if (nm > CD_SOLVE_MOVES_CAP) { S->aborted = 1; TR_RET(0, "movecap"); }

    // Move ordering (insertion sort, nm small), runtime-selected by S->order:
    //   2 = big-first (descending by card count) — aggressive cutoffs, small W,
    //       but dives into deep lines that can trip the ply-48 abort.
    //   3 = short-line-first (ascending) — round-enders/fewest cards first, so
    //       short lines resolve before the search goes deep (fuller, bigger W).
    //   0 = generation order. CD_TT_ADAPT runs 2, and on abort re-solves with 3.
    if (S->order) {
        int desc = (S->order == 2);
        for (int i = 1; i < nm; i++) {
            SolMove kv = moves[i];
            int ki = (kv.n == 0) ? -1 : kv.n;
            int j = i - 1;
            while (j >= 0) {
                int kj = (moves[j].n == 0) ? -1 : moves[j].n;
                if (desc ? (kj >= ki) : (kj <= ki)) break;
                moves[j + 1] = moves[j]; j--;
            }
            moves[j + 1] = kv;
        }
    }

    int alpha0 = alpha, beta0 = beta;   // original window for exactness test
    int maximizing = (actor == S->me);
    int best = maximizing ? -2000 : 2000;
    int applied = 0;
    SimState child;
#ifdef CD_TT_TRACE
    long _saved_parent = cd_tr_parent;
    if (cd_tr_active) cd_tr_parent = _nid;
#endif
    for (int i = 0; i < nm; i++) {
        // Copy everything EXCEPT the deck[] tail: the solver only ever runs on
        // deck-empty endgames (entered at deck_n==0 && !has_flipped) and nothing
        // under it draws (sim_apply_sol never refills), so deck[] is dead here.
        // Skipping the 64B tail shaves ~20% off this per-node clone — the single
        // hottest memmove in the semtex/octogen/cordite MC profile. deck_n (=0)
        // sits before deck[] and is still copied, so movegen sees a valid count.
        memcpy(&child, s, offsetof(SimState, deck));
        sim_apply_sol(&child, actor, &moves[i]);
        applied = 1;
#ifdef CD_TT_TRACE
        if (cd_tr_active) cd_tr_move(cd_tr_edge, sizeof(cd_tr_edge), &moves[i]);
#endif
        int v = sim_solve_rec(S, &child, alpha, beta, depth + 1);
        if (S->aborted) {
#ifdef CD_TT_TRACE
            cd_tr_parent = _saved_parent;
#endif
            TR_RET(0, "abort");
        }
        if (maximizing) {
            if (v > best) best = v;
            if (best > alpha) alpha = best;
        } else {
            if (v < best) best = v;
            if (best < beta) beta = best;
        }
        if (alpha >= beta) break;
    }
#ifdef CD_TT_TRACE
    if (cd_tr_active) cd_tr_parent = _saved_parent;
#endif
    if (!applied || best == -2000 || best == 2000) TR_RET(0, "unresolved");

    // Memoize EXACT values only. A fail-soft alpha-beta result `best` is the
    // true game value only when it lands strictly INSIDE the original window
    // (alpha0 < best < beta0). On a fail-low (best <= alpha0) it is only an
    // upper bound; on a fail-high (best >= beta0) only a lower bound — storing
    // either as exact would corrupt a later lookup under a wider window. So we
    // store solely the exact case; bound nodes are simply not memoized.
#ifdef CD_TT_STATS
    if (e && key) {   // census: how much completed work falls outside the window
        if (best >= beta0) cd_stat_failhi++;
        else if (best <= alpha0) cd_stat_faillo++;
    }
#endif
    if (e && key && best > alpha0 && best < beta0) {
        int store = 1;
#ifdef CD_TT_2WAY
        // Re-choose the victim from the bucket's LIVE contents (children may have
        // filled it since our probe): same-key slot, else an empty slot, else evict
        // the deeper-ply entry (bigger depth = smaller subtree = cheaper to redo).
        // Always store — refusal is the pathology that made 1-way DEPTH_PREF regress.
        {
            CdTTEntry *bkt = &S->tt[key & CD_TT_MASK & ~1ull];
            if (bkt[0].valid && bkt[0].key == key)      e = &bkt[0];
            else if (bkt[1].valid && bkt[1].key == key) e = &bkt[1];
            else if (!bkt[0].valid)                     e = &bkt[0];
            else if (!bkt[1].valid)                     e = &bkt[1];
            else e = (bkt[0].depth >= bkt[1].depth) ? &bkt[0] : &bkt[1];
        }
#endif
#ifdef CD_TT_DEPTH_PREF
        // Depth-preferred replacement: on a collision with a DIFFERENT key, keep
        // whichever entry is closer to the root (lower ply = larger subtree below
        // it = costliest to recompute). Empty slots and same-key refreshes always
        // store. Aims to cut the eviction thrashing that perturbs move choice at
        // small table sizes, at zero extra memory (one comparison).
        if (e->valid && e->key != key && (uint8_t)depth > e->depth) store = 0;
#endif
        if (store) {
#ifdef CD_TT_STATS
            if (!e->valid) {
                cd_stat_occ++; if (cd_stat_occ > cd_stat_max_I) cd_stat_max_I = cd_stat_occ;
                int tc = __builtin_popcountll(s->hand[a]) + __builtin_popcountll(s->hand[b]);
                if (tc > 24) tc = 24;
                cd_stat_ins_cards[tc]++;   // census: which layer the distinct keys live in
            }
            else if (e->key != key) cd_stat_collisions++;   // eviction — must stay ~0 at TT16
#endif
#ifdef CD_TT_TRACE
            if (cd_tr_active) {
                unsigned slot = (unsigned)(key & CD_TT_MASK);
                if (e->valid && e->key != key)
                    fprintf(stderr,"EVICT %ld slot=%u oldkey=%llx olddepth=%d newkey=%llx newdepth=%d\n",
                            _nid,slot,(unsigned long long)e->key,(int)e->depth,
                            (unsigned long long)key,depth);
                else
                    fprintf(stderr,"STORE %ld slot=%u key=%llx depth=%d\n",
                            _nid,slot,(unsigned long long)key,depth);
            }
#endif
            e->key = key;
            e->value = (int16_t)best;
            e->depth = (uint8_t)depth;
            e->valid = 1;
        }
    }
    TR_RET(best, "exact");
}

// Public entry: exact value of position `s` from `me`'s perspective, with the
// given window and budget. Returns the value; sets *aborted if budget/depth
// blew. Clears the TT each call (positions across calls are unrelated).
int cd_sim_solve(SimState *s, int me, int alpha, int beta, long budget, int *aborted) {
    long b = budget;
    return cd_sim_solve_d(s, me, alpha, beta, &b, 0, aborted);
}

// As cd_sim_solve, but starting the depth counter at `depth0` so the value
// encoding (±(1000-depth)) lines up with a caller that already applied a move
// (e.g. the root win-hunt evaluates children at depth 1). `*budget` is the
// shared remaining node budget: it is decremented by the nodes this call
// expanded so a caller can drain one budget across many root moves (matching
// the struct solver's shared-budget semantics).
int cd_sim_solve_d(SimState *s, int me, int alpha, int beta, long *budget,
                   int depth0, int *aborted) {
    SimSolver S;
    long budget0 = *budget;
    S.budget = *budget;
    S.aborted = 0;
    S.me = me;
    S.order = 0;
#if defined(CD_TT_ADAPT) || defined(CD_TT_ORDER2)
    S.order = 2;              // big-first by default
#elif defined(CD_TT_ORDER3)
    S.order = 3;
#endif
    S.tt = cd_tt_get();
    if (!S.tt) { if (aborted) *aborted = 1; return 0; }
#ifdef CD_TT_TRACE
    int _call = cd_tr_call++;
    long _nodes0 = cd_tr_nodes;
    char _tbl[128]; cd_tr_table(_tbl, sizeof(_tbl), s);
    if (cd_tr_grp_match()) {
        if (cd_tr_rootf == (const char*)-1) cd_tr_rootf = getenv("CD_TRACE_ROOT");
        cd_tr_active = (!cd_tr_rootf || !*cd_tr_rootf || strstr(_tbl, cd_tr_rootf) != NULL);
        strcpy(cd_tr_edge, "root");
        if (cd_tr_active) {
            char _h0[80], _h1[80]; int _p1=-1;
            for (int _i=0;_i<s->num_players;_i++) if (_i!=me && s->status_p[_i]==PLAYER_STATUS_IN){_p1=_i;break;}
            _h0[0]=0; _h1[0]=0;
            { char c[8]; for(uint64_t x=s->hand[me];x;x&=x-1){int id=__builtin_ctzll(x);cd_tr_card(c,id);size_t l=strlen(_h0);if(l){_h0[l]=',';_h0[l+1]=0;}strncat(_h0,c,sizeof(_h0)-strlen(_h0)-1);} }
            if(_p1>=0){ char c[8]; for(uint64_t x=s->hand[_p1];x;x&=x-1){int id=__builtin_ctzll(x);cd_tr_card(c,id);size_t l=strlen(_h1);if(l){_h1[l]=',';_h1[l+1]=0;}strncat(_h1,c,sizeof(_h1)-strlen(_h1)-1);} }
            fprintf(stderr,"SOLVE g=%ld call=%d me=%d opp=%d power=%d def=%d fa=%d d0=%d a=%d b=%d budget=%ld root=[%s] h%d=[%s] h%d=[%s]\n",
                    cd_tr_group,_call,me,_p1,s->power_suit,s->defender,s->first_attacker,
                    depth0,alpha,beta,*budget,_tbl,me,_h0,_p1,_h1);
        }
    } else cd_tr_active = 0;
#endif
    int v = sim_solve_rec(&S, s, alpha, beta, depth0);
#ifdef CD_TT_ADAPT
    // big-first (order 2) bailed on a deep line -> re-solve this position with the
    // fuller short-line-first order (3) and a fresh budget, so we resolve it
    // instead of dropping to the Monte-Carlo fallback. Aborts are rare, so the
    // common path stays cheap (small W) while the knife-edge games stay correct.
    if (S.aborted && S.order == 2) {
        S.budget = budget0; S.aborted = 0; S.order = 0;   // fall back to the reliable std order
        v = sim_solve_rec(&S, s, alpha, beta, depth0);
    }
#endif
    *budget = S.budget;
    if (aborted) *aborted = S.aborted;
#ifdef CD_TT_TRACE
    if (cd_tr_active)
        fprintf(stderr,"SOLRET g=%ld call=%d v=%d aborted=%d nodes=%ld budgetleft=%ld\n",
                cd_tr_group,_call,v,S.aborted,cd_tr_nodes-_nodes0,*budget);
    // Always-on lightweight per-move summary (group discovery, all groups).
    fprintf(stderr,"SUM g=%ld call=%d me=%d a=%d b=%d v=%d aborted=%d budgetleft=%ld root=[%s]\n",
            cd_tr_group,_call,me,alpha,beta,v,S.aborted,*budget,_tbl);
    cd_tr_active = 0;
#endif
    return v;
}

void cd_sim_solve_reset(void) {
#ifdef CD_TT_STATS
    // record the window that just closed (occ = distinct keys inserted since
    // the previous reset), then start a fresh window.
    if (cd_stat_occ > 0) {
        long b = cd_stat_occ < CD_STAT_MAXB ? cd_stat_occ : CD_STAT_MAXB - 1;
        cd_stat_hist[b]++;
        cd_stat_windows++;
        if (cd_stat_occ > cd_stat_game_max) cd_stat_game_max = cd_stat_occ;
    }
    cd_stat_occ = 0;
#endif
#ifdef CD_TT_TRACE
    cd_tr_group++;
    cd_tr_call = 0;
    if (cd_tr_grp_match()) { cd_tr_nid = 0; cd_tr_parent = 0; }
#endif
    if (cd_tt) memset(cd_tt, 0, CD_TT_SIZE * sizeof(CdTTEntry));
}

// Single-step (test hook): advance one actor; returns the actor index or -1.
int cd_sim_one_step(SimState *s) {
    for (int pi = 0; pi < s->num_players; pi++) {
        if (!sim_should_act(s, pi)) continue;
        SimMove m;
        if (!sim_handwritten_move(s, pi, &m)) continue;
        sim_apply(s, pi, &m);
        return pi;
    }
    return -1;
}

// ---------- per-seat-policy playout (semtex) -----------------------------
// POL_HW seats play the handwritten policy. POL_LOOSE seats play a weak
// "random-ish" opponent model: random attack leads (no lowest-first), random
// covers instead of cheapest (burning trumps freely), occasional needless
// pickups, and no trump conservation. Rolling a profiled-weak seat out with
// this model instead of handwritten is the fulminate lever: value estimates
// against weak opponents stop assuming they play well.

// Uniformly random set bit of `mask` (consumes one game_random() draw).
static int sim_random_card(uint64_t mask) {
    int n = popcnt64(mask);
    if (!n) return -1;
    int k = (int)(game_random() * n);
    if (k < 0) k = 0;
    if (k >= n) k = n - 1;
    while (k--) mask &= mask - 1;
    return ctz64(mask);
}

static int sim_loose_move(SimState *s, int p, SimMove *out) {
    int power = s->power_suit;
    int first = (s->num_battles == 0);
    int is_def = (p == s->defender);

    if (is_def && s->num_battles > 0) {
        // Occasional needless pickup (weak players give up early).
        if (game_random() < 0.10) { out->type = MV_PICKUP; out->n = 0; return 1; }
        // Pass half the time it's available, with a random matching card.
        SimMove pm;
        if (sim_pass_move(s, p, power, &pm)) {
            if (game_random() < 0.5) {
                int v0 = id_value(s->atk[0]);
                int id = sim_random_card(s->hand[p] & VALUE_MASK[v0]);
                if (id >= 0) pm.cards[0] = (uint8_t)id;
                *out = pm;
                return 1;
            }
        }
        // Random cover per battle (not cheapest — wasteful trumping included).
        uint64_t avail = s->hand[p];
        int n = 0;
        for (int i = 0; i < s->num_battles; i++) {
            if (s->covered_mask & (1ull << i)) continue;
            uint64_t cov = 0, a = avail;
            while (a) {
                int id = ctz64(a); a &= a - 1;
                if (id_can_cover(s->atk[i], id, power)) cov |= 1ull << id;
            }
            int pick = sim_random_card(cov);
            if (pick < 0) { out->type = MV_PICKUP; out->n = 0; return 1; }
            avail &= ~(1ull << pick);
            out->cards[n] = (uint8_t)pick;
            out->battle[n] = i;
            n++;
        }
        out->type = MV_COVER; out->n = n;
        return 1;
    }

    int can_attack = first ? (p == s->first_attacker)
                           : (!is_def && !(s->good_mask & (1u << p)));
    if (can_attack) {
        if (first) {
            if (sim_hand_count(s, s->defender) >= 1) {
                int id = sim_random_card(s->hand[p]);
                if (id >= 0) {
                    out->type = MV_ATTACK; out->n = 1; out->cards[0] = (uint8_t)id;
                    return 1;
                }
            }
        } else {
            int uncovered = sim_count_uncovered(s);
            int defcap = sim_hand_count(s, s->defender) - uncovered;
            uint64_t tv = sim_table_value_mask(s) & s->hand[p];
            if (defcap >= 1 && tv && game_random() < 0.6) {
                int id = sim_random_card(tv);
                out->type = MV_ATTACK; out->n = 1; out->cards[0] = (uint8_t)id;
                return 1;
            }
            out->type = MV_GOOD; out->n = 0;
            return 1;
        }
    }
    if (!is_def && s->num_battles > 0 && !(s->good_mask & (1u << p))) {
        out->type = MV_GOOD; out->n = 0;
        return 1;
    }
    return 0;
}

// MC-defender model (CD_POL_MCDEF): handwritten EXCEPT the defender's
// cover decision — when the greedy full cover would spend a trump while the
// deck is still alive, the seat picks up instead half the time. Handwritten
// NEVER picks up while holding a full cover, but MC bots (cordite/semtex)
// and thinking humans do it constantly to protect trumps — the same
// behavior the mc_tell belief evidence detects. Rolling proven-strategic
// seats out with this model instead of pure handwritten removes that bias
// from every value estimate at zero extra playout cost.
static int sim_mcdef_move(SimState *s, int p, SimMove *out) {
    if (p == s->defender && s->num_battles > 0 && !sim_all_covered(s)
        && (s->deck_n > 0 || s->has_flipped)) {
        SimMove pm;
        if (sim_pass_move(s, p, s->power_suit, &pm)) { *out = pm; return 1; }
        SimMove cm;
        if (sim_greedy_full_cover(s, p, s->power_suit, &cm)) {
            int trumps = 0;
            for (int i = 0; i < cm.n; i++)
                if (id_suit(cm.cards[i]) == s->power_suit) trumps++;
            if (trumps > 0 && game_random() < 0.5) {
                out->type = MV_PICKUP; out->n = 0;
                return 1;
            }
            *out = cm;
            return 1;
        }
        out->type = MV_PICKUP; out->n = 0;
        return 1;
    }
    return sim_handwritten_move(s, p, out);
}

// Playout where each seat plays its own policy (pol[p] = CD_POL_*), with
// optional exact leaf endgames (leaf_cards > 0). pol == NULL means all
// handwritten, matching cd_sim_playout_leaf / cd_sim_playout exactly.
int cd_sim_playout_pol(SimState *s, int my_idx, int max_turns, int early_exit,
                       int leaf_cards, long leaf_budget, const uint8_t *pol) {
    int turns = 0;
    int leaf_tried = 0;
    while (sim_done(s) < 0 && turns++ < max_turns) {
        if (early_exit && s->status_p[my_idx] != PLAYER_STATUS_IN) {
            for (int i = 0; i < s->num_eliminated; i++)
                if (s->elim_order[i] == my_idx) return i + 1;
            break;
        }
        if (leaf_cards > 0 && !leaf_tried && s->deck_n == 0 && !s->has_flipped) {
            int a = -1, b = -1;
            for (int i = 0; i < s->num_players; i++) {
                if (s->status_p[i] != PLAYER_STATUS_IN) continue;
                if (a < 0) a = i; else if (b < 0) b = i; else { b = -2; break; }
            }
            if (a >= 0 && b >= 0) {
                int total = __builtin_popcountll(s->hand[a])
                          + __builtin_popcountll(s->hand[b]);
                for (int i = 0; i < s->num_battles; i++)
                    total += 1 + ((s->covered_mask >> i) & 1);
                if (total <= leaf_cards) {
                    leaf_tried = 1;
                    int aborted = 0;
                    long budget = leaf_budget;
                    int v = cd_sim_solve_d(s, a, -1, 1, &budget, 0, &aborted);
                    if (!aborted && v != 0) {
                        int loser = (v < 0) ? a : b;
                        int np = s->num_players;
                        if (my_idx == loser) return np;
                        if (my_idx == a || my_idx == b) return np - 1;
                        for (int i = 0; i < s->num_eliminated; i++)
                            if (s->elim_order[i] == my_idx) return i + 1;
                        return np - 1;
                    }
                }
            }
        }
        int acted = 0;
        for (int pi = 0; pi < s->num_players; pi++) {
            if (!sim_should_act(s, pi)) continue;
            SimMove m;
            int got = (pol && pol[pi] == CD_POL_LOOSE)
                    ? sim_loose_move(s, pi, &m)
                    : (pol && pol[pi] == CD_POL_MCDEF)
                    ? sim_mcdef_move(s, pi, &m)
                    : sim_handwritten_move(s, pi, &m);
            if (!got) continue;
            sim_apply(s, pi, &m);
            acted = 1;
            break;
        }
        if (!acted) break;
    }
    if (sim_done(s) < 0) return 0;
    for (int i = 0; i < s->num_eliminated; i++)
        if (s->elim_order[i] == my_idx) return i + 1;
    return s->num_players;
}

// ---------- reply-tournament playout (octogen) ---------------------------
// The FIRST opponent decision of the playout is chosen by SEARCH instead of
// the rollout policy: enumerate the actor's full legal reply set (the
// solver's bitboard move-gen), play each candidate reply out to completion,
// and let the opponent take the reply with the best outcome FOR THEM (their
// own finish position; ties -> the cheapest-ranked reply, matching the
// cheap-first convention). Returns MY finish under that reply. This models
// "the opponent punishes this move" one ply deep — the classic determinized-
// MC blind spot where a fixed rollout policy never plays the refutation.
// Cost: up to reply_cap full playouts instead of one, so callers use it only
// on late-stage (few-candidate) worlds.

// Rank key for pruning oversized reply sets: same family ordering cordite's
// candidate picker uses (attacks max-cards-cheapest, covers cheapest,
// passes cheapest, then good/pickup — which are always kept).
static double sol_rank_key(const SolMove *m, int power) {
    switch (m->type) {
        case MV_ATTACK: {
            int sum = 0;
            for (int i = 0; i < m->n; i++) sum += id_score(m->cards[i], power);
            return -(double)m->n * 10000.0 + (double)sum;
        }
        case MV_COVER: {
            double prod = 1.0;
            for (int i = 0; i < m->n; i++) prod *= (double)id_score(m->cards[i], power);
            return 100000.0 + prod;
        }
        case MV_PASS: {
            int sum = 0;
            for (int i = 0; i < m->n; i++) sum += id_score(m->cards[i], power);
            return 200000.0 + (double)sum;
        }
        case MV_GOOD:   return 300000.0;
        default:        return 300001.0;   // MV_PICKUP
    }
}

// Finish position of `p` after a TERMINATED playout: eliminated players by
// slot, the one remaining IN player (the durak) gets N.
static int sim_pos_of(const SimState *s, int p) {
    for (int i = 0; i < s->num_eliminated; i++)
        if (s->elim_order[i] == p) return i + 1;
    return s->num_players;
}

extern uint32_t game_rng_get(void);
extern void game_rng_set(uint32_t s);

int cd_sim_playout_reply(SimState *s, int my_idx, int max_turns,
                         int leaf_cards, long leaf_budget,
                         const uint8_t *pol, int reply_cap) {
    // Advance with the policy while it is still MY move (or forced steps),
    // for a handful of plies, until the DEFENDER's reply to the attack
    // surfaces. Only the defender's cover/pass/pickup decision is searched:
    // the defender makes that choice from information they genuinely have
    // (their own hand + the visible attack), so the in-world best reply is a
    // realistic model of their actual play. Searching OTHER reply types
    // (e.g. an opponent's next attack) uses the sampled hidden cards the
    // real opponent cannot see — paranoid distortion, measured harmful.
    for (int guard = 0; guard < 8; guard++) {
        if (sim_done(s) >= 0)
            return cd_sim_playout_pol(s, my_idx, max_turns, 1,
                                      leaf_cards, leaf_budget, pol);
        int actor = -1;
        for (int pi = 0; pi < s->num_players; pi++)
            if (sim_should_act(s, pi)) { actor = pi; break; }
        if (actor < 0) break;
        if (actor != my_idx && actor == s->defender && s->num_battles > 0) {
            // The reply decision. Enumerate + tournament.
            SolMove buf[CD_SIM_SOLVE_MAX_MOVES];
            int n = sim_gen_moves(s, actor, buf, CD_SIM_SOLVE_MAX_MOVES);
            if (n <= 1) {
                if (n == 1) sim_apply_sol(s, actor, &buf[0]);
                else break;   // no reply moves: defer to the policy playout
                continue;
            }
            // Rank cheap-first; keep the top reply_cap.
            int order[CD_SIM_SOLVE_MAX_MOVES];
            double key[CD_SIM_SOLVE_MAX_MOVES];
            for (int i = 0; i < n; i++) {
                order[i] = i;
                key[i] = sol_rank_key(&buf[i], s->power_suit);
            }
            for (int i = 1; i < n; i++) {   // insertion sort, small n
                int oi = order[i]; double ki = key[oi];
                int j = i - 1;
                while (j >= 0 && key[order[j]] > ki) { order[j+1] = order[j]; j--; }
                order[j+1] = oi;
            }
            // Keep the top reply_cap cheap-first replies, but PICKUP and
            // GOOD (which rank last) are always searched — "just take the
            // cards" is the defender's most realistic fallback and must not
            // be pruned by a large cover-combination set.
            int kept_idx[CD_SIM_SOLVE_MAX_MOVES];
            int kept = 0;
            for (int k = 0; k < n && kept < reply_cap; k++) {
                uint8_t t = buf[order[k]].type;
                if (t == MV_PICKUP || t == MV_GOOD) continue;   // added below
                kept_idx[kept++] = order[k];
            }
            for (int i = 0; i < n; i++) {
                uint8_t t = buf[i].type;
                if (t == MV_PICKUP || t == MV_GOOD) kept_idx[kept++] = i;
            }
            uint32_t rng0 = game_rng_get();
            int best_actor_pos = 1 << 20;
            int best_my_pos = -1;
            for (int k = 0; k < kept; k++) {
                SimState trial = *s;
                game_rng_set(rng0);   // CRN across replies
                sim_apply_sol(&trial, actor, &buf[kept_idx[k]]);
                // Full playout, NO early exit: both finishes are needed.
                (void)cd_sim_playout_pol(&trial, my_idx, max_turns, 0,
                                         leaf_cards, leaf_budget, pol);
                if (sim_done(&trial) < 0) continue;   // unterminated: skip
                int ap = sim_pos_of(&trial, actor);
                if (ap < best_actor_pos) {
                    best_actor_pos = ap;
                    best_my_pos = sim_pos_of(&trial, my_idx);
                }
            }
            if (best_my_pos > 0) return best_my_pos;
            break;   // tournament failed entirely: policy playout below
        }
        // My move, or a non-defender opponent decision: one policy step.
        SimMove m;
        int got = (pol && pol[actor] == CD_POL_LOOSE)
                ? sim_loose_move(s, actor, &m)
                : (pol && pol[actor] == CD_POL_MCDEF)
                ? sim_mcdef_move(s, actor, &m)
                : sim_handwritten_move(s, actor, &m);
        if (!got) break;
        sim_apply(s, actor, &m);
    }
    return cd_sim_playout_pol(s, my_idx, max_turns, 1,
                              leaf_cards, leaf_budget, pol);
}

// As cd_sim_playout, but resolves small 2-player deck-empty endgames exactly
// with the bitboard solver instead of finishing them with policy play (one
// attempt per playout; a failed solve falls back to the policy for good).
// The TT is NOT cleared between leaf calls: entries are keyed on a full
// 64-bit position fingerprint, so worlds can share it, and values are
// depth-rebased so cross-call reuse reads back correctly. Used by semtex —
// against near-perfect endgame players (cordite itself) modeling the endgame
// as exact beats modeling it as handwritten play.
int cd_sim_playout_leaf(SimState *s, int my_idx, int max_turns, int early_exit,
                        int leaf_cards, long leaf_budget) {
    int turns = 0;
    int leaf_tried = 0;
    while (sim_done(s) < 0 && turns++ < max_turns) {
        if (early_exit && s->status_p[my_idx] != PLAYER_STATUS_IN) {
            for (int i = 0; i < s->num_eliminated; i++)
                if (s->elim_order[i] == my_idx) return i + 1;
            break;
        }
        if (!leaf_tried && s->deck_n == 0 && !s->has_flipped) {
            int a = -1, b = -1;
            for (int i = 0; i < s->num_players; i++) {
                if (s->status_p[i] != PLAYER_STATUS_IN) continue;
                if (a < 0) a = i; else if (b < 0) b = i; else { b = -2; break; }
            }
            if (a >= 0 && b >= 0) {
                int total = __builtin_popcountll(s->hand[a])
                          + __builtin_popcountll(s->hand[b]);
                for (int i = 0; i < s->num_battles; i++)
                    total += 1 + ((s->covered_mask >> i) & 1);
                if (total <= leaf_cards) {
                    leaf_tried = 1;
                    int aborted = 0;
                    long budget = leaf_budget;
                    // Sign-only null window: who is the durak?
                    int v = cd_sim_solve_d(s, a, -1, 1, &budget, 0, &aborted);
                    if (!aborted && v != 0) {
                        int loser = (v < 0) ? a : b;
                        int np = s->num_players;
                        if (my_idx == loser) return np;
                        if (my_idx == a || my_idx == b) return np - 1;
                        for (int i = 0; i < s->num_eliminated; i++)
                            if (s->elim_order[i] == my_idx) return i + 1;
                        return np - 1;   // unreachable; defensive
                    }
                }
            }
        }
        int acted = 0;
        for (int pi = 0; pi < s->num_players; pi++) {
            if (!sim_should_act(s, pi)) continue;
            SimMove m;
            if (!sim_handwritten_move(s, pi, &m)) continue;
            sim_apply(s, pi, &m);
            acted = 1;
            break;
        }
        if (!acted) break;
    }
    if (sim_done(s) < 0) return 0;
    for (int i = 0; i < s->num_eliminated; i++)
        if (s->elim_order[i] == my_idx) return i + 1;
    return s->num_players;
}

int cd_sim_playout(SimState *s, int my_idx, int max_turns, int early_exit) {
    int turns = 0;
    while (sim_done(s) < 0 && turns++ < max_turns) {
        if (early_exit && !(s->in_mask >> my_idx & 1u)) {
            for (int i = 0; i < s->num_eliminated; i++)
                if (s->elim_order[i] == my_idx) return i + 1;
            break;
        }
        // Eligible-actor mask == sim_should_act over every seat, evaluated
        // once per ply instead of per seat (this scan was the second-hottest
        // region in the wasm profile). Iterating set bits ascending keeps
        // the exact first-eligible-seat order of the old loop, including
        // trying the next seat when the policy returns no move.
        uint32_t elig = 0;
        if (s->status == GAME_STATUS_PLAYING) {
            if (s->num_battles == 0) {
                elig = s->in_mask & (1u << s->first_attacker);
            } else {
                elig = s->in_mask & ~s->good_mask & ~(1u << s->defender);
                if (!sim_all_covered(s)) elig |= s->in_mask & (1u << s->defender);
            }
        }
        int acted = 0;
        for (uint32_t m = elig; m; m &= m - 1) {
            int pi = __builtin_ctz(m);
            SimMove mv;
            if (!sim_handwritten_move(s, pi, &mv)) continue;
            sim_apply(s, pi, &mv);
            acted = 1;
            break;
        }
        if (!acted) break;
    }
    if (sim_done(s) < 0) return 0;
    for (int i = 0; i < s->num_eliminated; i++)
        if (s->elim_order[i] == my_idx) return i + 1;
    return s->num_players;
}

// ---------------------------------------------------------------------------
// Shared struct-solver scratch (see cordite_sim.h). BSS, not malloc: the wasm
// bump allocator never frees, so per-family mallocs accumulated; a single
// static copy is the whole footprint, paid once at instantiation.
// ---------------------------------------------------------------------------
#define SOLVE_CHILD_STRIDE (((size_t)offsetof(Game, logs) + 15u) & ~(size_t)15u)
static _Thread_local unsigned char solve_child_scratch[SOLVE_SCRATCH_DEPTH * SOLVE_CHILD_STRIDE]
    __attribute__((aligned(16)));

// Solver move slots and the struct-rollout move list share one allocation:
// they are never live at the same time (a leaf solve runs at the TOP of a
// rollout ply, before that ply's enumeration; the rollout list is consumed
// before the next ply — see cd_simulate/sx_simulate/og_simulate; root solves
// run before any rollout starts). The union costs
// max(48 x SolveMoves, LegalMoves) instead of both — the rollout list alone
// is sizeof(LegalMoves) (~332KB at the bots build's MAX_LEGAL_MOVES=4096).
static _Thread_local union {
    SolveMoves mv[SOLVE_SCRATCH_DEPTH];
    LegalMoves rollout;
} solve_ws;

// Solver root slot: prefix-sized like the child slots (roots never take
// appends — only children are applied to), but with num_logs pinned at 0,
// not MAX_LOGS: lite clones READ `src->num_logs` log entries, and 0 keeps
// any such read inside the prefix.
static _Thread_local unsigned char solve_root_scratch[SOLVE_CHILD_STRIDE]
    __attribute__((aligned(16)));

Game *solve_scratch_child(int depth) {
    return (Game *)(solve_child_scratch + (size_t)depth * SOLVE_CHILD_STRIDE);
}

void solve_clone_prefix(Game *dst, const Game *src) {
    memcpy(dst, src, offsetof(Game, logs));
    // Sinkhole: log_alloc sees a full array and drops appends into its own
    // static scratch, so nothing ever writes past the prefix-sized slot.
    dst->num_logs = MAX_LOGS;
}

SolveMoves *solve_scratch_mv(void) { return solve_ws.mv; }

LegalMoves *rollout_moves_scratch(void) { return &solve_ws.rollout; }

Game *solve_scratch_root(void) { return (Game *)solve_root_scratch; }

void solve_clone_root(Game *dst, const Game *src) {
    memcpy(dst, src, offsetof(Game, logs));
    dst->num_logs = 0;   // solver never reads history; keeps lite reads in-slot
}

// Sampled-world scratch (see cordite_sim.h). Was one world/trial pair PER
// family — 3 identical pairs of write-only-between-decisions state. The
// difftest slow-rollout game is world-shaped too (a rollout clone of the
// trial): all three become short-log slots under WORLD_LOG_CAP.
#if WORLD_LOG_CAP > 0
#define WORLD_SLOT_BYTES (offsetof(Game, logs) + (size_t)WORLD_LOG_CAP * sizeof(GameLog))
#else
#define WORLD_SLOT_BYTES sizeof(Game)
#endif
typedef struct { _Alignas(16) unsigned char bytes[WORLD_SLOT_BYTES]; } WorldSlot;
static _Thread_local WorldSlot world_slot, trial_slot, diff_slot;
static _Thread_local SimState world_sim_s, trial_sim_s;
static _Thread_local bool forced_loss_flags[MAX_LEGAL_MOVES];

Game     *world_scratch_game(void)  { return (Game *)world_slot.bytes; }
Game     *trial_scratch_game(void)  { return (Game *)trial_slot.bytes; }
Game     *rollout_scratch_diff(void){ return (Game *)diff_slot.bytes; }
SimState *world_scratch_sim(void)   { return &world_sim_s; }
SimState *trial_scratch_sim(void)   { return &trial_sim_s; }
bool     *forced_loss_scratch(void) { return forced_loss_flags; }
