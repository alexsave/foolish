// Handwritten 1v1 strategy — port of HandwrittenBotStrategy in
// supabase/functions/_shared/strategies/handwritten_strategy.ts.
//
// The model: never done attacking, attack with as many cards as possible,
// avoid trump attacks while the deck (or flipped trump) still has cards,
// cover only when ALL uncovered attacks can be covered together (else
// pickup), prefer non-trump everywhere, choose lowest-value tie-breaks.
//
// MOVE_WAIT is referenced in the TS source but the C legal-move enumerator
// never emits it (calculate_legal_moves only produces ATTACK/COVER/PASS/
// PICKUP/GOOD), so all `wait` branches are dropped in this port.

#include "strategy.h"
#include "card.h"
#include "game.h"
#include <stdint.h>
#include <stddef.h>

static inline int card_score(Card c, int power_suit) {
    return c.value + (c.suit == power_suit ? 1000 : 0);
}

static int compute_total_card_count(const Game *g) {
    int table = 0;
    for (int i = 0; i < g->num_battles; i++) {
        table += 1 + (!card_is_none(g->table_battles[i].defense) ? 1 : 0);
    }
    int hands = 0;
    for (int i = 0; i < g->num_players; i++) hands += g->players[i].hand_count;
    return g->deck_count + g->discard_pile_length + table + hands + (g->has_flipped ? 1 : 0);
}

static double trump_attack_probability(const Game *g) {
    if (g->deck_count > 0 || g->has_flipped) return 0.02;
    int total = compute_total_card_count(g);
    if (total < 1) total = 1;
    double ratio = (double)g->discard_pile_length / total;
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;
    double p = 0.65 + 0.35 * ratio;
    if (p < 0.5) p = 0.5;
    if (p > 0.95) p = 0.95;
    return p;
}

static bool move_has_trump(const LegalMove *m, int power_suit) {
    for (int i = 0; i < m->n_cards; i++) {
        if (m->cards[i].suit == power_suit) return true;
    }
    return false;
}

static bool move_all_non_trump(const LegalMove *m, int power_suit) {
    for (int i = 0; i < m->n_cards; i++) {
        if (m->cards[i].suit == power_suit) return false;
    }
    return true;
}

static int sum_card_score(const LegalMove *m, int power_suit) {
    int s = 0;
    for (int i = 0; i < m->n_cards; i++) s += card_score(m->cards[i], power_suit);
    return s;
}

// Among `idxs[0..n)` (indices into moves->moves), find the one whose
// n_cards is maximal; among ties, lowest summed score. Returns chosen idx.
static int pick_max_cards_lowest_score(const LegalMoves *moves, int power_suit,
                                       const int *idxs, int n) {
    int max_n = -1;
    for (int i = 0; i < n; i++) {
        int nc = moves->moves[idxs[i]].n_cards;
        if (nc > max_n) max_n = nc;
    }
    int best = -1;
    int best_score = INT32_MAX;
    for (int i = 0; i < n; i++) {
        const LegalMove *m = &moves->moves[idxs[i]];
        if (m->n_cards != max_n) continue;
        int s = sum_card_score(m, power_suit);
        if (s < best_score) { best_score = s; best = idxs[i]; }
    }
    return best;
}

// ===================================================================
// Direct rollout chooser (TASK A)
// ===================================================================
//
// handwritten_rollout_choose produces the *identical* move that
// handwritten_strategy_choose would pick from calculate_legal_moves_lite,
// but WITHOUT enumerating the full combination list first. It writes the
// chosen move into `*out` and returns true, or returns false when the
// position has no handwritten-policy move (caller falls back to the slow
// enumerate-then-pick path, which is correct for every case the direct
// chooser declines).
//
// Equivalence is validated by cd_rollout_difftest (see cordite_strategy.c,
// env CD_DIFFTEST=1): for millions of random rollout states it compares the
// move type, the card multiset, and (for covers) the attack-card pairing
// against the slow path and aborts on any divergence.

typedef struct { Card c; int score; int idx; } ScoredCard;

