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
#include <stdint.h>
#include <stdlib.h>   // calloc/free for the solver transposition table

// ---------- card-id helpers --------------------------------------------

#define ID(suit, value) ((suit) * 13 + ((value) - 1))
static inline int id_suit(int id)  { return id / 13; }
static inline int id_value(int id) { return id % 13 + 1; }
static inline int card_id(Card c)  { return c.suit * 13 + (c.value - 1); }

// Precomputed masks (id space 0..51).
static uint64_t VALUE_MASK[14];   // VALUE_MASK[v] = all ids with value v (1..13)
static uint64_t SUIT_MASK[4];
static int      g_masks_ready = 0;

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
    }
    for (int i = 0; i < g->num_eliminated; i++) s->elim_order[i] = g->elimination_order[i];

    for (int i = 0; i < g->num_battles; i++) {
        s->atk[i] = (uint8_t)card_id(g->table_battles[i].attack);
        if (g->table_battles[i].has_defense) {
            s->def[i] = (uint8_t)card_id(g->table_battles[i].defense);
            s->covered_mask |= (1ull << i);
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
    int n = 0;
    for (int i = 0; i < s->num_players; i++)
        if (s->status_p[i] == PLAYER_STATUS_IN) n++;
    return n;
}

static inline int sim_next_player(const SimState *s, int cur) {
    int n = s->num_players;
    int next = (cur + 1) % n;
    while (s->status_p[next] == PLAYER_STATUS_OUT) next = (next + 1) % n;
    return next;
}

// Returns the single IN player if exactly one remains (game over), else -1.
static int sim_done(const SimState *s) {
    int in_count = 0, out_count = 0, last_in = -1;
    for (int i = 0; i < s->num_players; i++) {
        if (s->status_p[i] == PLAYER_STATUS_IN) { in_count++; last_in = i; }
        else if (s->status_p[i] == PLAYER_STATUS_OUT) out_count++;
    }
    if (in_count == 1 && out_count == s->num_players - 1) return last_in;
    return -1;
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
static inline uint64_t sim_table_value_mask(const SimState *s) {
    uint64_t m = 0;
    for (int i = 0; i < s->num_battles; i++) {
        m |= VALUE_MASK[id_value(s->atk[i])];
        if (s->covered_mask & (1ull << i)) m |= VALUE_MASK[id_value(s->def[i])];
    }
    return m;
}

// draw one card id, mirroring draw_card (deck array splice + flipped fallback).
static int sim_draw(SimState *s, int *out) {
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
    s->elim_order[s->num_eliminated++] = (int8_t)p;
}

// refill_player_hands port.
static void sim_refill(SimState *s) {
    if (sim_no_cards_left(s)) {
        for (int i = 0; i < s->num_players; i++) {
            if (s->status_p[i] == PLAYER_STATUS_IN && sim_hand_count(s, i) == 0)
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
        if (sim_hand_count(s, p_idx) == 0 && s->status_p[p_idx] == PLAYER_STATUS_IN)
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
    }
    s->good_mask = 0;
    if (sim_hand_count(s, p_idx) == 0) {
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
    }

    if (sim_hand_count(s, p_idx) == 0) {
        s->discard_pile_length += s->num_battles * 2;
        s->num_battles = 0;
        s->covered_mask = 0;
        sim_refill(s);
        s->first_attacker = s->defender;
        s->good_mask = 0;
        if (sim_hand_count(s, s->first_attacker) == 0) {
            int fa = s->first_attacker;
            int was_in = (s->status_p[fa] == PLAYER_STATUS_IN);
            if (was_in) sim_eliminate(s, fa);
            else s->status_p[fa] = PLAYER_STATUS_OUT;
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
    sim_refill(s);
    s->first_attacker = sim_next_player(s, s->defender);
    s->defender = sim_next_player(s, s->first_attacker);
    s->good_mask = 0;
}

static void sim_round_transition(SimState *s) {
    s->discard_pile_length += s->num_battles * 2;
    s->num_battles = 0;
    s->covered_mask = 0;
    sim_refill(s);
    s->first_attacker = s->defender;
    s->defender = sim_next_player(s, s->first_attacker);
    s->good_mask = 0;
}

static void sim_apply_good(SimState *s, int p_idx) {
    s->good_mask |= (1u << p_idx);
    int n_attackers = 0;
    int all_good = 1;
    for (int i = 0; i < s->num_players; i++) {
        if (i != s->defender && s->status_p[i] == PLAYER_STATUS_IN) {
            n_attackers++;
            if (!(s->good_mask & (1ull << i))) all_good = 0;
        }
    }
    if (n_attackers == 0) all_good = 0;
    if (all_good && sim_all_covered(s)) sim_round_transition(s);
}

// ---------- should_act (bitboard) --------------------------------------

static int sim_should_act(const SimState *s, int p) {
    if (s->status != GAME_STATUS_PLAYING) return 0;
    if (s->status_p[p] != PLAYER_STATUS_IN) return 0;
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
    int best_v = -1, best_eff = 0;
    for (int v = 1; v <= 13; v++) {
        uint64_t g = h & VALUE_MASK[v];
        int sz = popcnt64(g);
        if (sz == 0) continue;
        int eff = sz < defcap ? sz : defcap;
        if (eff <= 0) continue;
        if (eff > best_eff || (eff == best_eff && (best_v < 0 || v < best_v))) {
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
// capped by defender capacity (defender_cards >= uncovered + k).
static int sim_regular_attack_group(const SimState *s, int p, int power,
                                    int non_trump_only, uint8_t *out) {
    uint64_t tv = sim_table_value_mask(s);
    uint64_t h = s->hand[p] & tv;
    if (non_trump_only) h &= ~SUIT_MASK[power];
    if (!h) return 0;
    int uncovered = sim_count_uncovered(s);
    int defcap = sim_hand_count(s, s->defender) - uncovered;
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
        int best = -1, best_score = INT32_MAX;
        uint64_t a = avail;
        while (a) {
            int id = ctz64(a); a &= a - 1;
            if (id_can_cover(atk, id, power)) {
                int sc = id_score(id, power);
                if (sc < best_score) { best_score = sc; best = id; }
            }
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
    // (positive scores). So k=1, lowest-score matching card.
    int best = -1, best_score = INT32_MAX;
    uint64_t mm = matching;
    while (mm) {
        int id = ctz64(mm); mm &= mm - 1;
        int sc = id_score(id, power);
        if (sc < best_score) { best_score = sc; best = id; }
    }
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

    if (can_attack) {
        uint8_t buf[MAX_HAND_SIZE];
        int n_nt, n_full;
        if (first_attack) {
            n_nt = sim_first_attack_group(s, p, power, 1, buf);
        } else {
            n_nt = sim_regular_attack_group(s, p, power, 1, buf);
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
        else              n_tr = sim_regular_attack_group(s, p, power, 0, tbuf);
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
            else              n = sim_regular_attack_group(s, p, power, 1, buf);
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
        else              n = sim_regular_attack_group(s, p, power, 0, buf);
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

#define CD_TT_BITS  16
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
    CdTTEntry *tt;
} SimSolver;

static _Thread_local CdTTEntry *cd_tt = NULL;

static CdTTEntry *cd_tt_get(void) {
    if (!cd_tt) cd_tt = (CdTTEntry *)calloc(CD_TT_SIZE, sizeof(CdTTEntry));
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

// Move-ordering key: lower = tried first. For the maximizer we want quick
// wins; for both sides emptying the hand / ending the round tends to resolve
// fast. We sort attacks/covers by card count desc then score, pickup/good
// last. Ordering does not affect the value (full search), only speed.
static int sim_solve_rec(SimSolver *S, SimState *s, int alpha, int beta, int depth) {
    int loser = sim_done(s);
    if (loser >= 0) return (loser == S->me) ? -(1000 - depth) : (1000 - depth);
    int incount = sim_in_count(s);
    if (incount == 0) return 0;
    if (depth >= CD_SIM_SOLVE_MAX_DEPTH) { S->aborted = 1; return 0; }
    if (--S->budget <= 0) { S->aborted = 1; return 0; }

    // actor: defender-priority, then first IN actor (mirrors cd_solve).
    int actor = -1;
    if (sim_should_act(s, s->defender)) actor = s->defender;
    else {
        for (int i = 0; i < s->num_players; i++)
            if (sim_should_act(s, i)) { actor = i; break; }
    }
    if (actor < 0) return 0;

    // Two players for the fingerprint (the only IN pair in an endgame).
    int a = -1, b = -1;
    for (int i = 0; i < s->num_players; i++)
        if (s->status_p[i] == PLAYER_STATUS_IN) { if (a < 0) a = i; else b = i; }
    uint64_t key = 0;
    CdTTEntry *e = NULL;
    if (b >= 0) {
        key = sim_fingerprint(s, a, b);
        e = &S->tt[key & CD_TT_MASK];
        if (e->valid && e->key == key) {
            // stored value is depth-relative to e->depth; re-base to this depth.
            int v = e->value;
            if (v > 0) v = v - (1000 - e->depth) + (1000 - depth);
            else if (v < 0) v = v + (1000 - e->depth) - (1000 - depth);
            return v;
        }
    }

    SolMove moves[CD_SIM_SOLVE_MAX_MOVES];
    int nm = sim_gen_moves(s, actor, moves, CD_SIM_SOLVE_MAX_MOVES);
    if (nm == 0) return 0;
    // Mirror the struct solver's `mv->n > CD_SOLVE_MAX_MOVES` bail: abort on
    // nodes with more than the cap legal moves, so we resolve/abort the exact
    // same position set as the struct solver (identical play). The buffer has
    // slack above the cap so a true count above it is still detected.
    if (nm > CD_SOLVE_MOVES_CAP) { S->aborted = 1; return 0; }

    int alpha0 = alpha, beta0 = beta;   // original window for exactness test
    int maximizing = (actor == S->me);
    int best = maximizing ? -2000 : 2000;
    int applied = 0;
    SimState child;
    for (int i = 0; i < nm; i++) {
        child = *s;
        sim_apply_sol(&child, actor, &moves[i]);
        applied = 1;
        int v = sim_solve_rec(S, &child, alpha, beta, depth + 1);
        if (S->aborted) return 0;
        if (maximizing) {
            if (v > best) best = v;
            if (best > alpha) alpha = best;
        } else {
            if (v < best) best = v;
            if (best < beta) beta = best;
        }
        if (alpha >= beta) break;
    }
    if (!applied || best == -2000 || best == 2000) return 0;

    // Memoize EXACT values only. A fail-soft alpha-beta result `best` is the
    // true game value only when it lands strictly INSIDE the original window
    // (alpha0 < best < beta0). On a fail-low (best <= alpha0) it is only an
    // upper bound; on a fail-high (best >= beta0) only a lower bound — storing
    // either as exact would corrupt a later lookup under a wider window. So we
    // store solely the exact case; bound nodes are simply not memoized.
    if (e && key && best > alpha0 && best < beta0) {
        e->key = key;
        e->value = (int16_t)best;
        e->depth = (uint8_t)depth;
        e->valid = 1;
    }
    return best;
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
    S.budget = *budget;
    S.aborted = 0;
    S.me = me;
    S.tt = cd_tt_get();
    if (!S.tt) { if (aborted) *aborted = 1; return 0; }
    int v = sim_solve_rec(&S, s, alpha, beta, depth0);
    *budget = S.budget;
    if (aborted) *aborted = S.aborted;
    return v;
}

void cd_sim_solve_reset(void) {
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
        if (early_exit && s->status_p[my_idx] != PLAYER_STATUS_IN) {
            for (int i = 0; i < s->num_eliminated; i++)
                if (s->elim_order[i] == my_idx) return i + 1;
            break;
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
