// Legal-move enumeration. Direct port of bot_strategy.ts. We preserve the
// emitted ordering (combinations enumerated in input order) so that anything
// downstream that picks "first matching" stays deterministic vs TS.

#include "legal.h"
#include <string.h>

// Output cap (see legal_set_move_cap in legal.h): defaults to the full
// MAX_LEGAL_MOVES; the endgame solvers lower it around their movegen calls
// so they can enumerate into compact scratch buffers.
static _Thread_local int g_move_cap = MAX_LEGAL_MOVES;

void legal_set_move_cap(int cap) {
    g_move_cap = (cap > 0 && cap <= MAX_LEGAL_MOVES) ? cap : MAX_LEGAL_MOVES;
}

// LEGAL_STATS (measurement-only, compiled out of every production build): record
// the widest FULL-cap enumeration — i.e. the exported move MENU (the g_moves
// buffer sized MAX_LEGAL_MOVES), NOT the solver's compact scratch, which lowers
// g_move_cap. Sizes the M2 decision (docs/BOTS_WASM_MEMORY_PLAN.md). Prints each
// new high-water to stderr so a sweep can `grep LEGALMAX | sort -n | tail -1`.
#ifdef LEGAL_STATS
#include <stdio.h>
static int legal_stat_max_n = 0;
static void legal_stat_note(const LegalMoves *out, const Game *g, int bot_idx) {
    // Emit each new high-water with the state that drove it (player count,
    // uncovered battles, actor hand size) so a sweep can identify WHICH states
    // are wide: `... 2>&1 | grep LEGALMAX | sort -n | tail`. The wide ones are
    // large-hand 8-player defenders — cover-combination blow-up (M2 finding).
    if (g_move_cap >= MAX_LEGAL_MOVES && out->n > legal_stat_max_n) {
        legal_stat_max_n = out->n;
        fprintf(stderr, "LEGALMAX %d np=%d nb=%d hand=%d\n",
                out->n, g->num_players, g->num_battles, g->players[bot_idx].hand_count);
    }
}
#else
#define legal_stat_note(out, g, bot_idx) ((void)0)
#endif

// Append a move; silently drops past the cap (MAX_LEGAL_MOVES by default).
// The slot is NOT zeroed: every consumer reads cards[]/attack_cards[] bounded
// by n_cards (set by the caller), and clearing the full struct was a
// measurable cost at tens of thousands of moves per enumeration.
static LegalMove *push_move(LegalMoves *out) {
    if (out->n >= g_move_cap) return NULL;
    LegalMove *m = &out->moves[out->n++];
    m->n_cards = 0;
    return m;
}

// ---------- combinations -----------------------------------------------

static void emit_attack(LegalMoves *out, const Card *combo, int k,
                        int defender_cards, int uncovered) {
    if (defender_cards < uncovered + k) return;
    LegalMove *m = push_move(out);
    if (!m) return;
    m->type = MOVE_ATTACK;
    m->n_cards = (int8_t)k;
    for (int i = 0; i < k; i++) m->cards[i] = combo[i];
}

// Recursive combinations of `arr[start..]` choose `k`, calling cb with the
// chosen items.
static void combinations_attack(const Card *arr, int n, int start, int k,
                                Card *buf, int depth,
                                LegalMoves *out, int defender_cards, int uncovered) {
    // Prune once the cap is hit — otherwise a hand carrying many same-value
    // cards (only reachable via a malformed/corrupt state, since a real hand
    // holds no duplicates) drives ~2^n recursion while push_move silently
    // drops past the cap: an unbounded hang. Mirrors emit_cover_combo's
    // guard. One compare per node; real hands recurse a handful of levels.
    if (out->n >= g_move_cap) return;
    if (depth == k) {
        emit_attack(out, buf, k, defender_cards, uncovered);
        return;
    }
    for (int i = start; i <= n - (k - depth); i++) {
        buf[depth] = arr[i];
        combinations_attack(arr, n, i + 1, k, buf, depth + 1, out,
                            defender_cards, uncovered);
    }
}