// Stable sort by (score asc, idx asc): selecting the prefix of length k of
// this order yields both the minimum-sum k-subset AND the lexicographically
// smallest index tuple among min-sum subsets — exactly what
// combinations_attack (index-lex order) + pick_max_cards_lowest_score
// (first strict-min sum) returns.
static void sc_sort(ScoredCard *a, int n) {
    for (int i = 1; i < n; i++) {
        ScoredCard key = a[i];
        int j = i - 1;
        while (j >= 0 && (a[j].score > key.score
                || (a[j].score == key.score && a[j].idx > key.idx))) {
            a[j + 1] = a[j]; j--;
        }
        a[j + 1] = key;
    }
}

// Build a MOVE_ATTACK / MOVE_PASS from the k lowest-score non-trump (or any)
// cards of `pool`, in the order the enumerator would emit them (ascending
// original index among the chosen set — combinations_attack copies arr in
// index order, so the chosen prefix is re-sorted by idx).
static void emit_attack_move(LegalMove *out, int8_t type,
                             ScoredCard *pool, int npool, int k) {
    (void)npool;
    // pool already sorted by (score, idx); the chosen set is the first k.
    // The enumerator emits cards in original-index order, so re-sort the
    // chosen k by idx to reproduce the exact card array (multiset identical
    // regardless, but we match ordering for cleanliness).
    ScoredCard chosen[MAX_MOVE_CARDS];
    for (int i = 0; i < k; i++) chosen[i] = pool[i];
    for (int i = 1; i < k; i++) {
        ScoredCard key = chosen[i]; int j = i - 1;
        while (j >= 0 && chosen[j].idx > key.idx) { chosen[j + 1] = chosen[j]; j--; }
        chosen[j + 1] = key;
    }
    out->type = type;
    out->n_cards = (int8_t)k;
    for (int i = 0; i < k; i++) out->cards[i] = chosen[i].c;
}

