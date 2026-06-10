// Blackpowder — belief-constrained determinized Monte Carlo.
//
// Same legitimacy contract as robusta (public info only: own hand, table,
// logs, hand counts, deck count), but three upgrades aimed at the places
// where robusta/gunpowder stall:
//
// 1. CARD MEMORY, rebuilt from game.logs on every call (so it needs no
//    server-side state):
//      - pinned cards from pickups (robusta's tracking, kept);
//      - the flipped trump's holder: the draw event that consumed the last
//        deck card contains the publicly visible flipped trump, so whoever
//        drew it holds it until they play it;
//      - void constraints: a defender who picked up while exactly one attack
//        card was uncovered held nothing that covers it. Until that player
//        next draws, every unknown card in their hand obeys the constraint
//        (their unknown set only shrinks between draws).
// 2. COMMON RANDOM NUMBERS: every candidate move is scored on the SAME
//    sampled worlds with the SAME rollout RNG stream, so move comparison is
//    a paired test instead of independent noisy estimates. This is what lets
//    MC keep working at 5-8 players where per-move sampling noise previously
//    drowned the signal.
// 3. EXACT ENDGAME: once 2 players remain and the deck is empty, the unseen
//    pool IS the opponent's remaining hand — a pure public deduction (the
//    same information espresso reads illegally). Alpha-beta over the real
//    engine rules then plays the endgame perfectly; if a forced win exists
//    we take it, otherwise we fall back to MC against the (imperfect) real
//    opponent models.
//
// The game RNG state is saved on entry and restored on exit, so deliberation
// never perturbs the outer game's random stream.

#include "blackpowder_strategy.h"
#include "strategy.h"
#include "card.h"
#include "game.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

// ---------- small utils ------------------------------------------------

static inline int bp_card_score(Card c, int power) {
    return c.value + (c.suit == power ? 1000 : 0);
}

static bool bp_set_contains(const Card *arr, int n, Card c) {
    for (int i = 0; i < n; i++) if (card_eq(arr[i], c)) return true;
    return false;
}

static uint32_t bp_xorshift(uint32_t s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s ? s : 0xB1A570u;
}

static uint32_t bp_mix(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u);
    h ^= h >> 16; h *= 0x85EBCA77u; h ^= h >> 13;
    return h ? h : 1;
}

// Ablation switches (read once): BP_NO_SOLVE / BP_NO_VOIDS / BP_NO_FLIP
// disable the endgame solver, the void constraints, and the flipped-trump
// pin respectively. For benchmarking component contributions.
static int bp_flag(const char *name) {
    const char *v = getenv(name);
    return v && v[0] && v[0] != '0';
}
static _Thread_local int bp_flags_loaded = 0;
static _Thread_local int bp_no_solve = 0, bp_no_voids = 0, bp_no_flip = 0;
static _Thread_local int bp_verify = 0;

static int bp_in_count(const Game *g) {
    int n = 0;
    for (int i = 0; i < g->num_players; i++)
        if (g->players[i].status == PLAYER_STATUS_IN) n++;
    return n;
}

static bool bp_apply(Game *g, int p_idx, const LegalMove *m) {
    switch (m->type) {
        case MOVE_ATTACK: return handle_attack(g, p_idx, m->cards, m->n_cards);
        case MOVE_COVER:  return handle_cover (g, p_idx, m->cards, m->attack_cards, m->n_cards);
        case MOVE_PASS:   return handle_pass  (g, p_idx, m->cards, m->n_cards);
        case MOVE_PICKUP: return handle_pickup(g, p_idx);
        case MOVE_GOOD:   return handle_good  (g, p_idx);
        default:          return false;
    }
}

// ---------- belief state ------------------------------------------------

#define BP_MAX_VOIDS 6