static void emit_pass(LegalMoves *out, const Card *combo, int k,
                      int next_player_cards, int n_battles) {
    if (next_player_cards < k + n_battles) return;
    LegalMove *m = push_move(out);
    if (!m) return;
    m->type = MOVE_PASS;
    m->n_cards = (int8_t)k;
    for (int i = 0; i < k; i++) m->cards[i] = combo[i];
}

static void combinations_pass(const Card *arr, int n, int start, int k,
                              Card *buf, int depth,
                              LegalMoves *out, int next_player_cards, int n_battles) {
    if (out->n >= g_move_cap) return;   // same anti-blowup guard as attack
    if (depth == k) {
        emit_pass(out, buf, k, next_player_cards, n_battles);
        return;
    }
    for (int i = start; i <= n - (k - depth); i++) {
        buf[depth] = arr[i];
        combinations_pass(arr, n, i + 1, k, buf, depth + 1, out,
                          next_player_cards, n_battles);
    }
}

// ---------- attack moves ----------------------------------------------

static void calc_first_attack_moves(const Game *g, const Player *p, LegalMoves *out) {
    // Group cards by value, preserving original order within each group
    // (which matches TS Map iteration: insertion order).
    int defender_cards = g->players[g->defender].hand_count;
    // For each unique value, in first-seen order in hand.
    bool seen[16] = { false };
    Card buf[MAX_MOVE_CARDS];
    for (int i = 0; i < p->hand_count; i++) {
        int v = p->hand[i].value;
        // Guard the seen[] index: a real card value is 1..13, but a
        // malformed state can carry anything an int8 holds — an unguarded
        // seen[v] is an out-of-bounds stack access. Skip impossible values.
        if (v < 1 || v > ACE_VALUE) continue;
        if (seen[v]) continue;
        seen[v] = true;
        Card group[MAX_MOVE_CARDS];
        int gn = 0;
        for (int j = 0; j < p->hand_count && gn < MAX_MOVE_CARDS; j++) {
            if (p->hand[j].value == v) group[gn++] = p->hand[j];
        }
        // Bound k by defender capacity: emit_attack drops any k where
        // defender_cards < k (uncovered is 0 on a first attack), so combos
        // above that never enter the move list — but WITHOUT this bound the
        // recursion still explores all C(gn,k) subsets only to reject them at
        // the leaf, a ~2^gn hang on a many-same-value (corrupt) hand. Same
        // move set, no doomed recursion.
        int k_max = gn < defender_cards ? gn : defender_cards;
        for (int k = 1; k <= k_max; k++) {
            combinations_attack(group, gn, 0, k, buf, 0, out, defender_cards, 0);
        }
    }
}

static void calc_regular_attack_moves(const Game *g, const Player *p, LegalMoves *out) {
    // Bound every table_values[] index by the card value: a malformed state
    // can carry out-of-range values that would otherwise index this
    // 16-entry stack array out of bounds. Real values are 1..ACE_VALUE.
    bool table_values[16] = { false };
    for (int i = 0; i < g->num_battles; i++) {
        int av = g->table_battles[i].attack.value;
        if (av >= 1 && av <= ACE_VALUE) table_values[av] = true;
        if (!card_is_none(g->table_battles[i].defense)) {
            int dv = g->table_battles[i].defense.value;
            if (dv >= 1 && dv <= ACE_VALUE) table_values[dv] = true;
        }
    }
    Card valid[MAX_HAND_SIZE];
    int vn = 0;
    for (int i = 0; i < p->hand_count; i++) {
        int v = p->hand[i].value;
        if (v >= 1 && v <= ACE_VALUE && table_values[v]) valid[vn++] = p->hand[i];
    }
    if (vn == 0) return;
    int defender_cards = g->players[g->defender].hand_count;
    int uncovered = 0;
    for (int i = 0; i < g->num_battles; i++) if (!!card_is_none(g->table_battles[i].defense)) uncovered++;
    Card buf[MAX_MOVE_CARDS];
    // Combos wider than a LegalMove can hold are dropped (documented cap;
    // also prevents buf overflow when a post-pickup hand is huge).
    int k_max = vn < MAX_MOVE_CARDS ? vn : MAX_MOVE_CARDS;
    // Also bound k by remaining defender capacity: emit_attack drops any k
    // with defender_cards < uncovered + k, so wider combos never reach the
    // move list — bounding here avoids the doomed ~2^vn recursion on a
    // many-same-value (corrupt) hand. Same emitted set.
    int cap = defender_cards - uncovered;
    if (cap < k_max) k_max = cap;
    for (int k = 1; k <= k_max; k++) {
        combinations_attack(valid, vn, 0, k, buf, 0, out, defender_cards, uncovered);
    }
}