// Returns true and fills *out with handwritten's chosen lite move; false if
// it declines (caller uses the slow path).
bool handwritten_rollout_choose(const Game *g, int bot_idx, LegalMove *out) {
    if (g->status != GAME_STATUS_PLAYING) return false;
    const Player *p = &g->players[bot_idx];
    // Degenerate positions where a move could exceed MAX_MOVE_CARDS slots are
    // left to the slow path (the enumerator's own handling, buggy or not, is
    // then the reference; the fast path must never disagree).
    if (g->num_battles > MAX_MOVE_CARDS) return false;
    int power = g->power_suit;
    bool is_def = (bot_idx == g->defender);
    bool is_first_attacker = (bot_idx == g->first_attacker);
    bool first_attack = (g->num_battles == 0);

    int defender_cards = g->players[g->defender].hand_count;
    int uncovered = 0;
    for (int i = 0; i < g->num_battles; i++)
        if (!!card_is_none(g->table_battles[i].defense)) uncovered++;

    // ---------- Attacker: first attack ----------------------------------
    if (first_attack && is_first_attacker) {
        // Non-trump first attack: among values, most non-trump cards (capped
        // by defender_cards), ties -> lowest value. Move = those cards.
        // Trump-only attacks considered only if no non-trump attack legal.
        int best_v_nt = -1, best_k_nt = 0;
        int best_v_tr = -1, best_k_tr = 0;
        // Walk values in first-seen hand order to match enumerator value order
        // for tie handling (lowest value wins anyway, so value order suffices).
        for (int v = 0; v <= ACE_VALUE; v++) {
            int nt = 0, tr = 0;
            for (int i = 0; i < p->hand_count; i++) {
                if (p->hand[i].value != v) continue;
                if (p->hand[i].suit == power) tr++; else nt++;
            }
            if (nt > 0) {
                int k = nt; if (k > defender_cards) k = defender_cards;
                if (k >= 1) {
                    // most cards, ties -> lowest value (v ascending, so first
                    // strictly-greater-k wins; equal-k keeps the lower v).
                    if (k > best_k_nt) { best_k_nt = k; best_v_nt = v; }
                }
            }
            // A trump-only attack of this value: all cards same value are
            // trump (tr>0 && nt==0). Mixed values can't form one move here.
            if (tr > 0 && nt == 0) {
                int k = tr; if (k > defender_cards) k = defender_cards;
                if (k >= 1 && k > best_k_tr) { best_k_tr = k; best_v_tr = v; }
            }
        }
        if (best_v_nt >= 0) {
            ScoredCard pool[MAX_HAND_SIZE]; int np = 0;
            for (int i = 0; i < p->hand_count; i++) {
                if (p->hand[i].value == best_v_nt && p->hand[i].suit != power)
                    pool[np++] = (ScoredCard){ p->hand[i], card_score(p->hand[i], power), i };
            }
            if (best_k_nt > MAX_MOVE_CARDS) return false;
            sc_sort(pool, np);
            emit_attack_move(out, MOVE_ATTACK, pool, np, best_k_nt);
            return true;
        }
        if (best_v_tr >= 0) {
            // Trump attack gated by probability; on decline handwritten falls
            // through to GOOD/forced fallbacks. There is no GOOD for a first
            // attacker, and the forced fallback re-picks an attack. Defer to
            // the slow path for this rare RNG-gated branch to stay exact.
            return false;
        }
        // No attack at all (e.g. defender_cards == 0): slow path.
        return false;
    }

    // ---------- Defender ------------------------------------------------
    if (is_def && g->num_battles > 0) {
        // Handwritten covers ONLY when it can fully cover all uncovered
        // attacks (else pickup). The lite enumerator's greedy cover is the
        // lowest-score full cover; handwritten then picks the lowest-PRODUCT
        // full cover. With lite producing exactly ONE cover move, the policy
        // either takes that single cover or picks up. We must replicate the
        // greedy cover (calc_cover_moves_greedy) and the full-cover test.
        Card uc[MAX_BATTLES];
        for (int i = 0, j = 0; i < g->num_battles; i++)
            if (!!card_is_none(g->table_battles[i].defense)) uc[j++] = g->table_battles[i].attack;

        // Pass branch takes priority in handwritten only AFTER attacks; for a
        // defender there are no attack moves, so order is: (lite) cover move
        // exists -> but handwritten evaluates PASS before COVER. Replicate the
        // exact handwritten order: pass first (if any), then cover-if-full,
        // then pickup.
        // --- pass moves ---
        bool any_cov = (uncovered != g->num_battles);
        if (!any_cov && g->num_battles > 0) {
            int v0 = g->table_battles[0].attack.value;
            bool same = true;
            for (int i = 1; i < g->num_battles; i++)
                if (g->table_battles[i].attack.value != v0) { same = false; break; }
            if (same) {
                ScoredCard pool[MAX_HAND_SIZE]; int np = 0;
                for (int i = 0; i < p->hand_count; i++)
                    if (p->hand[i].value == v0)
                        pool[np++] = (ScoredCard){ p->hand[i], card_score(p->hand[i], power), i };
                if (np > 0) {
                    int next = get_next_player_index(g, g->defender);
                    int next_cards = g->players[next].hand_count;
                    // pass legal for size k iff next_cards >= k + num_battles.
                    // handwritten picks lowest summed score among ALL legal
                    // pass moves (any size). Min sum -> smallest single card
                    // (k=1) when legal, since adding cards only increases sum.
                    sc_sort(pool, np);
                    // Find the smallest k>=1 with a legal pass; min-sum is the
                    // k smallest cards. Smallest sum overall is k=1 if legal.
                    int kmin = -1;
                    for (int k = 1; k <= np; k++) {
                        if (next_cards >= k + g->num_battles) { kmin = k; break; }
                    }
                    if (kmin >= 1) {
                        if (kmin > MAX_MOVE_CARDS) return false;
                        // Among all legal sizes, min summed score is the
                        // smallest legal k (each extra card adds >=0). Equal
                        // only if extra cards score 0 (impossible: value>=1).
                        emit_attack_move(out, MOVE_PASS, pool, np, kmin);
                        return true;
                    }
                }
            }
        }

        // --- cover-if-full (greedy lowest-score full cover) ---
        bool used[MAX_HAND_SIZE] = { false };
        Card covers[MAX_BATTLES];
        bool full = true;
        for (int i = 0; i < uncovered; i++) {
            int best = -1, best_score = INT32_MAX;
            for (int j = 0; j < p->hand_count; j++) {
                if (used[j]) continue;
                if (can_cover(uc[i], p->hand[j], power)) {
                    int s = card_score(p->hand[j], power);
                    if (s < best_score) { best_score = s; best = j; }
                }
            }
            if (best < 0) { full = false; break; }
            used[best] = true;
            covers[i] = p->hand[best];
        }
        if (full) {
            out->type = MOVE_COVER;
            out->n_cards = (int8_t)uncovered;
            for (int i = 0; i < uncovered; i++) {
                out->cards[i] = covers[i];
                out->attack_cards[i] = uc[i];
            }
            return true;
        }
        // Can't fully cover -> pickup (only if not all covered, which holds).
        if (uncovered > 0) {
            out->type = MOVE_PICKUP;
            out->n_cards = 0;
            return true;
        }
        return false;
    }

    // ---------- Attacker: regular (additional) attack -------------------
    if (!is_def && g->num_battles > 0) {
        bool said_good = (g->good_players_mask & (1u << bot_idx)) != 0;
        if (said_good) return false;  // no moves; slow path returns -1 anyway
        // valid = hand cards whose value is on the table.
        bool table_values[16] = { false };
        for (int i = 0; i < g->num_battles; i++) {
            table_values[g->table_battles[i].attack.value] = true;
            if (!card_is_none(g->table_battles[i].defense))
                table_values[g->table_battles[i].defense.value] = true;
        }
        int cap = defender_cards - uncovered;   // max k via emit_attack guard
        // Non-trump valid cards.
        ScoredCard nt[MAX_HAND_SIZE]; int n_nt = 0;
        int n_tr = 0;   // only the count matters: trump attacks are deferred
        for (int i = 0; i < p->hand_count; i++) {
            if (!table_values[p->hand[i].value]) continue;
            if (p->hand[i].suit == power) n_tr++;
            else nt[n_nt++] = (ScoredCard){ p->hand[i], card_score(p->hand[i], power), i };
        }
        if (n_nt > 0 && cap >= 1) {
            int k = n_nt; if (k > cap) k = cap;
            if (k > MAX_MOVE_CARDS) return false;
            sc_sort(nt, n_nt);
            emit_attack_move(out, MOVE_ATTACK, nt, n_nt, k);
            return true;
        }
        // No legal non-trump attack. If there is also no legal trump attack,
        // handwritten falls straight through to GOOD (the only other legal
        // move for an additional attacker) — the overwhelmingly common case
        // (an attacker usually can't add cards). Emit GOOD directly.
        bool have_trump_attack = (n_tr > 0 && cap >= 1);
        if (!have_trump_attack) {
            // handwritten reaches GOOD via its `n_goods>0` branch, which draws
            // one game_random() to index among GOOD moves (always exactly one
            // GOOD here). Consume the identical draw so the rollout RNG stream
            // stays bit-identical to the enumerate-then-pick path.
            (void)game_random();
            out->type = MOVE_GOOD;
            out->n_cards = 0;
            return true;
        }
        // A trump attack exists: handwritten RNG-gates it (and on decline
        // emits GOOD). Defer this rarer branch to the slow path to stay exact.
        return false;
    }

    return false;
}