typedef struct {
    Card pool[80];                  // unseen pool (deck ∪ opp unknowns)
    int  n;
    Card pinned[MAX_PLAYERS][MAX_HAND_SIZE];  // publicly located in p's hand
    int  pinned_n[MAX_PLAYERS];
    // Active void constraints per opponent: each entry is an attack card
    // that player demonstrably could not cover at pickup time. Valid for
    // their unknown cards until their next draw (cleared on LOG_DRAW).
    Card voids[MAX_PLAYERS][BP_MAX_VOIDS];
    int  void_n[MAX_PLAYERS];
} Belief;

static bool bp_card_forbidden(const Belief *B, const Game *g, int p, Card c) {
    for (int k = 0; k < B->void_n[p]; k++) {
        if (can_cover(B->voids[p][k], c, g->power_suit)) return true;
    }
    return false;
}

static void bp_pinned_remove(Belief *B, int p, Card c) {
    for (int q = 0; q < B->pinned_n[p]; q++) {
        if (card_eq(B->pinned[p][q], c)) {
            B->pinned[p][q] = B->pinned[p][B->pinned_n[p] - 1];
            B->pinned_n[p]--;
            return;
        }
    }
}

static void bp_pinned_add(Belief *B, int p, Card c) {
    if (B->pinned_n[p] >= MAX_HAND_SIZE) return;
    if (bp_set_contains(B->pinned[p], B->pinned_n[p], c)) return;
    B->pinned[p][B->pinned_n[p]++] = c;
}