// ---------- cover moves -----------------------------------------------

// Given attackIndices and a card-cover assignment under construction,
// recursively enumerate. The outer loop picks how many attacks to cover
// (1..uncovered.length) and which set of attack indices.

typedef struct {
    int attack_idx;        // index into uncovered_battles
    int covers_n;
    Card covers[MAX_HAND_SIZE]; // cards in defender hand that can cover this attack
    int8_t cover_hand_idx[MAX_HAND_SIZE]; // hand index of each cover (dedup key)
    Card attack_card;      // the actual card on the table
} CoverOption;

static void emit_cover_combo(LegalMoves *out,
                             const CoverOption *const *opts, int n_opts,
                             int *chosen_idx, int depth, bool *used) {
    if (out->n >= g_move_cap) return;  // early-exit; combinatorial blowup otherwise
    if (depth == n_opts) {
        LegalMove *m = push_move(out);
        if (!m) return;
        m->type = MOVE_COVER;
        m->n_cards = (int8_t)n_opts;
        for (int i = 0; i < n_opts; i++) {
            m->cards[i] = opts[i]->covers[chosen_idx[i]];
            m->attack_cards[i] = opts[i]->attack_card;
        }
        return;
    }
    for (int i = 0; i < opts[depth]->covers_n; i++) {
        // Avoid using the same card twice across attacks: the hand index of
        // each cover was recorded at option-build time, so the dedup check is
        // O(1) instead of a hand scan per candidate.
        int hi = opts[depth]->cover_hand_idx[i];
        if (used[hi]) continue;
        used[hi] = true;
        chosen_idx[depth] = i;
        emit_cover_combo(out, opts, n_opts, chosen_idx, depth + 1, used);
        used[hi] = false;
    }
}

static void choose_attack_subset(const Game *g, const Player *defender,
                                 const int *uncovered_battles, int n_uncovered,
                                 const CoverOption *all_opts,
                                 int start, int k_left,
                                 int *picked, int picked_n,
                                 LegalMoves *out) {
    if (k_left == 0) {
        // Reference the picked options by pointer — copying each CoverOption
        // (a couple hundred bytes) per enumerated subset was pure overhead.
        const CoverOption *opts[MAX_BATTLES];
        for (int i = 0; i < picked_n; i++) opts[i] = &all_opts[picked[i]];
        // Skip if any picked attack has no covers.
        for (int i = 0; i < picked_n; i++) if (opts[i]->covers_n == 0) return;
        int chosen[MAX_BATTLES];
        bool used[MAX_HAND_SIZE] = { false };
        emit_cover_combo(out, opts, picked_n, chosen, 0, used);
        return;
    }
    for (int i = start; i <= n_uncovered - k_left; i++) {
        picked[picked_n] = i;
        choose_attack_subset(g, defender, uncovered_battles, n_uncovered,
                             all_opts, i + 1, k_left - 1, picked, picked_n + 1, out);
    }
}

