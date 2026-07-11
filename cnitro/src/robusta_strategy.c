// Robusta — public-info-only strategy. Same intent as espresso (beat
// handwritten), but never looks at opponent hands. The "guess" is an
// unseen-pool inference: every card not visible to us (not in my hand,
// not on the table, not in discards, not the flipped trump) is a
// candidate for being in the deck OR some opponent's hand. We score
// moves against expected outcomes computed from that pool plus public
// hand-counts.

#include "robusta_strategy.h"
#include "card.h"
#include "game.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>   // malloc — wasm-shim/native both; see stack-hoist note below

// The MC rollout puts a full Game (~67 KiB) and a LegalMoves (~232 KiB) in
// play per sample. Natively that lives on an 8 MB stack, but the bots.wasm
// module ships a 22 KiB shadow stack, so those buffers MUST NOT be stack
// locals there (an on-stack LegalMoves alone traps under --stack-first). They
// are lazily malloc'd into per-thread scratch instead — the exact pattern the
// blackpowder solver already uses (bp_solver_child), and behaviour-neutral:
// each buffer is written-before-read within a single call, the rollout policy
// is a leaf that never re-enters these functions, and the pointers are
// _Thread_local so native OMP parallel eval keeps one buffer per thread.

// ---------- helpers ---------------------------------------------------

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