// Chronological scan over logs: pinned cards, flipped-trump holder, and
// void constraints, all in one pass.
static void bp_build_belief(const Game *g, int bot_idx, Belief *B) {
    memset(B, 0, sizeof(*B));

    // The flipped trump is the last card ever drawn. If the deck (incl.
    // flipped) is exhausted, the final LOG_DRAW event's player took it.
    int flip_log_idx = -1;
    if (g->deck_count == 0 && !g->has_flipped && !bp_no_flip) {
        for (int i = 0; i < g->num_logs; i++) {
            if (g->logs[i].log_type == LOG_DRAW) flip_log_idx = i;
        }
    }

    // Replayed table state. We rebuild the table from ATTACK/COVER/PASS
    // events rather than trusting LOG_PICKUP/LOG_DISCARD card lists: those
    // two can exceed MAX_LOG_PAIRS (a 10-battle pickup is 20+ cards) and get
    // silently truncated, whereas per-move events carry at most 8 cards.
    Card tbl[80];               // every card currently on the table
    int  tbl_n = 0;
    Card unc[MAX_BATTLES * 2];  // uncovered attack cards only
    int  unc_n = 0;
    Card discards[160];         // accumulated discard pile (complete)
    int  disc_n = 0;

    for (int i = 0; i < g->num_logs; i++) {
        const GameLog *L = &g->logs[i];
        int p = L->player_idx;
        switch (L->log_type) {
            case LOG_ATTACK:
            case LOG_PASS:
                for (int k = 0; k < L->num_pairs; k++) {
                    Card c = L->pairs[k].primary;
                    if (unc_n < (int)(sizeof(unc) / sizeof(unc[0]))) unc[unc_n++] = c;
                    if (tbl_n < (int)(sizeof(tbl) / sizeof(tbl[0]))) tbl[tbl_n++] = c;
                    if (p >= 0 && p != bot_idx) bp_pinned_remove(B, p, c);
                }
                break;
            case LOG_COVER:
                for (int k = 0; k < L->num_pairs; k++) {
                    // primary = cover card (leaves p's hand), target = attack.
                    Card c = L->pairs[k].primary;
                    if (tbl_n < (int)(sizeof(tbl) / sizeof(tbl[0]))) tbl[tbl_n++] = c;
                    if (p >= 0 && p != bot_idx) bp_pinned_remove(B, p, c);
                    if (L->pairs[k].has_target) {
                        for (int q = 0; q < unc_n; q++) {
                            if (card_eq(unc[q], L->pairs[k].target)) {
                                unc[q] = unc[--unc_n];
                                break;
                            }
                        }
                    }
                }
                break;
            case LOG_PICKUP:
                if (p >= 0 && p != bot_idx) {
                    // Exactly one uncovered attack => the defender held no
                    // cover for it. (Multi-attack pickups only prove "no
                    // joint full cover" — too weak to use as a hard
                    // constraint, so we skip those.)
                    if (unc_n == 1 && B->void_n[p] < BP_MAX_VOIDS) {
                        B->voids[p][B->void_n[p]++] = unc[0];
                    }
                    // The whole replayed table enters p's hand.
                    for (int k = 0; k < tbl_n; k++) bp_pinned_add(B, p, tbl[k]);
                }
                tbl_n = 0;
                unc_n = 0;
                break;
            case LOG_DISCARD:
                for (int k = 0; k < tbl_n && disc_n < 160; k++) discards[disc_n++] = tbl[k];
                tbl_n = 0;
                unc_n = 0;
                break;
            case LOG_DRAW:
                if (p >= 0 && p != bot_idx) {
                    B->void_n[p] = 0;  // new unknown cards: constraints expire
                    if (i == flip_log_idx) bp_pinned_add(B, p, g->flipped);
                }
                break;
            default:
                break;
        }
    }

    // Players that left the game hold nothing.
    for (int p = 0; p < g->num_players; p++) {
        if (p == bot_idx) { B->pinned_n[p] = 0; B->void_n[p] = 0; continue; }
        if (g->players[p].status != PLAYER_STATUS_IN) {
            B->pinned_n[p] = 0;
            B->void_n[p] = 0;
        }
        if (B->pinned_n[p] > g->players[p].hand_count) {
            B->pinned_n[p] = g->players[p].hand_count;  // defensive clamp
        }
    }

    // Unseen pool = full deck minus everything publicly located:
    // my hand, table, discards, the face-up flipped card, pinned cards.
    Card known[160];
    int kn = 0;
    const Player *bot = &g->players[bot_idx];
    for (int j = 0; j < bot->hand_count; j++) known[kn++] = bot->hand[j];
    for (int i = 0; i < g->num_battles; i++) {
        known[kn++] = g->table_battles[i].attack;
        if (g->table_battles[i].has_defense) known[kn++] = g->table_battles[i].defense;
    }
    if (g->has_flipped) known[kn++] = g->flipped;
    for (int i = 0; i < disc_n && kn < 160; i++) known[kn++] = discards[i];
    for (int p = 0; p < g->num_players; p++) {
        for (int j = 0; j < B->pinned_n[p] && kn < 160; j++) {
            known[kn++] = B->pinned[p][j];
        }
    }

    int start_v = min_value_for(g->num_players);
    B->n = 0;
    for (int suit = 0; suit < 4; suit++) {
        for (int v = start_v; v <= ACE_VALUE; v++) {
            Card c = { (int8_t)suit, (int8_t)v };
            if (!bp_set_contains(known, kn, c)) B->pool[B->n++] = c;
        }
    }

    // Sanity: if a constraint leaves fewer allowed pool cards than the
    // player's unknown count, the opponent didn't play "cover-if-you-can"
    // (e.g. the random bot). Drop their constraints rather than skewing
    // every sampled world.
    for (int p = 0; p < g->num_players; p++) {
        if (B->void_n[p] == 0) continue;
        int unknown = g->players[p].hand_count - B->pinned_n[p];
        if (unknown <= 0) continue;
        int allowed = 0;
        for (int i = 0; i < B->n; i++) {
            if (!bp_card_forbidden(B, g, p, B->pool[i])) allowed++;
        }
        if (allowed < unknown) B->void_n[p] = 0;
    }
}

// ---------- world sampling ------------------------------------------------

