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
    // PODKIDNOY: this table has no transfer, so the defender's menu is covers
    // and pickup and nothing else. The gate is HERE rather than at the two call
    // sites (calculate_legal_moves and its lite twin) so that the enumerator a
    // bot searches with and the one a human's menu is built from cannot come to
    // disagree - handle_pass rejects the move either way, and a menu offering a
    // move the validator refuses is how a phantom "invalid move" reaches a
    // board.
    if (!game_pass_allowed(g)) return;
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
    // An out (hand empty, already safe) or otherwise not-in seat is a spectator:
    // no legal move. Without this, the non-defender branch below would still hand
    // an out player MOVE_GOOD, which handle_good rejects (status != IN) — the
    // legal menu and the apply-validator disagreeing, which surfaces as a phantom
    // "invalid move" the instant an out seat is queried (iMessage §out-player).
    if (p->status != PLAYER_STATUS_IN) return;
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
            // GOOD IS ALWAYS HERE WHILE THE SEAT HAS NOT SAID IT, uncovered
            // table or not. It is not slack in the menu: saying good is how an
            // attacker signals "done attacking" and LEAVES the bot loop's
            // eligible set (it sets good_players_mask, which should_bot_act
            // reads for a non-defender, which bot_drive_eligible_mask reads in
            // turn). Gate it on all-covered anywhere a bot can see and a bot
            // holding a legal throw-in could never decline one - it would be
            // forced to keep attacking until it ran out of cards, and would
            // stay eligible every cycle for as long as that lasted.
            //
            // The human rule ("no good over an uncovered attack") is a
            // narrowing of this menu, applied by play_human_menu on the way to
            // a board. It never touches this enumeration.
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
    // An out (hand empty, already safe) or otherwise not-in seat is a spectator:
    // no legal move. Without this, the non-defender branch below would still hand
    // an out player MOVE_GOOD, which handle_good rejects (status != IN) — the
    // legal menu and the apply-validator disagreeing, which surfaces as a phantom
    // "invalid move" the instant an out seat is queried (iMessage §out-player).
    if (p->status != PLAYER_STATUS_IN) return;
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
            // GOOD IS ALWAYS HERE WHILE THE SEAT HAS NOT SAID IT, uncovered
            // table or not. It is not slack in the menu: saying good is how an
            // attacker signals "done attacking" and LEAVES the bot loop's
            // eligible set (it sets good_players_mask, which should_bot_act
            // reads for a non-defender, which bot_drive_eligible_mask reads in
            // turn). Gate it on all-covered anywhere a bot can see and a bot
            // holding a legal throw-in could never decline one - it would be
            // forced to keep attacking until it ran out of cards, and would
            // stay eligible every cycle for as long as that lasted.
            //
            // The human rule ("no good over an uncovered attack") is a
            // narrowing of this menu, applied by play_human_menu on the way to
            // a board. It never touches this enumeration.
            LegalMove *m = push_move(out);
            if (m) m->type = MOVE_GOOD;
        }
    }
    legal_stat_note(out, g, bot_idx);
}

// ---------- one-tap cover resolution (F9) ----------------------------------
//
// Given a set of selected cover cards and the current table, decide whether they
// cover the uncovered attacks in exactly ONE unambiguous way — every valid full
// pairing of cover cards to distinct uncovered attacks covers the SAME set of
// attacks — so a UI can commit the cover on one gesture instead of asking the
// player which card goes on which attack. The web drag, the phone tap-commit,
// the watch chooser and iMessage all want this, and it is pure set logic over
// can_cover, so it lives here beside the legality it is built on rather than as a
// TS copy per surface (docs/C_CORE_CONSOLIDATION.md F9; ported from
// coverCombinations.ts findUnambiguousCover).
//
// Bounds are defensive, not expected: the uncovered-attack count is capped at
// UC_MAX_UNCOVERED (= a u64 pairing bitmask) and the permutation search at
// UC_SEARCH_CAP nodes. A real bout never has more than 6 uncovered attacks; a
// corrupt/hostile input that would blow the search up simply reads as "not
// unambiguous" (return 0), and the caller falls back to manual placement —
// exactly the safe degradation, never a hang.
#define UC_MAX_UNCOVERED 64
#define UC_SEARCH_CAP    200000