static double get_trump_attack_probability(const Game *g) {
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

static bool set_contains(const Card *arr, int n, Card c) {
    for (int i = 0; i < n; i++) if (card_eq(arr[i], c)) return true;
    return false;
}

// Build the unseen pool: cards in the full deck minus everything I've
// publicly seen. Anything in this pool could be in the deck or in some
// opponent's hand — that's the irreducible uncertainty we model.
//
// We *also* track cards that we know are specifically in a given opp's
// hand right now, via LOG_PICKUP — when opp X picks up an uncovered attack
// pile, those cards enter X's hand publicly. We follow them through X's
// subsequent attack/cover/pass plays and remove them when X discards them.
// These "pinned" cards are NOT in the unseen pool — we know exactly where
// they are.
typedef struct {
    Card pool[80];
    int  n;            // size of unseen pool (excludes pinned cards)
    int  trumps;       // count of trumps in the pool
    int  opp_hand_total; // sum of hand_counts across IN opponents
    int  in_opp_count;   // number of IN opponents
    int  deck_size;      // public deck count (+1 if flipped is still up)
    // Per-opp pinned cards (publicly known to be in their hand right now).
    Card pinned[MAX_PLAYERS][MAX_HAND_SIZE];
    int  pinned_n[MAX_PLAYERS];
} UnseenPool;

// Track cards that are publicly known to currently sit in a specific opp's
// hand. We start from each LOG_PICKUP log entry (cards land in that player's
// hand) and remove cards as that player plays them (attack/cover/pass).
static void compute_pinned_per_opp(const Game *g, int bot_idx, UnseenPool *u) {
    for (int p = 0; p < MAX_PLAYERS; p++) u->pinned_n[p] = 0;
    for (int i = 0; i < g->num_logs; i++) {
        const GameLog *L = &g->logs[i];
        if (L->log_type == LOG_PICKUP) {
            int p = L->player_idx;
            if (p < 0 || p >= g->num_players || p == bot_idx) continue;
            // Pickup adds the (attack) cards onto p's hand. Engine stores
            // them as `primary` in card_pairs.
            for (int k = 0; k < L->num_pairs && u->pinned_n[p] < MAX_HAND_SIZE; k++) {
                u->pinned[p][u->pinned_n[p]++] = L->pairs[k].primary;
            }
        } else if (L->log_type == LOG_ATTACK
                   || L->log_type == LOG_COVER
                   || L->log_type == LOG_PASS) {
            int p = L->player_idx;
            if (p < 0 || p >= g->num_players || p == bot_idx) continue;
            // Cards leave p's hand. Remove them from pinned if present.
            for (int k = 0; k < L->num_pairs; k++) {
                Card c = L->pairs[k].primary;
                for (int q = 0; q < u->pinned_n[p]; q++) {
                    if (card_eq(u->pinned[p][q], c)) {
                        u->pinned[p][q] = u->pinned[p][u->pinned_n[p] - 1];
                        u->pinned_n[p]--;
                        break;
                    }
                }
            }
        }
    }
    // Drop pinned cards for players who are no longer IN (their hand cleared).
    for (int p = 0; p < g->num_players; p++) {
        if (g->players[p].status != PLAYER_STATUS_IN) u->pinned_n[p] = 0;
    }
}

static void build_unseen_pool(const Game *g, int bot_idx, UnseenPool *u) {
    compute_pinned_per_opp(g, bot_idx, u);
    // Known cards = my hand + table battles + discard log + flipped
    //             + all pinned cards (specifically located in some opp's hand).
    Card known[160];
    int kn = 0;
    const Player *bot = &g->players[bot_idx];
    for (int j = 0; j < bot->hand_count; j++) known[kn++] = bot->hand[j];
    for (int i = 0; i < g->num_battles; i++) {
        known[kn++] = g->table_battles[i].attack;
        if (!card_is_none(g->table_battles[i].defense)) known[kn++] = g->table_battles[i].defense;
    }
    if (g->has_flipped) known[kn++] = g->flipped;
    for (int i = 0; i < g->num_logs; i++) {
        if (g->logs[i].log_type == LOG_DISCARD) {
            for (int j = 0; j < g->logs[i].num_pairs; j++) {
                known[kn++] = g->logs[i].pairs[j].primary;
            }
        }
    }
    for (int p = 0; p < g->num_players; p++) {
        for (int j = 0; j < u->pinned_n[p] && kn < (int)(sizeof(known)/sizeof(known[0])); j++) {
            known[kn++] = u->pinned[p][j];
        }
    }

    int start_v = min_value_for(g->num_players);
    u->n = 0;
    u->trumps = 0;
    for (int suit = 0; suit < 4; suit++) {
        for (int v = start_v; v <= ACE_VALUE; v++) {
            Card c = { (int8_t)suit, (int8_t)v };
            if (!set_contains(known, kn, c)) {
                u->pool[u->n++] = c;
                if (suit == g->power_suit) u->trumps++;
            }
        }
    }

    u->opp_hand_total = 0;
    u->in_opp_count = 0;
    for (int i = 0; i < g->num_players; i++) {
        if (i == bot_idx || g->players[i].status != PLAYER_STATUS_IN) continue;
        // opp_hand_total counts only the *unknown* portion of their hand.
        int unk = g->players[i].hand_count - u->pinned_n[i];
        if (unk < 0) unk = 0;
        u->opp_hand_total += unk;
        u->in_opp_count++;
    }
    u->deck_size = g->deck_count + (g->has_flipped ? 1 : 0);
}

// Count cards in the unseen pool that could cover `attack` given trump.
static int pool_covers_count(const UnseenPool *u, Card attack, int power_suit) {
    int n = 0;
    for (int i = 0; i < u->n; i++) {
        if (can_cover(attack, u->pool[i], power_suit)) n++;
    }
    return n;
}

// Count cards in the unseen pool with a given value (any suit).
static int pool_value_count(const UnseenPool *u, int value) {
    int n = 0;
    for (int i = 0; i < u->n; i++) if (u->pool[i].value == value) n++;
    return n;
}

// ---------- move-type helpers ----------------------------------------

static bool move_is_attack_all_non_trump(const LegalMove *m, int power_suit) {
    if (m->type != MOVE_ATTACK) return false;
    for (int i = 0; i < m->n_cards; i++) if (m->cards[i].suit == power_suit) return false;
    return true;
}
static bool move_is_attack_any_trump(const LegalMove *m, int power_suit) {
    if (m->type != MOVE_ATTACK) return false;
    for (int i = 0; i < m->n_cards; i++) if (m->cards[i].suit == power_suit) return true;
    return false;
}

// ---------- attack scoring -------------------------------------------

// Defender's hand size (or smallest IN opponent if defender is us / out).
static int defender_hand_size(const Game *g, int bot_idx) {
    if (g->defender != bot_idx && g->players[g->defender].status == PLAYER_STATUS_IN) {
        return g->players[g->defender].hand_count;
    }
    int min_h = 99;
    for (int i = 0; i < g->num_players; i++) {
        if (i == bot_idx || g->players[i].status != PLAYER_STATUS_IN) continue;
        if (g->players[i].hand_count < min_h) min_h = g->players[i].hand_count;
    }
    return (min_h == 99) ? 0 : min_h;
}

// P(defender has zero cards that cover the attack), using sampling-without-
// replacement (hypergeometric: pool of size N, K "good" cards, draw n).
static double prob_no_cover(const UnseenPool *u, const Game *g, int bot_idx,
                             Card attack) {
    int N = u->n;
    if (N <= 0) return 1.0;
    int K = pool_covers_count(u, attack, g->power_suit);
    int n = defender_hand_size(g, bot_idx);
    if (K == 0) return 1.0;
    if (n <= 0) return 1.0;
    if (K >= N) return 0.0;
    double p = 1.0;
    for (int i = 0; i < n; i++) {
        int remaining_good = K;
        int remaining_total = N - i;
        if (remaining_total - remaining_good <= 0) return 0.0;
        // P(i-th draw is non-cover | previous i draws were non-cover)
        // = (N - K - i) / (N - i)
        p *= (double)(N - K - i) / (double)remaining_total;
        if (p <= 0.0) return 0.0;
    }
    return p;
}

// Expected pile-on threat: across non-defender opponents, expected count of
// same-value cards they could throw at us.
static double expected_pile_on(const UnseenPool *u, const Game *g, int bot_idx, int value) {
    int vc = pool_value_count(u, value);
    if (u->n == 0) return 0.0;
    double frac = (double)vc / (double)u->n;
    int hand = 0;
    for (int i = 0; i < g->num_players; i++) {
        if (i == bot_idx || g->players[i].status != PLAYER_STATUS_IN) continue;
        if (i == g->defender) continue;
        hand += g->players[i].hand_count;
    }
    return frac * (double)hand;
}

// ---------- 1v1 rollout (espresso-style, but with a sampled opp hand) -----
//
// Sample `hand_size` cards uniformly without replacement from the pool using
// a deterministic xorshift seeded by `seed`. Writes into `out`.
static uint32_t xorshift32(uint32_t s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s ? s : 1;
}

static void sample_opp_hand(const UnseenPool *u, int hand_size,
                             uint32_t seed, Card *out, int *out_n) {
    Card tmp[80];
    int N = u->n;
    if (N > 80) N = 80;
    for (int i = 0; i < N; i++) tmp[i] = u->pool[i];
    uint32_t s = seed ? seed : 0xC0FFEEu;
    int take = hand_size < N ? hand_size : N;
    for (int i = 0; i < take; i++) {
        s = xorshift32(s);
        int j = i + (int)(s % (uint32_t)(N - i));
        Card sw = tmp[i]; tmp[i] = tmp[j]; tmp[j] = sw;
    }
    *out_n = take;
    for (int i = 0; i < take; i++) out[i] = tmp[i];
}

typedef struct {
    Card my_hand[MAX_HAND_SIZE]; int my_n;
    Card opp_hand[MAX_HAND_SIZE]; int opp_n;
    bool pickup;
} RolloutResult;

// Greedy lowest-score canCover, same as espresso.
static void predict_cover(const Card *attack, int attack_n,
                          const Card *opp_hand, int opp_n,
                          int power_suit,
                          Card *covers, int *covers_n, bool *pickup) {
    Card remaining[MAX_HAND_SIZE];
    int rn = opp_n;
    for (int i = 0; i < opp_n; i++) remaining[i] = opp_hand[i];
    *covers_n = 0;
    *pickup = false;
    for (int a = 0; a < attack_n; a++) {
        int best_idx = -1;
        int best_score = INT32_MAX;
        for (int i = 0; i < rn; i++) {
            if (can_cover(attack[a], remaining[i], power_suit)) {
                int s = card_score(remaining[i], power_suit);
                if (s < best_score) { best_score = s; best_idx = i; }
            }
        }
        if (best_idx < 0) {
            *covers_n = 0; *pickup = true; return;
        }
        covers[(*covers_n)++] = remaining[best_idx];
        for (int i = best_idx + 1; i < rn; i++) remaining[i - 1] = remaining[i];
        rn--;
    }
}

// 1-ply rollout starting with my attack. Returns the resulting hand state
// and whether opp ultimately picked up.
static void rollout_round(const Card *first_attack, int fa_n,
                          const Card *my_hand_in, int my_n_in,
                          const Card *opp_hand_in, int opp_n_in,
                          int power_suit, RolloutResult *r) {
    Card my[MAX_HAND_SIZE]; int mN = my_n_in;
    Card opp[MAX_HAND_SIZE]; int oN = opp_n_in;
    for (int i = 0; i < my_n_in; i++) my[i] = my_hand_in[i];
    for (int i = 0; i < opp_n_in; i++) opp[i] = opp_hand_in[i];

    bool table_values[16] = { false };
    Card attack[MAX_MOVE_CARDS]; int an = fa_n;
    for (int i = 0; i < fa_n; i++) attack[i] = first_attack[i];

    for (int iter = 0; iter < 5; iter++) {
        int new_mN = 0;
        for (int i = 0; i < mN; i++) {
            if (!set_contains(attack, an, my[i])) my[new_mN++] = my[i];
        }
        mN = new_mN;
        for (int i = 0; i < an; i++) table_values[attack[i].value] = true;

        Card covers[MAX_MOVE_CARDS]; int cn; bool pickup;
        predict_cover(attack, an, opp, oN, power_suit, covers, &cn, &pickup);
        if (pickup) {
            for (int i = 0; i < an; i++) opp[oN++] = attack[i];
            r->pickup = true;
            r->my_n = mN; r->opp_n = oN;
            for (int i = 0; i < mN; i++) r->my_hand[i] = my[i];
            for (int i = 0; i < oN; i++) r->opp_hand[i] = opp[i];
            return;
        }
        int new_oN = 0;
        for (int i = 0; i < oN; i++) {
            if (!set_contains(covers, cn, opp[i])) opp[new_oN++] = opp[i];
        }
        oN = new_oN;
        for (int i = 0; i < cn; i++) table_values[covers[i].value] = true;

        Card matching[MAX_HAND_SIZE]; int mn = 0;
        for (int i = 0; i < mN; i++) {
            if (table_values[my[i].value] && my[i].suit != power_suit) matching[mn++] = my[i];
        }
        if (mn == 0) break;

        bool seen_v[16] = { false };
        Card best_group[MAX_MOVE_CARDS]; int best_n = 0; int best_sum = INT32_MAX;
        for (int i = 0; i < mn; i++) {
            int v = matching[i].value;
            if (seen_v[v]) continue;
            seen_v[v] = true;
            Card group[MAX_MOVE_CARDS]; int gn = 0; int sum = 0;
            for (int j = 0; j < mn; j++) {
                if (matching[j].value == v) { group[gn++] = matching[j]; sum += matching[j].value; }
            }
            bool better = (gn > best_n) || (gn == best_n && sum < best_sum);
            if (better) {
                best_n = gn; best_sum = sum;
                for (int j = 0; j < gn; j++) best_group[j] = group[j];
            }
        }
        an = best_n < oN ? best_n : oN;
        for (int i = 0; i < an; i++) attack[i] = best_group[i];
        if (an == 0) break;
    }
    r->pickup = false; r->my_n = mN; r->opp_n = oN;
    for (int i = 0; i < mN; i++) r->my_hand[i] = my[i];
    for (int i = 0; i < oN; i++) r->opp_hand[i] = opp[i];
}

// ---------- Monte Carlo full-game evaluation -------------------------
//
// For each candidate move, sample a consistent fictional state (opp hands
// + deck order drawn from the unseen pool, matching everyone's public
// hand-counts), apply the move, then simulate the rest of the game with
// every player using handwritten. Returns expected seat-0 finish position
// — lower = better.

// Sample a fictional consistent state into `g_out`. Pinned cards (from
// pickup logs) go to their known holder; the unseen pool shuffles into the
// remaining opp-hand slots + the deck.
static bool sample_consistent_state(Game *g_out, const Game *g_in, int my_idx,
                                     const UnseenPool *u, uint32_t seed) {
    game_clone(g_out, g_in);

    // Step 1: place pinned cards in the right opp's hand at slot 0..pinned_n.
    for (int i = 0; i < g_in->num_players; i++) {
        if (i == my_idx) continue;
        for (int k = 0; k < u->pinned_n[i]; k++) {
            g_out->players[i].hand[k] = u->pinned[i][k];
        }
    }

    // Step 2: shuffle the unseen pool, deal into remaining opp-hand slots
    // and the deck.
    Card hidden[80]; int hn = 0;
    for (int i = 0; i < u->n && hn < 80; i++) hidden[hn++] = u->pool[i];
    if (hn == 0) return true;
    uint32_t s = seed ? seed : 0xCAFEu;
    for (int i = hn - 1; i > 0; i--) {
        s = xorshift32(s);
        int j = (int)(s % (uint32_t)(i + 1));
        Card sw = hidden[i]; hidden[i] = hidden[j]; hidden[j] = sw;
    }
    int k = 0;
    // Re-deal: deck first, then unknown portions of each opp's hand.
    for (int i = 0; i < g_in->deck_count && k < hn; i++) g_out->deck[i] = hidden[k++];
    for (int i = 0; i < g_in->num_players; i++) {
        if (i == my_idx) continue;
        int need = g_in->players[i].hand_count - u->pinned_n[i];
        for (int j = 0; j < need && k < hn; j++) {
            g_out->players[i].hand[u->pinned_n[i] + j] = hidden[k++];
        }
    }
    return true;
}

// Apply a move to game `g` for player `p_idx`. Returns true on success.
static bool apply_move(Game *g, int p_idx, const LegalMove *m) {
    switch (m->type) {
        case MOVE_ATTACK: return handle_attack(g, p_idx, m->cards, m->n_cards);
        case MOVE_COVER:  return handle_cover (g, p_idx, m->cards, m->attack_cards, m->n_cards);
        case MOVE_PASS:   return handle_pass  (g, p_idx, m->cards, m->n_cards);
        case MOVE_PICKUP: return handle_pickup(g, p_idx);
        case MOVE_GOOD:   return handle_good  (g, p_idx);
        default:          return false;
    }
}

// Roll the game forward using `rollout_fn` for every seat, until completion
// or `max_turns` reached. Uses calculate_legal_moves_lite (single greedy
// cover) so cover enumeration doesn't blow up. Callers pass:
//   - handwritten_strategy_choose for robusta (safe deterministic baseline)
//   - espresso_strategy_choose  for firecracker (cheats inside the
//     fictional state — which is robusta's own MC sample, not real cards)
static int simulate_to_end(Game *g, int my_idx, int max_turns, StrategyFn rollout_fn) {
    static _Thread_local LegalMoves *moves = NULL;  // hoisted off the stack
    if (!moves) { moves = malloc(sizeof(LegalMoves)); if (!moves) return 0; }
    int turns = 0;
    while (game_done(g) < 0 && turns++ < max_turns) {
        int elig[MAX_PLAYERS]; int n_e = 0;
        for (int i = 0; i < g->num_players; i++) if (should_bot_act(g, i)) elig[n_e++] = i;
        if (n_e == 0) break;
        bool acted = false;
        for (int k = 0; k < n_e; k++) {
            int pi = elig[k];
            calculate_legal_moves_lite(g, pi, moves);
            if (moves->n == 0) continue;
            int idx = rollout_fn(g, pi, moves, NULL);
            if (idx < 0 || idx >= moves->n) continue;
            if (apply_move(g, pi, &moves->moves[idx])) { acted = true; break; }
        }
        if (!acted) break;
    }
    if (game_done(g) < 0) return 0;
    for (int i = 0; i < g->num_eliminated; i++) {
        if (g->elimination_order[i] == my_idx) return i + 1;
    }
    return g->num_players;  // durak
}

// Evaluate a candidate move via Monte Carlo: avg seat-`my_idx` finish.
static double mc_eval_move(const Game *g_orig, int my_idx, const LegalMove *m,
                            const UnseenPool *u, int n_samples, uint32_t base_seed,
                            StrategyFn rollout_fn) {
    double total = 0.0;
    int valid = 0;
    static _Thread_local Game *g = NULL;   // hoisted off the stack (see note atop file)
    if (!g) { g = malloc(sizeof(Game)); if (!g) return (double)g_orig->num_players; }
    for (int s = 0; s < n_samples; s++) {
        uint32_t seed = base_seed + (uint32_t)(s + 1) * 0x85EBCA77u;
        if (!sample_consistent_state(g, g_orig, my_idx, u, seed)) continue;
        if (!apply_move(g, my_idx, m)) continue;
        int fp = simulate_to_end(g, my_idx, 600, rollout_fn);
        if (fp == 0) fp = g->num_players;  // count incomplete as durak (worst)
        total += (double)fp;
        valid++;
    }
    return valid > 0 ? total / (double)valid : (double)g_orig->num_players;
}

// ---------- legacy 1-ply rollout (used by older paths) ---------------

// Average rollout outcome across `n_samples` sampled opp hands. Returns an
// eval matching the espresso 1v1 formula:
//   size * (opp_n - my_n) + 1.5 * (my_t - opp_t) + 3*pickup
// where size = 1.0 if pickup OR no-deck, else 0.0.
static double rollout_eval(const UnseenPool *u, const Game *g, int bot_idx,
                            const Card *attack_cards, int attack_n,
                            const Card *my_hand, int my_hand_n,
                            int opp_hand_size, int n_samples) {
    if (opp_hand_size <= 0 || u->n <= 0) return 0.0;
    bool deck_active = (g->deck_count > 0 || g->has_flipped);
    double total = 0.0;
    int valid = 0;
    for (int s = 0; s < n_samples; s++) {
        // Seed varies across samples and across this call.
        uint32_t seed = (uint32_t)(0x9E3779B9u * (uint32_t)(s + 1))
                     ^ (uint32_t)attack_cards[0].value
                     ^ ((uint32_t)attack_cards[0].suit << 8)
                     ^ (uint32_t)g->num_logs;
        Card opp[MAX_HAND_SIZE]; int oN;
        sample_opp_hand(u, opp_hand_size, seed, opp, &oN);
        RolloutResult r;
        rollout_round(attack_cards, attack_n, my_hand, my_hand_n,
                       opp, oN, g->power_suit, &r);
        int my_t = 0; for (int i = 0; i < r.my_n; i++) if (r.my_hand[i].suit == g->power_suit) my_t++;
        int opp_t = 0; for (int i = 0; i < r.opp_n; i++) if (r.opp_hand[i].suit == g->power_suit) opp_t++;
        double size_w = (r.pickup || !deck_active) ? 1.0 : 0.0;
        double pickup_bonus = r.pickup ? 3.0 : 0.0;
        double e = size_w * (double)(r.opp_n - r.my_n)
                 + 1.5 * (double)(my_t - opp_t)
                 + pickup_bonus;
        total += e;
        valid++;
    }
    return valid > 0 ? total / (double)valid : 0.0;
}

// ---------- choose --------------------------------------------------

static int mc_samples_for_pc(int num_players) {
    if (num_players <= 2) return 12;
    if (num_players <= 4) return 8;
    if (num_players <= 6) return 5;
    return 4;
}

int robusta_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx) {
    (void)ctx;
    if (moves->n == 0) return -1;
    // Robusta uses handwritten as the deterministic rollout policy.
    return robusta_mc_choose(g, bot_idx, moves, handwritten_strategy_choose);
}