int handwritten_strategy_choose(const Game *g, int bot_idx,
                                const LegalMoves *moves, void *ctx) {
    (void)bot_idx; (void)ctx;
    if (moves->n == 0) return -1;
    int power = g->power_suit;

    int attacks[MAX_LEGAL_MOVES],   n_attacks = 0;
    int covers[MAX_LEGAL_MOVES],    n_covers = 0;
    int passes[MAX_LEGAL_MOVES],    n_passes = 0;
    int goods[MAX_LEGAL_MOVES],     n_goods = 0;
    int pickups[MAX_LEGAL_MOVES],   n_pickups = 0;
    for (int i = 0; i < moves->n; i++) {
        switch (moves->moves[i].type) {
            case MOVE_ATTACK: attacks[n_attacks++] = i; break;
            case MOVE_COVER:  covers[n_covers++]   = i; break;
            case MOVE_PASS:   passes[n_passes++]   = i; break;
            case MOVE_GOOD:   goods[n_goods++]     = i; break;
            case MOVE_PICKUP: pickups[n_pickups++] = i; break;
            default: break;
        }
    }

    // ---- Attack branch ----------------------------------------------
    if (n_attacks > 0) {
        int non_trump[MAX_LEGAL_MOVES]; int n_nt = 0;
        int trump[MAX_LEGAL_MOVES];     int n_tr = 0;
        for (int i = 0; i < n_attacks; i++) {
            const LegalMove *m = &moves->moves[attacks[i]];
            if (move_all_non_trump(m, power)) non_trump[n_nt++] = attacks[i];
            else if (move_has_trump(m, power)) trump[n_tr++] = attacks[i];
        }
        const int *candidates = NULL; int n_cand = 0;
        if (n_nt > 0) {
            candidates = non_trump; n_cand = n_nt;
        } else if (n_tr > 0) {
            if (game_random() < trump_attack_probability(g)) {
                candidates = trump; n_cand = n_tr;
            } else {
                // Decline trump attack: prefer GOOD (end round) over falling
                // through to pass/cover. (TS also checks `wait`, dropped.)
                if (n_goods > 0) return goods[0];
                // else fall through to non-attack branches
            }
        }
        if (candidates) {
            return pick_max_cards_lowest_score(moves, power, candidates, n_cand);
        }
    }

    // ---- Pass branch (lowest-value cards) ---------------------------
    if (n_passes > 0) {
        int best = passes[0];
        int best_score = INT32_MAX;
        for (int i = 0; i < n_passes; i++) {
            int s = sum_card_score(&moves->moves[passes[i]], power);
            if (s < best_score) { best_score = s; best = passes[i]; }
        }
        return best;
    }

    // ---- Cover branch — only if we can cover ALL uncovered attacks --
    if (n_covers > 0) {
        int uncovered = 0;
        for (int i = 0; i < g->num_battles; i++) {
            if (!!card_is_none(g->table_battles[i].defense)) uncovered++;
        }
        int full[MAX_LEGAL_MOVES]; int n_full = 0;
        for (int i = 0; i < n_covers; i++) {
            if (moves->moves[covers[i]].n_cards == uncovered) full[n_full++] = covers[i];
        }
        if (n_full > 0) {
            // PRODUCT of card scores (TS uses *=, matching the original aiDefend
            // logic: penalize using power cards much more than additively).
            int best = full[0];
            // Use double for the product to avoid overflow when scores can
            // exceed 1000 (trump bonus) over multiple cards.
            double best_score = 1e30;
            for (int i = 0; i < n_full; i++) {
                const LegalMove *m = &moves->moves[full[i]];
                double s = 1.0;
                for (int j = 0; j < m->n_cards; j++) s *= (double)card_score(m->cards[j], power);
                if (s < best_score) { best_score = s; best = full[i]; }
            }
            return best;
        }
        // Can't fully cover → fall through (no partial cover).
    }

    // ---- Non-attack/cover/pass/pickup moves: pick GOOD if available -
    if (n_goods > 0) {
        // The TS picks randomly among "non-attack non-pickup non-wait" — only
        // GOOD ever lands here for our legal-move set, so pick the first.
        int idx = (int)(game_random() * n_goods);
        if (idx < 0) idx = 0; if (idx >= n_goods) idx = n_goods - 1;
        return goods[idx];
    }

    // ---- Forced attack fallback -------------------------------------
    if (n_attacks > 0) {
        if (g->deck_count > 0 || g->has_flipped) {
            int nt[MAX_LEGAL_MOVES]; int nn = 0;
            for (int i = 0; i < n_attacks; i++) {
                if (move_all_non_trump(&moves->moves[attacks[i]], power)) nt[nn++] = attacks[i];
            }
            if (nn > 0) {
                // Prefer most cards, ties → lowest summed score.
                return pick_max_cards_lowest_score(moves, power, nt, nn);
            }
            if (n_goods > 0) return goods[0];
        }
        // No good fallback — pick most-cards, lowest-score among all attacks.
        return pick_max_cards_lowest_score(moves, power, attacks, n_attacks);
    }

    // ---- Pickup as absolute last resort -----------------------------
    if (n_pickups > 0) return pickups[0];

    // ---- Final fallback: random move (should be unreachable) --------
    int idx = (int)(game_random() * moves->n);
    if (idx < 0) idx = 0; if (idx >= moves->n) idx = moves->n - 1;
    return idx;
}