typedef struct {
    const Card *cover;
    int         n_cover;
    Card        uncovered[UC_MAX_UNCOVERED];
    int         n_uncovered;
    int         power_suit;
    int         first_perm[UC_MAX_UNCOVERED];  // attack index per cover card
    uint64_t    first_set;                     // bitmask of attacks it covers
    bool        have_first;
    bool        ambiguous;                      // a valid pairing covered a different set
    long        nodes;
    bool        overflow;
} UcCtx;

static void uc_recurse(UcCtx *x, int depth, uint64_t used, int *perm) {
    if (x->ambiguous || x->overflow) return;
    if (++x->nodes > UC_SEARCH_CAP) { x->overflow = true; return; }
    if (depth == x->n_cover) {
        uint64_t set = 0;
        for (int i = 0; i < x->n_cover; i++) set |= (uint64_t)1 << perm[i];
        if (!x->have_first) {
            x->have_first = true;
            x->first_set = set;
            for (int i = 0; i < x->n_cover; i++) x->first_perm[i] = perm[i];
        } else if (set != x->first_set) {
            x->ambiguous = true;   // two valid pairings cover different attacks
        }
        return;
    }
    for (int j = 0; j < x->n_uncovered; j++) {
        if (used & ((uint64_t)1 << j)) continue;
        if (!can_cover(x->uncovered[j], x->cover[depth], x->power_suit)) continue;
        perm[depth] = j;
        uc_recurse(x, depth + 1, used | ((uint64_t)1 << j), perm);
        if (x->ambiguous || x->overflow) return;
    }
}

int unambiguous_cover(const Card *cover_cards, int n_cover,
                      const Battle *battles, int n_battles, int power_suit,
                      Card *out_attacks) {
    if (n_cover <= 0) return 0;
    UcCtx x;
    x.cover = cover_cards; x.n_cover = n_cover; x.power_suit = power_suit;
    x.n_uncovered = 0; x.have_first = false; x.ambiguous = false;
    x.nodes = 0; x.overflow = false;
    for (int i = 0; i < n_battles && x.n_uncovered < UC_MAX_UNCOVERED; i++) {
        if (card_is_none(battles[i].defense)) x.uncovered[x.n_uncovered++] = battles[i].attack;
    }
    // Mirrors findUnambiguousCover's guards: no cover cards, nothing to cover, or
    // more cover cards than uncovered attacks all yield no valid full pairing.
    if (x.n_uncovered == 0 || n_cover > x.n_uncovered) return 0;
    int perm[UC_MAX_UNCOVERED];
    uc_recurse(&x, 0, 0, perm);
    if (x.overflow || x.ambiguous || !x.have_first) return 0;
    for (int i = 0; i < n_cover; i++) out_attacks[i] = x.uncovered[x.first_perm[i]];
    return 1;
}

// ---------- the packed menu wire --------------------------------------------
//
// The layout is documented in legal.h. Writer and reader sit together here so a
// byte offset that moves, moves once.

int legal_menu_write(const LegalMoves *lm, int start, int count,
                     unsigned char *out, int cap) {
    if (!lm || !out || cap < 4) return LEGAL_WIRE_ECAP;
    if (start < 0) start = 0;
    if (start > lm->n) start = lm->n;
    int end = (count < 0) ? lm->n : start + count;
    if (end > lm->n) end = lm->n;
    if (end < start) end = start;

    // The count is written LAST, so a buffer that runs out leaves no header
    // claiming moves that are not there.
    unsigned char *q = out + 4;
    for (int i = start; i < end; i++) {
        const LegalMove *m = &lm->moves[i];
        if ((int)(q - out) + 2 + 2 * m->n_cards > cap) return LEGAL_WIRE_ECAP;
        *q++ = (unsigned char)m->type;
        *q++ = (unsigned char)m->n_cards;
        for (int j = 0; j < m->n_cards; j++) *q++ = (unsigned char)card_to_id(m->cards[j]);
        for (int j = 0; j < m->n_cards; j++) *q++ = (unsigned char)card_to_id(m->attack_cards[j]);
    }
    const unsigned int n = (unsigned int)(end - start);
    out[0] = (unsigned char)(n & 0xff);
    out[1] = (unsigned char)((n >> 8) & 0xff);
    out[2] = (unsigned char)((n >> 16) & 0xff);
    out[3] = (unsigned char)((n >> 24) & 0xff);
    return (int)(q - out);
}