// Generic MC move selector — Monte Carlo over fictional consistent states,
// scored by simulating to game end with `rollout_fn` for every seat.
int robusta_mc_choose(const Game *g, int bot_idx, const LegalMoves *moves,
                       StrategyFn rollout_fn) {
    if (moves->n == 0) return -1;
    if (moves->n == 1) return 0;

    int n_non_pickup = 0, last_non_pickup = -1;
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type != MOVE_PICKUP) { n_non_pickup++; last_non_pickup = i; }
    }
    if (n_non_pickup == 0) return 0;
    if (n_non_pickup == 1) return last_non_pickup;

    UnseenPool U; build_unseen_pool(g, bot_idx, &U);

    uint32_t base_seed = (uint32_t)g->num_logs * 0x9E3779B1u
                       ^ (uint32_t)g->discard_pile_length
                       ^ ((uint32_t)g->deck_count << 7);
    int n_samples = mc_samples_for_pc(g->num_players);

    int best = -1;
    double best_score = 1e30;
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type == MOVE_PICKUP) continue;
        double s = mc_eval_move(g, bot_idx, &moves->moves[i], &U,
                                 n_samples, base_seed + (uint32_t)i, rollout_fn);
        if (s < best_score) { best_score = s; best = i; }
    }
    return best >= 0 ? best : 0;
}