// Sample one full consistent world: pinned cards go to their holders, the
// rest of the unseen pool shuffles into the deck and the unknown hand slots,
// then a repair pass swaps constraint-violating cards into the deck.
static void bp_sample_world(Game *g_out, const Game *g_in, int my_idx,
                            const Belief *B, uint32_t seed, bool apply_voids) {
    game_clone(g_out, g_in);

    for (int i = 0; i < g_in->num_players; i++) {
        if (i == my_idx) continue;
        for (int k = 0; k < B->pinned_n[i]; k++) {
            g_out->players[i].hand[k] = B->pinned[i][k];
        }
    }

    Card hidden[80];
    int hn = B->n;
    for (int i = 0; i < hn; i++) hidden[i] = B->pool[i];
    if (hn == 0) return;

    uint32_t s = seed ? seed : 0xCAFEu;
    for (int i = hn - 1; i > 0; i--) {
        s = bp_xorshift(s);
        int j = (int)(s % (uint32_t)(i + 1));
        Card sw = hidden[i]; hidden[i] = hidden[j]; hidden[j] = sw;
    }

    // Deal: deck first, then each opponent's unknown slots.
    int k = 0;
    int deck_n = g_in->deck_count;
    for (int i = 0; i < deck_n && k < hn; i++) g_out->deck[i] = hidden[k++];

    typedef struct { int player, slot, hidden_idx; } SlotRef;
    SlotRef slots[80];
    int ns = 0;
    for (int i = 0; i < g_in->num_players; i++) {
        if (i == my_idx) continue;
        int need = g_in->players[i].hand_count - B->pinned_n[i];
        for (int j = 0; j < need && k < hn; j++) {
            g_out->players[i].hand[B->pinned_n[i] + j] = hidden[k];
            slots[ns].player = i;
            slots[ns].slot = B->pinned_n[i] + j;
            slots[ns].hidden_idx = k;
            ns++;
            k++;
        }
    }

    // Repair: any dealt card that violates its holder's void constraints is
    // swapped with a compatible card from the deck portion. Deck cards are
    // unconstrained, so this preserves consistency. If the deck has no
    // compatible card we accept the violation (graceful degradation, e.g.
    // against bots that pick up while holding covers).
    if (!apply_voids) return;
    for (int si = 0; si < ns; si++) {
        int p = slots[si].player;
        if (B->void_n[p] == 0) continue;
        Card c = g_out->players[p].hand[slots[si].slot];
        if (!bp_card_forbidden(B, g_in, p, c)) continue;
        for (int d = 0; d < deck_n; d++) {
            if (!bp_card_forbidden(B, g_in, p, g_out->deck[d])) {
                Card sw = g_out->deck[d];
                g_out->deck[d] = c;
                g_out->players[p].hand[slots[si].slot] = sw;
                break;
            }
        }
    }
}

// ---------- simulation ---------------------------------------------------

// Stage-aware rollout policy (gunpowder's rule): handwritten while the deck
// is alive or the game is heads-up, espresso for multi-player endgames.
static StrategyFn bp_rollout_for(const Game *g) {
    bool deck_active = (g->deck_count > 0 || g->has_flipped);
    if (deck_active || bp_in_count(g) == 2) return handwritten_strategy_choose;
    return espresso_strategy_choose;
}

// Roll a sampled world to completion; returns my finish position (1..N),
// or 0 if the simulation didn't terminate.
static int bp_simulate(Game *g, int my_idx, int max_turns) {
    int turns = 0;
    while (game_done(g) < 0 && turns++ < max_turns) {
        bool acted = false;
        for (int pi = 0; pi < g->num_players; pi++) {
            if (!should_bot_act(g, pi)) continue;
            LegalMoves moves;
            calculate_legal_moves_lite(g, pi, &moves);
            if (moves.n == 0) continue;
            StrategyFn fn = bp_rollout_for(g);
            int idx = fn(g, pi, &moves, NULL);
            if (idx < 0 || idx >= moves.n) continue;
            if (bp_apply(g, pi, &moves.moves[idx])) { acted = true; break; }
        }
        if (!acted) break;
    }
    if (game_done(g) < 0) return 0;
    for (int i = 0; i < g->num_eliminated; i++) {
        if (g->elimination_order[i] == my_idx) return i + 1;
    }
    return g->num_players;
}