int legal_menu_begin(MenuWalk *w, const unsigned char *buf, int len) {
    if (!w) return LEGAL_WIRE_EPARSE;
    w->buf = buf; w->len = len; w->n = 0; w->index = -1; w->q = 4;
    if (!buf || len < 4) return LEGAL_WIRE_EPARSE;
    w->n = (int)((unsigned)buf[0] | ((unsigned)buf[1] << 8)
                 | ((unsigned)buf[2] << 16) | ((unsigned)buf[3] << 24));
    if (w->n < 0) { w->n = 0; return LEGAL_WIRE_EPARSE; }
    return w->n;
}

int legal_menu_next(MenuWalk *w, MenuMove *out) {
    if (!w || !out || !w->buf) return LEGAL_WIRE_EPARSE;
    if (w->index + 1 >= w->n) return 0;
    if (w->q + 2 > w->len) return LEGAL_WIRE_EPARSE;
    const int n_cards = w->buf[w->q + 1];
    if (w->q + 2 + 2 * n_cards > w->len) return LEGAL_WIRE_EPARSE;
    out->type = w->buf[w->q];
    out->n_cards = n_cards;
    out->cards = w->buf + w->q + 2;
    out->attacks = w->buf + w->q + 2 + n_cards;
    w->q += 2 + 2 * n_cards;
    w->index++;
    return 1;
}

// ---------- what a gesture on a board means ---------------------------------

// Card identity, over the wire bytes both sides already hold. Order never
// matters - a selection is a set - and the deck has no duplicates, so equal
// counts plus containment is set equality.
static int same_cards(const MenuMove *m, const unsigned char *sel, int n_sel) {
    if (m->n_cards != n_sel) return 0;
    for (int i = 0; i < n_sel; i++) {
        int found = 0;
        for (int j = 0; j < m->n_cards; j++) if (m->cards[j] == sel[i]) { found = 1; break; }
        if (!found) return 0;
    }
    return 1;
}

static int covers_attack(const MenuMove *m, unsigned char attack) {
    for (int i = 0; i < m->n_cards; i++) if (m->attacks[i] == attack) return 1;
    return 0;
}

static unsigned char battle_attack(const PlayBoard *b, int i) { return b->table[2 * i]; }
static int battle_is_uncovered(const PlayBoard *b, int i) {
    return b->table[2 * i + 1] == LEGAL_WIRE_NONE;
}

// A board with no table bytes has no battles, whatever it claims.
static int board_battles(const PlayBoard *b) {
    if (!b->table || b->n_battles < 0) return 0;
    return b->n_battles;
}

// How high a card stands in Durak's own order: every trump outranks every
// non-trump, and within a class the rank decides. Ranks run 1..13, so the 100
// is clear of any collision between the two classes.
static int wire_strength(unsigned char card, int power_suit) {
    const int suit = card / 13, value = card % 13 + 1;
    return value + (suit == power_suit ? 100 : 0);
}

int play_resolve(const PlayBoard *b, const unsigned char *sel, int n_sel,
                 int target) {
    if (!b || !b->menu || n_sel <= 0) return -1;
    if (target == PLAY_TARGET_HAND || target < PLAY_TARGET_HAND) return -1;

    MenuWalk w;
    MenuMove m;
    if (legal_menu_begin(&w, b->menu, b->menu_len) < 0) return -1;

    if (!b->is_defender) {
        // Attacker: the only card play is an attack with exactly this selection
        // (one card, or several of a rank - the kernel enumerates which). The
        // target is not read, which is why the hand is answered above.
        while (legal_menu_next(&w, &m) == 1)
            if (m.type == MOVE_ATTACK && same_cards(&m, sel, n_sel)) return w.index;
        return -1;
    }

    if (target >= 0) {
        // Dropped on a named battle: a cover with these cards that covers THIS
        // attack (a single cover pairs one, a multicover has it among them).
        if (target >= board_battles(b) || !battle_is_uncovered(b, target)) return -1;
        const unsigned char attack = battle_attack(b, target);
        while (legal_menu_next(&w, &m) == 1)
            if (m.type == MOVE_COVER && same_cards(&m, sel, n_sel)
                && covers_attack(&m, attack)) return w.index;
        return -1;
    }

    // Open table: a pass (bounce the bout) wins if one is legal with these
    // cards, otherwise auto-target a cover - but only when it is unambiguous,
    // i.e. exactly one menu entry uses this selection.
    int only_cover = -1, n_covers = 0;
    while (legal_menu_next(&w, &m) == 1) {
        if (m.type == MOVE_PASS && same_cards(&m, sel, n_sel)) return w.index;
        if (m.type == MOVE_COVER && same_cards(&m, sel, n_sel)
            && n_covers++ == 0) only_cover = w.index;
    }
    return n_covers == 1 ? only_cover : -1;
}