// ---------- multi-player (PC 3..8) -----------------------------------
//
// Winning at PC=3, 5, 8 in v1 baseline. Keep this body untouched while we
// iterate on PC=2 separately.
static int robusta_choose_multi(const Game *g, int bot_idx, const LegalMoves *moves) {
    const Player *bot = &g->players[bot_idx];
    UnseenPool U; build_unseen_pool(g, bot_idx, &U);

    int in_count = 0;
    for (int i = 0; i < g->num_players; i++) {
        if (g->players[i].status == PLAYER_STATUS_IN) in_count++;
    }
    bool is_1v1 = (in_count == 2);
    bool deck_active = (g->deck_count > 0 || g->has_flipped);

    // ---- attack moves: trump conservation gate --------------------------
    int n_attack = 0, n_non_trump = 0, n_trump = 0;
    for (int i = 0; i < moves->n; i++) {
        if (moves->moves[i].type == MOVE_ATTACK) {
            n_attack++;
            if (move_is_attack_all_non_trump(&moves->moves[i], g->power_suit)) n_non_trump++;
            else if (move_is_attack_any_trump(&moves->moves[i], g->power_suit)) n_trump++;
        }
    }

    int candidate_idx[MAX_LEGAL_MOVES];
    int cn = 0;
    if (n_attack > 0) {
        if (n_non_trump > 0) {
            for (int i = 0; i < moves->n; i++) {
                if (move_is_attack_all_non_trump(&moves->moves[i], g->power_suit)) candidate_idx[cn++] = i;
            }
        } else if (n_trump > 0) {
            // Without knowing opp trumps, gate by trump-attack probability.
            // (Espresso would peek at opp hand to allow when opp clearly has
            // higher trumps; we can't, so we just use the prob curve.)
            if (game_random() < get_trump_attack_probability(g)) {
                for (int i = 0; i < moves->n; i++) {
                    if (move_is_attack_any_trump(&moves->moves[i], g->power_suit)) candidate_idx[cn++] = i;
                }
            } else {
                // Prefer good if available; else fall through to the cascade.
                for (int i = 0; i < moves->n; i++) {
                    if (moves->moves[i].type == MOVE_GOOD) return i;
                }
            }
        }
    }

    // ---- candidate-attack evaluation ------------------------------------
    if (cn > 0) {
        int best = candidate_idx[0];
        double best_eval = -1e30;
        int best_count = -1;
        int best_sum = INT32_MAX;
        bool first = true;

        // Defender info for leader-block bonus.
        int min_opp_hand = 99;
        for (int i = 0; i < g->num_players; i++) {
            if (i == bot_idx || g->players[i].status != PLAYER_STATUS_IN) continue;
            if (g->players[i].hand_count < min_opp_hand) min_opp_hand = g->players[i].hand_count;
        }
        bool defender_is_leader = (g->defender != bot_idx
            && g->players[g->defender].status == PLAYER_STATUS_IN
            && g->players[g->defender].hand_count == min_opp_hand);

        // Expected opp trumps in the defender's hand.
        double exp_opp_trumps = 0.0;
        if (U.n > 0) {
            exp_opp_trumps = (double)U.trumps * (double)defender_hand_size(g, bot_idx) / (double)U.n;
        }

        int defender_hand = defender_hand_size(g, bot_idx);
        int n_samples = is_1v1 ? 20 : 0;

        for (int ci = 0; ci < cn; ci++) {
            const LegalMove *m = &moves->moves[candidate_idx[ci]];
            int v = m->cards[0].value;

            int my_trumps_after = 0;
            for (int i = 0; i < bot->hand_count; i++) {
                bool used = false;
                for (int j = 0; j < m->n_cards; j++) if (card_eq(bot->hand[i], m->cards[j])) { used = true; break; }
                if (!used && bot->hand[i].suit == g->power_suit) my_trumps_after++;
            }

            double e;
            if (is_1v1) {
                // 1v1 dual signal:
                //   (a) directly from unseen pool: p_pickup × n_cards is the
                //       expected gain (opp's hand grows by n on pickup).
                //   (b) averaged rollout over 20 sampled opp hands — catches
                //       follow-up pile-on dynamics inside the round.
                double p_pickup = prob_no_cover(&U, g, bot_idx, m->cards[0]);
                double direct = 4.0 * p_pickup * (double)m->n_cards;
                double rollout = rollout_eval(&U, g, bot_idx,
                                              m->cards, m->n_cards,
                                              bot->hand, bot->hand_count,
                                              defender_hand, n_samples);
                // Trump retention: heavier penalty for spending trumps when
                // opp probably also has trumps (gives them a trumped attack).
                double exp_opp_t_1v1 = 0.0;
                if (U.n > 0) exp_opp_t_1v1 = (double)U.trumps * (double)defender_hand / (double)U.n;
                double trump_term = 1.5 * ((double)my_trumps_after - exp_opp_t_1v1);
                e = direct + rollout + trump_term;
            } else {
                // Multi-player: heuristic on public stats.
                double p_pickup = prob_no_cover(&U, g, bot_idx, m->cards[0]);
                double po       = expected_pile_on(&U, g, bot_idx, v);
                double trump_w  = 1.2;
                double size_w   = (deck_active ? 0.3 : 1.0);

                double pickup_bonus = p_pickup * (1.5 + 0.5 * m->n_cards);
                if (defender_is_leader) pickup_bonus += p_pickup * 1.5;
                double trump_term = trump_w * ((double)my_trumps_after - exp_opp_trumps);
                double size_term  = size_w * 0.5 * (double)m->n_cards;
                double pile_pen   = 0.7 * po;
                e = pickup_bonus + size_term + trump_term - pile_pen;
            }

            int cnt = m->n_cards;
            int sum = 0;
            for (int i = 0; i < cnt; i++) sum += card_score(m->cards[i], g->power_suit);

            bool take = first
                || e > best_eval
                || (e == best_eval && cnt > best_count)
                || (e == best_eval && cnt == best_count && sum < best_sum);
            if (take) {
                best = candidate_idx[ci];
                best_eval = e;
                best_count = cnt;
                best_sum = sum;
                first = false;
            }
        }
        return best;
    }

    // ---- pass moves -----------------------------------------------------
    int pass_idx[MAX_LEGAL_MOVES]; int pn = 0;
    for (int i = 0; i < moves->n; i++) if (moves->moves[i].type == MOVE_PASS) pass_idx[pn++] = i;
    if (pn > 0) {
        // 1v1: passing forwards the attack to me-as-defender, which is usually
        // bad. Skip to good/cover/etc. if anything else is available.
        if (is_1v1) {
            bool has_other = false;
            for (int i = 0; i < moves->n; i++) {
                int t = moves->moves[i].type;
                if (t != MOVE_PASS && t != MOVE_PICKUP) { has_other = true; break; }
            }
            if (!has_other) return pass_idx[0];
        } else {
            // Multi-player: score passes by expected ability of next defender
            // to be overwhelmed. Quick heuristic — prefer the pass that keeps
            // the most cards in my hand and avoids passing trumps.
            int best = pass_idx[0];
            double best_eval = -1e30; bool first = true;
            for (int pi = 0; pi < pn; pi++) {
                const LegalMove *m = &moves->moves[pass_idx[pi]];
                int trumps_passed = 0, sum_v = 0;
                for (int i = 0; i < m->n_cards; i++) {
                    if (m->cards[i].suit == g->power_suit) trumps_passed++;
                    sum_v += m->cards[i].value;
                }
                double e = -2.0 * trumps_passed - 0.05 * sum_v + 0.2 * m->n_cards;
                if (first || e > best_eval) { best_eval = e; best = pass_idx[pi]; first = false; }
            }
            return best;
        }
    }

    // ---- cover moves ----------------------------------------------------
    // 1v1 uses handwritten's product-of-scores (strong trump penalty,
    // crucial in 1v1 endgame). Multi-player uses a looser per-card weight
    // — empirically the product is too restrictive in multi-player.
    int cover_idx[MAX_LEGAL_MOVES]; int cnv = 0;
    for (int i = 0; i < moves->n; i++) if (moves->moves[i].type == MOVE_COVER) cover_idx[cnv++] = i;
    if (cnv > 0) {
        int uncovered = 0;
        for (int i = 0; i < g->num_battles; i++) if (!!card_is_none(g->table_battles[i].defense)) uncovered++;
        int full_idx[MAX_LEGAL_MOVES]; int fn = 0;
        for (int i = 0; i < cnv; i++) {
            if (moves->moves[cover_idx[i]].n_cards == uncovered) full_idx[fn++] = cover_idx[i];
        }
        if (fn > 0) {
            if (is_1v1) {
                int best = full_idx[0];
                double best_score = 1e30;
                for (int fi = 0; fi < fn; fi++) {
                    const LegalMove *m = &moves->moves[full_idx[fi]];
                    double prod = 1.0;
                    for (int i = 0; i < m->n_cards; i++) {
                        prod *= (double)card_score(m->cards[i], g->power_suit);
                    }
                    if (prod < best_score) { best_score = prod; best = full_idx[fi]; }
                }
                return best;
            }
            // Multi-player: v4 formula. Trumps weighted moderately; sum_score
            // breaks ties; pile-on penalty for fresh values.
            bool table_v[16] = { false };
            for (int i = 0; i < g->num_battles; i++) {
                table_v[g->table_battles[i].attack.value] = true;
                if (!card_is_none(g->table_battles[i].defense)) table_v[g->table_battles[i].defense.value] = true;
            }
            int best = full_idx[0];
            double best_eval = -1e30; bool first = true;
            int best_max = INT32_MAX, best_sum = INT32_MAX;
            for (int fi = 0; fi < fn; fi++) {
                const LegalMove *m = &moves->moves[full_idx[fi]];
                int trumps_used = 0, sum_score = 0, max_score = 0;
                for (int i = 0; i < m->n_cards; i++) {
                    int sc = card_score(m->cards[i], g->power_suit);
                    sum_score += sc;
                    if (sc > max_score) max_score = sc;
                    if (m->cards[i].suit == g->power_suit) trumps_used++;
                }
                double po = 0.0;
                for (int i = 0; i < m->n_cards; i++) {
                    Card c = m->cards[i];
                    if (!table_v[c.value]) po += expected_pile_on(&U, g, bot_idx, c.value);
                }
                double e = -1.0 * trumps_used - 0.002 * sum_score - 0.4 * po;
                bool take = first
                    || e > best_eval
                    || (e == best_eval && max_score < best_max)
                    || (e == best_eval && max_score == best_max && sum_score < best_sum);
                if (take) {
                    best = full_idx[fi];
                    best_eval = e;
                    best_max = max_score;
                    best_sum = sum_score;
                    first = false;
                }
            }
            return best;
        }
    }

    // ---- good ----------------------------------------------------------
    for (int i = 0; i < moves->n; i++) if (moves->moves[i].type == MOVE_GOOD) return i;

    // ---- done attacks (sort by count desc, score asc) ------------------
    int done_idx[MAX_LEGAL_MOVES]; int dnv = 0;
    for (int i = 0; i < moves->n; i++) if (moves->moves[i].type == MOVE_ATTACK) done_idx[dnv++] = i;
    if (dnv > 0) {
        int best = done_idx[0];
        const LegalMove *bm = &moves->moves[best];
        int best_count = bm->n_cards;
        int best_sum = 0;
        for (int i = 0; i < bm->n_cards; i++) best_sum += card_score(bm->cards[i], g->power_suit);
        for (int i = 1; i < dnv; i++) {
            const LegalMove *m = &moves->moves[done_idx[i]];
            int sm = 0;
            for (int j = 0; j < m->n_cards; j++) sm += card_score(m->cards[j], g->power_suit);
            int cnt = m->n_cards;
            bool take = (cnt > best_count) || (cnt == best_count && sm < best_sum);
            if (take) { best = done_idx[i]; best_count = cnt; best_sum = sm; }
        }
        return best;
    }

    // ---- pickup --------------------------------------------------------
    for (int i = 0; i < moves->n; i++) if (moves->moves[i].type == MOVE_PICKUP) return i;

    // ---- random fallback (should never reach here) ---------------------
    int idx = (int)(game_random() * moves->n);
    if (idx < 0) idx = 0;
    if (idx >= moves->n) idx = moves->n - 1;
    return idx;
}

// ---------- PC=7 ------------------------------------------------------
//
// The default multi-player heuristic regresses badly at 7p because the
// pile-on penalty (po) grows linearly with non-defender count and
// dominates the eval. Until we have a properly-scaled formula, just fall
// back to handwritten at this seat count.
static int robusta_choose_7p_real(const Game *g, int bot_idx, const LegalMoves *moves) {
    return handwritten_strategy_choose(g, bot_idx, moves, NULL);
}