// ---------- exact endgame solver ------------------------------------------

#define BP_SOLVE_MAX_DEPTH   48
#define BP_SOLVE_MAX_MOVES   96
#define BP_SOLVE_BUDGET      200000L
#define BP_SOLVE_MAX_CARDS   20

typedef struct {
    long budget;
    bool aborted;
    int  me;
    // Depth-indexed scratch (Game + LegalMoves per frame are far too big for
    // the stack at depth 48). Lazily allocated, reused across calls.
    Game       *child;   // [BP_SOLVE_MAX_DEPTH]
    LegalMoves *mv;      // [BP_SOLVE_MAX_DEPTH]
} Solver;

static _Thread_local Game       *bp_solver_child = NULL;
static _Thread_local LegalMoves *bp_solver_mv = NULL;

// Copy only the live prefix of the Game (everything before the log tail).
// The solver zeroes num_logs at the root, so log copying stays tiny.
static void bp_lite_clone(Game *dst, const Game *src) {
    size_t base = offsetof(Game, logs);
    memcpy(dst, src, base + (size_t)src->num_logs * sizeof(GameLog));
}

// Returns a value in [-1000, 1000] from `me`'s perspective: positive = me
// escaping (win), negative = me as durak. Magnitude prefers faster wins and
// slower losses.
static int bp_solve(Solver *S, const Game *g, int alpha, int beta, int depth) {
    int loser = game_done(g);
    if (loser >= 0) return (loser == S->me) ? -(1000 - depth) : (1000 - depth);
    if (bp_in_count(g) == 0) return 0;   // simultaneous out: draw
    if (depth >= BP_SOLVE_MAX_DEPTH) { S->aborted = true; return 0; }
    if (--S->budget <= 0) { S->aborted = true; return 0; }

    // Actor: defender first when they have uncovered attacks; otherwise the
    // first eligible attacker. (The real loop randomizes simultaneous
    // eligibility; defender-priority is the standard sequential abstraction.)
    int actor = -1;
    if (should_bot_act(g, g->defender)) actor = g->defender;
    else {
        for (int i = 0; i < g->num_players; i++) {
            if (should_bot_act(g, i)) { actor = i; break; }
        }
    }
    if (actor < 0) return 0;

    LegalMoves *mv = &S->mv[depth];
    calculate_legal_moves(g, actor, mv);
    if (mv->n == 0) return 0;
    if (mv->n > BP_SOLVE_MAX_MOVES) { S->aborted = true; return 0; }

    bool maximizing = (actor == S->me);
    int best = maximizing ? -2000 : 2000;
    for (int i = 0; i < mv->n; i++) {
        Game *child = &S->child[depth];
        bp_lite_clone(child, g);
        if (!bp_apply(child, actor, &mv->moves[i])) continue;
        int v = bp_solve(S, child, alpha, beta, depth + 1);
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
    if (best == -2000 || best == 2000) return 0;  // no move applied
    return best;
}

// Try to solve the real position exactly. Possible when 2 players remain,
// the deck is gone, and the unseen pool exactly fills the opponent's unknown
// slots (always true in that situation — pure public deduction). Returns the
// winning move index, or -1 if no forced win / not applicable / aborted.
static int bp_try_endgame_solve(const Game *g, int bot_idx,
                                const LegalMoves *moves, const Belief *B) {
    if (g->deck_count > 0 || g->has_flipped) return -1;
    if (bp_in_count(g) != 2) return -1;
    if (g->players[bot_idx].status != PLAYER_STATUS_IN) return -1;

    int opp = -1;
    for (int i = 0; i < g->num_players; i++) {
        if (i != bot_idx && g->players[i].status == PLAYER_STATUS_IN) opp = i;
    }
    if (opp < 0) return -1;

    int unknown = g->players[opp].hand_count - B->pinned_n[opp];
    if (unknown < 0 || unknown != B->n) return -1;  // deduction failed; bail

    int total = g->players[bot_idx].hand_count + g->players[opp].hand_count;
    if (total > BP_SOLVE_MAX_CARDS) return -1;

    if (!bp_solver_child) {
        bp_solver_child = malloc(sizeof(Game) * BP_SOLVE_MAX_DEPTH);
        bp_solver_mv    = malloc(sizeof(LegalMoves) * BP_SOLVE_MAX_DEPTH);
        if (!bp_solver_child || !bp_solver_mv) return -1;
    }

    Game root;
    game_clone(&root, g);
    root.num_logs = 0;  // solver never reads history; keeps clones tiny
    for (int k = 0; k < B->pinned_n[opp]; k++) {
        root.players[opp].hand[k] = B->pinned[opp][k];
    }
    for (int k = 0; k < B->n; k++) {
        root.players[opp].hand[B->pinned_n[opp] + k] = B->pool[k];
    }

    Solver S;
    S.budget  = BP_SOLVE_BUDGET;
    S.aborted = false;
    S.me      = bot_idx;
    S.child   = bp_solver_child;
    S.mv      = bp_solver_mv;

    int best_idx = -1;
    int best_v = 0;   // only accept strictly winning lines
    int alpha = -2000;
    for (int i = 0; i < moves->n; i++) {
        Game child;
        bp_lite_clone(&child, &root);
        if (!bp_apply(&child, bot_idx, &moves->moves[i])) continue;
        int v = bp_solve(&S, &child, alpha, 2000, 1);
        if (S.aborted) return -1;
        if (v > best_v) { best_v = v; best_idx = i; }
        if (v > alpha) alpha = v;
    }
    return best_idx;
}

// ---------- candidate selection -------------------------------------------

#define BP_MAX_CANDS 26

typedef struct {
    int idx[BP_MAX_CANDS];
    int n;
} Candidates;

// Insert idx into a capacity-capped list ordered by ascending key.
static void bp_ranked_insert(int *idxs, double *keys, int *n, int cap,
                             int idx, double key) {
    int pos = *n;
    while (pos > 0 && keys[pos - 1] > key) pos--;
    if (pos >= cap) return;
    int last = (*n < cap) ? *n : cap - 1;
    for (int i = last; i > pos; i--) { idxs[i] = idxs[i - 1]; keys[i] = keys[i - 1]; }
    idxs[pos] = idx; keys[pos] = key;
    if (*n < cap) (*n)++;
}

static void bp_pick_candidates(const Game *g, const LegalMoves *moves,
                               Candidates *out) {
    int power = g->power_suit;

    int atk[12];  double atk_k[12];  int n_atk = 0;
    int cov[10];  double cov_k[10];  int n_cov = 0;
    int pas[3];   double pas_k[3];   int n_pas = 0;
    int good_idx = -1, pickup_idx = -1;

    for (int i = 0; i < moves->n; i++) {
        const LegalMove *m = &moves->moves[i];
        switch (m->type) {
            case MOVE_ATTACK: {
                // Prefer more cards, then cheaper. Key: -(count*10000) + sum.
                int sum = 0;
                for (int j = 0; j < m->n_cards; j++) sum += bp_card_score(m->cards[j], power);
                bp_ranked_insert(atk, atk_k, &n_atk, 12, i,
                                 -(double)m->n_cards * 10000.0 + (double)sum);
                break;
            }
            case MOVE_COVER: {
                // Cheaper covers first (product penalizes trumps hard).
                double prod = 1.0;
                for (int j = 0; j < m->n_cards; j++) prod *= (double)bp_card_score(m->cards[j], power);
                // Favor fuller covers among equals.
                bp_ranked_insert(cov, cov_k, &n_cov, 10, i,
                                 prod - (double)m->n_cards * 0.5);
                break;
            }
            case MOVE_PASS: {
                int sum = 0;
                for (int j = 0; j < m->n_cards; j++) sum += bp_card_score(m->cards[j], power);
                bp_ranked_insert(pas, pas_k, &n_pas, 3, i, (double)sum);
                break;
            }
            case MOVE_GOOD:   good_idx = i;   break;
            case MOVE_PICKUP: pickup_idx = i; break;
            default: break;
        }
    }

    out->n = 0;
    for (int i = 0; i < n_atk && out->n < BP_MAX_CANDS; i++) out->idx[out->n++] = atk[i];
    for (int i = 0; i < n_cov && out->n < BP_MAX_CANDS; i++) out->idx[out->n++] = cov[i];
    for (int i = 0; i < n_pas && out->n < BP_MAX_CANDS; i++) out->idx[out->n++] = pas[i];
    if (good_idx >= 0 && out->n < BP_MAX_CANDS)   out->idx[out->n++] = good_idx;
    if (pickup_idx >= 0 && out->n < BP_MAX_CANDS) out->idx[out->n++] = pickup_idx;
}

// ---------- main MC ---------------------------------------------------------

static void bp_params(int num_players, int *W1, int *W2) {
    if (num_players <= 2)      { *W1 = 12; *W2 = 20; }
    else if (num_players <= 4) { *W1 = 8;  *W2 = 16; }
    else if (num_players <= 6) { *W1 = 10; *W2 = 20; }
    else                       { *W1 = 8;  *W2 = 16; }
}

// BP_VERIFY=1: oracle self-check (test-only — reads real hands to validate
// the public-info belief, never to play). Prints any violation to stderr.
static void bp_verify_belief(const Game *g, int bot_idx, const Belief *B) {
    for (int p = 0; p < g->num_players; p++) {
        if (p == bot_idx) continue;
        const Player *pl = &g->players[p];
        for (int k = 0; k < B->pinned_n[p]; k++) {
            bool found = false;
            for (int j = 0; j < pl->hand_count; j++) {
                if (card_eq(pl->hand[j], B->pinned[p][k])) { found = true; break; }
            }
            if (!found) {
                fprintf(stderr, "BP_VERIFY: pinned card v%d s%d NOT in p%d hand (logs=%d)\n",
                        B->pinned[p][k].value, B->pinned[p][k].suit, p, g->num_logs);
            }
        }
        for (int j = 0; j < pl->hand_count; j++) {
            // Unknown portion = cards not pinned; every unknown must obey voids.
            if (bp_set_contains(B->pinned[p], B->pinned_n[p], pl->hand[j])) continue;
            if (B->void_n[p] > 0 && bp_card_forbidden(B, g, p, pl->hand[j])) {
                fprintf(stderr, "BP_VERIFY: void violated: p%d holds v%d s%d power=%d voids=[",
                        p, pl->hand[j].value, pl->hand[j].suit, g->power_suit);
                for (int k = 0; k < B->void_n[p]; k++) {
                    fprintf(stderr, "v%ds%d ", B->voids[p][k].value, B->voids[p][k].suit);
                }
                fprintf(stderr, "] pinned_n=%d hand_n=%d logs=%d\n",
                        B->pinned_n[p], pl->hand_count, g->num_logs);
            }
            // Every unknown card must be in the pool.
            if (!bp_set_contains(B->pool, B->n, pl->hand[j])) {
                fprintf(stderr, "BP_VERIFY: p%d unknown card v%d s%d missing from pool (logs=%d)\n",
                        p, pl->hand[j].value, pl->hand[j].suit, g->num_logs);
            }
        }
    }
}

int blackpowder_strategy_choose(const Game *g, int bot_idx,
                                const LegalMoves *moves, void *ctx) {
    (void)ctx;
    if (moves->n == 0) return -1;
    if (moves->n == 1) return 0;

    if (!bp_flags_loaded) {
        bp_no_solve  = bp_flag("BP_NO_SOLVE");
        bp_no_voids  = bp_flag("BP_NO_VOIDS");
        bp_no_flip   = bp_flag("BP_NO_FLIP");
        bp_verify    = bp_flag("BP_VERIFY");
        bp_flags_loaded = 1;
    }

    uint32_t saved_rng = game_rng_get();

    Belief B;
    bp_build_belief(g, bot_idx, &B);
    if (bp_no_voids) for (int p = 0; p < MAX_PLAYERS; p++) B.void_n[p] = 0;
    if (bp_verify) bp_verify_belief(g, bot_idx, &B);

    // Exact endgame: take a proven win when one exists.
    int solved = bp_no_solve ? -1 : bp_try_endgame_solve(g, bot_idx, moves, &B);
    if (solved >= 0) {
        game_rng_set(saved_rng);
        return solved;
    }

    Candidates C;
    bp_pick_candidates(g, moves, &C);
    if (C.n == 0) { game_rng_set(saved_rng); return 0; }
    if (C.n == 1) { game_rng_set(saved_rng); return C.idx[0]; }

    int W1, W2;
    bp_params(g->num_players, &W1, &W2);

    uint32_t base = bp_mix((uint32_t)g->num_logs * 2654435761u,
                           ((uint32_t)g->deck_count << 8)
                           ^ (uint32_t)g->discard_pile_length
                           ^ ((uint32_t)bot_idx << 20));

    double score[BP_MAX_CANDS] = {0};
    int    nsim [BP_MAX_CANDS] = {0};
    bool   alive[BP_MAX_CANDS];
    for (int i = 0; i < C.n; i++) alive[i] = true;

    static _Thread_local Game world, trial;

    // Stage 1: all candidates on W1 shared worlds.
    // Stage 2: survivors on W2 more shared worlds.
    for (int stage = 0; stage < 2; stage++) {
        int w_lo = (stage == 0) ? 0 : W1;
        int w_hi = (stage == 0) ? W1 : W1 + W2;
        for (int w = w_lo; w < w_hi; w++) {
            uint32_t wseed = bp_mix(base, (uint32_t)(w + 1) * 0x85EBCA77u);
            // Void constraints assume "cover if you can" pickups. Strategic
            // players (and the random bot) sometimes pick up while holding
            // covers, so 1 world in 4 ignores the voids — a belief mixture
            // that degrades gracefully when the assumption is wrong.
            bp_sample_world(&world, g, bot_idx, &B, wseed, (w & 3) != 3);
            uint32_t sim_rng = bp_mix(wseed, 0x51AB1E5u);
            for (int ci = 0; ci < C.n; ci++) {
                if (!alive[ci]) continue;
                game_clone(&trial, &world);
                game_rng_set(sim_rng);   // identical stream for every move
                if (!bp_apply(&trial, bot_idx, &moves->moves[C.idx[ci]])) {
                    // Move invalid in this world (can't happen for own-hand
                    // moves, but stay safe): count as worst.
                    score[ci] += (double)g->num_players;
                    nsim[ci]++;
                    continue;
                }
                int fp = bp_simulate(&trial, bot_idx, 600);
                if (fp == 0) fp = g->num_players;
                score[ci] += (double)fp;
                nsim[ci]++;
            }
        }
        if (stage == 0) {
            // Keep the best third (min 3) for stage 2.
            int keep = C.n / 3;
            if (keep < 3) keep = 3;
            if (keep >= C.n) continue;
            for (int dropped = C.n - keep; dropped > 0; dropped--) {
                int worst = -1;
                double worst_v = -1e30;
                for (int i = 0; i < C.n; i++) {
                    if (!alive[i]) continue;
                    double v = score[i] / (double)(nsim[i] ? nsim[i] : 1);
                    if (v > worst_v) { worst_v = v; worst = i; }
                }
                if (worst < 0) break;
                alive[worst] = false;
            }
        }
    }

    int best = -1;
    double best_v = 1e30;
    for (int i = 0; i < C.n; i++) {
        if (!alive[i] || nsim[i] == 0) continue;
        double v = score[i] / (double)nsim[i];
        if (v < best_v) { best_v = v; best = i; }
    }

    game_rng_set(saved_rng);
    return best >= 0 ? C.idx[best] : 0;
}
