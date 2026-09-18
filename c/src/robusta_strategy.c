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
#include "cordite_sim.h"   // shared MC scratch (reused so wasm memory is unchanged)
#include <string.h>
#include <stdint.h>

// The MC rollout puts a full Game (~67 KiB) and a LegalMoves (~232 KiB) in play
// per sample. Natively that lives on an 8 MB stack, but bots.wasm ships a 22 KiB
// shadow stack, so those buffers must not be stack locals (an on-stack LegalMoves
// alone traps under --stack-first) — and malloc'ing them would grow the module
// past octogen's footprint. Instead firecracker's rollout reuses the SHARED
// scratch octogen already reserves: the sampled world goes in world_scratch_game()
// (a short log-capped slot) and the rollout move list in rollout_moves_scratch().
// Only one MC family runs per decision, so the slots are ours for the duration.

// ---------- helpers ---------------------------------------------------

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
    for (int suit = 0; suit < 4; suit++) {
        for (int v = start_v; v <= ACE_VALUE; v++) {
            Card c = { (int8_t)suit, (int8_t)v };
            if (!set_contains(known, kn, c)) {
                u->pool[u->n++] = c;
            }
        }
    }
}

// The shuffle's PRNG for sample_consistent_state.
static uint32_t xorshift32(uint32_t s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s ? s : 1;
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
// Clone into a shared short world slot: the log-free prefix plus the LOG_DISCARD
// tail (the only log type the espresso/handwritten rollout policies read),
// capped at WORLD_LOG_CAP. Mirrors cordite's sampler so the shared world slot is
// respected on the wasm short-log build; native (WORLD_LOG_CAP==0) keeps every
// discard in a full-size slot. Same tradeoff cordite already makes.
static void robusta_clone_world(Game *dst, const Game *src) {
    memcpy(dst, src, offsetof(Game, logs));
    int nl = 0;
    for (int i = 0; i < src->num_logs; i++) {
        if (src->logs[i].log_type != LOG_DISCARD) continue;
        if (WORLD_LOG_CAP > 0 && nl >= WORLD_LOG_CAP) break;
        dst->logs[nl++] = src->logs[i];
    }
    dst->num_logs = nl;
    dst->log_cap  = WORLD_LOG_CAP;
    dst->log_virt = (int16_t)nl;
}

static bool sample_consistent_state(Game *g_out, const Game *g_in, int my_idx,
                                     const UnseenPool *u, uint32_t seed) {
    robusta_clone_world(g_out, g_in);

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
    LegalMoves *moves = rollout_moves_scratch();  // shared rollout buffer, no new memory
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
    Game *g = world_scratch_game();   // shared sampled-world slot, no new memory
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