static void calc_cover_moves(const Game *g, const Player *defender, LegalMoves *out) {
    int uncovered_battles[MAX_BATTLES];
    int n_uncovered = 0;
    for (int i = 0; i < g->num_battles; i++) {
        if (!!card_is_none(g->table_battles[i].defense)) uncovered_battles[n_uncovered++] = i;
    }
    if (n_uncovered == 0) return;

    CoverOption opts[MAX_BATTLES];
    for (int i = 0; i < n_uncovered; i++) {
        const Battle *b = &g->table_battles[uncovered_battles[i]];
        opts[i].attack_idx = uncovered_battles[i];
        opts[i].attack_card = b->attack;
        opts[i].covers_n = 0;
        for (int j = 0; j < defender->hand_count; j++) {
            if (can_cover(b->attack, defender->hand[j], g->power_suit)) {
                opts[i].cover_hand_idx[opts[i].covers_n] = (int8_t)j;
                opts[i].covers[opts[i].covers_n++] = defender->hand[j];
            }
        }
    }

    int picked[MAX_BATTLES];
    int k_max = n_uncovered < MAX_MOVE_CARDS ? n_uncovered : MAX_MOVE_CARDS;
    for (int k = 1; k <= k_max; k++) {
        choose_attack_subset(g, defender, uncovered_battles, n_uncovered,
                             opts, 0, k, picked, 0, out);
    }
}

// ---------- pass moves ------------------------------------------------

static void calc_pass_moves(const Game *g, const Player *defender, LegalMoves *out) {
    bool any_covered = false;
    for (int i = 0; i < g->num_battles; i++) if (!card_is_none(g->table_battles[i].defense)) any_covered = true;
    if (any_covered) return;
    if (g->num_battles == 0) return;

    int v0 = g->table_battles[0].attack.value;
    for (int i = 1; i < g->num_battles; i++) if (g->table_battles[i].attack.value != v0) return;

    Card matching[MAX_HAND_SIZE];
    int mn = 0;
    for (int i = 0; i < defender->hand_count; i++) {
        if (defender->hand[i].value == v0) matching[mn++] = defender->hand[i];
    }
    if (mn == 0) return;

    int next = get_next_player_index(g, g->defender);
    int next_cards = g->players[next].hand_count;

    Card buf[MAX_MOVE_CARDS];
    // Bound k by (a) the buf capacity — combinations_pass writes buf[0..k-1],
    // so k > MAX_MOVE_CARDS overflows it on a corrupt oversized hand — and
    // (b) the next player's capacity, since emit_pass drops any k with
    // next_cards < k + n_battles (those combos never reach the list). Without
    // (b) the recursion explores ~2^mn doomed subsets. Same emitted set.
    int k_max = mn < MAX_MOVE_CARDS ? mn : MAX_MOVE_CARDS;
    int cap = next_cards - g->num_battles;
    if (cap < k_max) k_max = cap;
    for (int k = 1; k <= k_max; k++) {
        combinations_pass(matching, mn, 0, k, buf, 0, out, next_cards, g->num_battles);
    }
}