uint64_t play_coverable_battles(const PlayBoard *b,
                                const unsigned char *sel, int n_sel) {
    if (!b || !b->menu || n_sel <= 0) return 0;
    const int nb = board_battles(b);
    MenuWalk w;
    MenuMove m;
    if (legal_menu_begin(&w, b->menu, b->menu_len) < 0) return 0;
    uint64_t mask = 0;
    while (legal_menu_next(&w, &m) == 1) {
        if (m.type != MOVE_COVER || !same_cards(&m, sel, n_sel)) continue;
        for (int i = 0; i < nb && i < 64; i++) {
            if (!battle_is_uncovered(b, i)) continue;
            if (covers_attack(&m, battle_attack(b, i))) mask |= (uint64_t)1 << i;
        }
    }
    return mask;
}

int play_best_cover_target(const PlayBoard *b,
                           const unsigned char *sel, int n_sel) {
    const uint64_t mask = play_coverable_battles(b, sel, n_sel);
    int best = -1;
    for (int i = 0; i < 64; i++) {
        if (!(mask & ((uint64_t)1 << i))) continue;
        if (best < 0 || wire_strength(battle_attack(b, i), b->power_suit)
                        > wire_strength(battle_attack(b, best), b->power_suit)) best = i;
    }
    return best;
}

int play_has_verb(const PlayBoard *b, int move_type,
                  const unsigned char *sel, int n_sel) {
    if (!b || !b->menu || n_sel <= 0) return 0;
    MenuWalk w;
    MenuMove m;
    if (legal_menu_begin(&w, b->menu, b->menu_len) < 0) return 0;
    while (legal_menu_next(&w, &m) == 1)
        if (m.type == move_type && same_cards(&m, sel, n_sel)) return 1;
    return 0;
}

int play_can_say_good(const PlayBoard *b) {
    if (!b || !b->menu) return 0;
    const int nb = board_battles(b);
    if (nb == 0) return 0;
    for (int i = 0; i < nb; i++) if (battle_is_uncovered(b, i)) return 0;
    MenuWalk w;
    MenuMove m;
    if (legal_menu_begin(&w, b->menu, b->menu_len) < 0) return 0;
    while (legal_menu_next(&w, &m) == 1) if (m.type == MOVE_GOOD) return 1;
    return 0;
}

int play_human_menu(const PlayBoard *b, unsigned char *out, int cap) {
    if (!b || !b->menu || !out || cap < 4) return LEGAL_WIRE_ECAP;
    const int good_allowed = play_can_say_good(b);

    MenuWalk w;
    MenuMove m;
    if (legal_menu_begin(&w, b->menu, b->menu_len) < 0) return LEGAL_WIRE_EPARSE;
    unsigned char *p = out + 4;
    unsigned int n = 0;
    int rc;
    while ((rc = legal_menu_next(&w, &m)) == 1) {
        if (m.type == MOVE_WAIT) continue;
        if (m.type == MOVE_GOOD && !good_allowed) continue;
        if ((int)(p - out) + 2 + 2 * m.n_cards > cap) return LEGAL_WIRE_ECAP;
        *p++ = (unsigned char)m.type;
        *p++ = (unsigned char)m.n_cards;
        for (int j = 0; j < m.n_cards; j++) *p++ = m.cards[j];
        for (int j = 0; j < m.n_cards; j++) *p++ = m.attacks[j];
        n++;
    }
    if (rc == LEGAL_WIRE_EPARSE) return LEGAL_WIRE_EPARSE;
    out[0] = (unsigned char)(n & 0xff);
    out[1] = (unsigned char)((n >> 8) & 0xff);
    out[2] = (unsigned char)((n >> 16) & 0xff);
    out[3] = (unsigned char)((n >> 24) & 0xff);
    return (int)(p - out);
}
