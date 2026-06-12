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