// Greedy lowest-cost full cover for the defender. Picks, for each uncovered
// attack, the lowest-score covering card not already used (preferring non-
// trump). Adds at most one MOVE_COVER move to `out`. Skipped entirely if the
// defender can't fully cover.
static void calc_cover_moves_greedy(const Game *g, const Player *defender, LegalMoves *out) {
    Card uncovered[MAX_BATTLES];
    int n_uncovered = 0;
    for (int i = 0; i < g->num_battles; i++) {
        if (!!card_is_none(g->table_battles[i].defense)) uncovered[n_uncovered++] = g->table_battles[i].attack;
    }
    if (n_uncovered == 0) return;
    if (n_uncovered > MAX_MOVE_CARDS) return; // can't fit in one LegalMove

    bool used[MAX_HAND_SIZE] = { false };
    Card covers[MAX_BATTLES];

    for (int i = 0; i < n_uncovered; i++) {
        int best = -1;
        int best_score = INT32_MAX;
        for (int j = 0; j < defender->hand_count; j++) {
            if (used[j]) continue;
            if (can_cover(uncovered[i], defender->hand[j], g->power_suit)) {
                int s = defender->hand[j].value + (defender->hand[j].suit == g->power_suit ? 1000 : 0);
                if (s < best_score) { best_score = s; best = j; }
            }
        }
        if (best < 0) return;
        used[best] = true;
        covers[i] = defender->hand[best];
    }

    LegalMove *m = push_move(out);
    if (!m) return;
    m->type = MOVE_COVER;
    m->n_cards = (int8_t)n_uncovered;
    for (int i = 0; i < n_uncovered; i++) {
        m->cards[i] = covers[i];
        m->attack_cards[i] = uncovered[i];
    }
}

// ---------- main entry point ------------------------------------------

void calculate_legal_moves(const Game *g, int bot_idx, LegalMoves *out) {
    out->n = 0;
    if (g->status != GAME_STATUS_PLAYING) return;
    const Player *p = &g->players[bot_idx];
    bool is_def = (bot_idx == g->defender);
    bool is_first_attacker = (bot_idx == g->first_attacker);
    bool first_attack = (g->num_battles == 0);
    bool all_covered = (g->num_battles > 0);
    for (int i = 0; i < g->num_battles; i++) if (!!card_is_none(g->table_battles[i].defense)) all_covered = false;

    if (first_attack && is_first_attacker) {
        calc_first_attack_moves(g, p, out);
    } else if (is_def && g->num_battles > 0) {
        calc_cover_moves(g, p, out);
        if (!all_covered) {
            LegalMove *m = push_move(out);
            if (m) m->type = MOVE_PICKUP;
        }
        calc_pass_moves(g, p, out);
    } else if (!is_def && g->num_battles > 0) {
        bool said_good = (g->good_players_mask & (1u << bot_idx)) != 0;
        if (!said_good) {
            calc_regular_attack_moves(g, p, out);
            LegalMove *m = push_move(out);
            if (m) m->type = MOVE_GOOD;
        }
    }
    legal_stat_note(out, g, bot_idx);
}

// Lite variant — identical to calculate_legal_moves except cover enumeration
// is replaced by a single greedy lowest-cost full-cover. Handwritten always
// picks the lowest-product full cover, so the greedy result is equivalent in
// 99%+ of cases (greedy can rarely deviate from product-optimal, but the
// difference is small and dominated by MC sampling variance).
void calculate_legal_moves_lite(const Game *g, int bot_idx, LegalMoves *out) {
    out->n = 0;
    if (g->status != GAME_STATUS_PLAYING) return;
    const Player *p = &g->players[bot_idx];
    bool is_def = (bot_idx == g->defender);
    bool is_first_attacker = (bot_idx == g->first_attacker);
    bool first_attack = (g->num_battles == 0);
    bool all_covered = (g->num_battles > 0);
    for (int i = 0; i < g->num_battles; i++) if (!!card_is_none(g->table_battles[i].defense)) all_covered = false;

    if (first_attack && is_first_attacker) {
        calc_first_attack_moves(g, p, out);
    } else if (is_def && g->num_battles > 0) {
        calc_cover_moves_greedy(g, p, out);    // <-- greedy single cover
        if (!all_covered) {
            LegalMove *m = push_move(out);
            if (m) m->type = MOVE_PICKUP;
        }
        calc_pass_moves(g, p, out);
    } else if (!is_def && g->num_battles > 0) {
        bool said_good = (g->good_players_mask & (1u << bot_idx)) != 0;
        if (!said_good) {
            calc_regular_attack_moves(g, p, out);
            LegalMove *m = push_move(out);
            if (m) m->type = MOVE_GOOD;
        }
    }
    legal_stat_note(out, g, bot_idx);
}
