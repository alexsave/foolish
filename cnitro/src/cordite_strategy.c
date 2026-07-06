// Cordite — belief-constrained determinized Monte Carlo, v2.
//
// Blackpowder's contract (public info only: own hand, table, logs, hand
// counts, deck count; no LLM, no reading hidden state) with five upgrades:
//
// 1. COMPACT WORLDS: sampled worlds keep only the LOG_DISCARD entries of the
//    real log (the only log type any rollout policy reads — espresso's
//    discard memory). Trial clones in the hot loop shrink ~8x, buying more
//    sampled worlds for the same wall-clock.
// 2. EARLY ROLLOUT EXIT: a rollout returns the moment our seat is
//    eliminated — the finish position is already determined. At 5-8 players
//    this skips the long tail of other players' endgames.
// 3. EXACT LEAF ENDGAMES: when a rollout funnels to 2 players + empty deck
//    with few cards, alpha-beta resolves the durak exactly (full info is
//    legitimate inside a sampled world) instead of noisy policy play.
// 4. LOSS-AVOIDING ROOT SOLVER: blackpowder only TAKES forced wins. Cordite
//    solves every root move with a full window: take the fastest forced win;
//    otherwise exclude forced-loss moves from MC whenever a non-losing move
//    exists (MC vs the real, imperfect opponent decides among the safe set).
// 5. RANK FLOORS + PER-PLAYER TRUST: a single-card first attack by a
//    lowest-first opponent (handwritten family, incl. espresso at 3+ in)
//    reveals their lowest non-trump rank — sampled as a soft constraint in
//    half the worlds. Players that attack with trumps while the deck is
//    alive get their floors (1 strike) and voids (2 strikes) distrusted:
//    random and MC-style opponents don't obey either assumption.
//
// The game RNG is saved on entry and restored on exit; deliberation never
// perturbs the outer game.

#include "cordite_strategy.h"
#include "strategy.h"
#include "card.h"
#include "game.h"
#include "cordite_sim.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

// ---------- small utils ------------------------------------------------

static inline int cd_card_score(Card c, int power) {
    return c.value + (c.suit == power ? 1000 : 0);
}

static bool cd_set_contains(const Card *arr, int n, Card c) {
    for (int i = 0; i < n; i++) if (card_eq(arr[i], c)) return true;
    return false;
}

static uint32_t cd_xorshift(uint32_t s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s ? s : 0xB1A570u;
}

static uint32_t cd_mix(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u);
    h ^= h >> 16; h *= 0x85EBCA77u; h ^= h >> 13;
    return h ? h : 1;
}

// Ablation switches (read once): CD_NO_SOLVE / CD_NO_VOIDS / CD_NO_FLIP /
// CD_NO_FLOORS / CD_NO_LEAF / CD_NO_AVOID / CD_VERIFY, plus CD_W1/CD_W2
// world-count overrides for tuning.
static int cd_flag(const char *name) {
    const char *v = getenv(name);
    return v && v[0] && v[0] != '0';
}
static int cd_env_int(const char *name, int def) {
    const char *v = getenv(name);
    return (v && v[0]) ? atoi(v) : def;
}
static _Thread_local int cd_flags_loaded = 0;
// Force the CD_* knobs to be re-read on the next choose call. The wasm bridge
// calls this after rewriting its env table (one module instance serves both
// cordite and cordite_max, which differ only by CD_W*/CD_KEEP*).
void cordite_reload_flags(void);
void cordite_reload_flags(void) { cd_flags_loaded = 0; }
static _Thread_local int cd_no_solve = 0, cd_no_voids = 0, cd_no_flip = 0;
static _Thread_local int cd_no_floors = 0, cd_no_leaf = 0, cd_no_avoid = 0;
static _Thread_local int cd_no_earlyexit = 0;
static _Thread_local int cd_verify = 0;
static _Thread_local int cd_no_fastroll = 0;   // CD_NO_FASTROLL=1: struct rollout
static _Thread_local int cd_difftest = 0;      // CD_DIFFTEST=1: assert fast==slow
static _Thread_local int cd_w1_override = 0, cd_w2_override = 0;
static _Thread_local int cd_w3_override = -1;
static _Thread_local int cd_old_budget = 0;    // cordite_old variant: pre-2x worlds
static _Thread_local int cd_keep1 = 0, cd_keep2 = 0;  // CD_KEEP1/2: candidates kept past stage 0/1 (0=default n/3, 2)
static _Thread_local int cd_budget_mode = 0;   // CD_BUDGET: 0=arena, 1=prod (TS v2.4), 2=max
static _Thread_local int cd_race = 0;          // CD_RACE=1: stop sampling once the leader is separated
static _Thread_local int cd_race_c = 100;      // CD_RACE_C: separation threshold, percent (see cd loop)
static _Thread_local int cd_rollout_policy = 0;       // CD_ROLLOUT: 0=default, 1=espresso, 2=handwritten (struct path)
// Bitboard endgame-solver node budgets (per shared pass). The bitboard solver
// (transposition table + O(1) clone) resolves far more per node than the
// struct solver, so it needs a much smaller node budget to do equivalent work
// in less wall-clock. Tunable via CD_BB_WIN / CD_BB_AVOID for sweeps.
static _Thread_local int cd_bb_win_budget = 20000;
static _Thread_local int cd_bb_avoid_budget = 15000;

static int cd_in_count(const Game *g) {
    int n = 0;
    for (int i = 0; i < g->num_players; i++)
        if (g->players[i].status == PLAYER_STATUS_IN) n++;
    return n;
}

static bool cd_apply(Game *g, int p_idx, const LegalMove *m) {
    switch (m->type) {
        case MOVE_ATTACK: return handle_attack(g, p_idx, m->cards, m->n_cards);
        case MOVE_COVER:  return handle_cover (g, p_idx, m->cards, m->attack_cards, m->n_cards);
        case MOVE_PASS:   return handle_pass  (g, p_idx, m->cards, m->n_cards);
        case MOVE_PICKUP: return handle_pickup(g, p_idx);
        case MOVE_GOOD:   return handle_good  (g, p_idx);
        default:          return false;
    }
}

// Copy only the live prefix of a Game (header + first num_logs entries).
// logs[] is the final member, so everything else is inside the prefix.
static void cd_lite_clone(Game *dst, const Game *src) {
    size_t base = offsetof(Game, logs);
    memcpy(dst, src, base + (size_t)src->num_logs * sizeof(GameLog));
}

// ---------- belief state ------------------------------------------------

#define CD_MAX_VOIDS 6

typedef struct {
    Card pool[80];                  // unseen pool (deck ∪ opp unknowns)
    int  n;
    Card pinned[MAX_PLAYERS][MAX_HAND_SIZE];  // publicly located in p's hand
    int  pinned_n[MAX_PLAYERS];
    // Hard-ish void constraints (see blackpowder): attack cards a defender
    // demonstrably couldn't cover at pickup time. Cleared on their draw.
    Card voids[MAX_PLAYERS][CD_MAX_VOIDS];
    int  void_n[MAX_PLAYERS];
    // Soft rank floor: lowest non-trump value p can hold (0 = none). Set by
    // a single-card non-trump first attack at 3+ players in (lowest-first
    // attacker policies reveal their minimum). Cleared when p gains cards.
    int  floor_v[MAX_PLAYERS];
    // Trust flags, from observed behavior this game.
    bool distrust_floor[MAX_PLAYERS];
    bool distrust_void[MAX_PLAYERS];
} Belief;

static bool cd_void_forbidden(const Belief *B, const Game *g, int p, Card c) {
    for (int k = 0; k < B->void_n[p]; k++) {
        if (can_cover(B->voids[p][k], c, g->power_suit)) return true;
    }
    return false;
}

static bool cd_floor_forbidden(const Belief *B, const Game *g, int p, Card c) {
    return B->floor_v[p] > 0 && c.suit != g->power_suit && c.value < B->floor_v[p];
}

static void cd_pinned_remove(Belief *B, int p, Card c) {
    for (int q = 0; q < B->pinned_n[p]; q++) {
        if (card_eq(B->pinned[p][q], c)) {
            B->pinned[p][q] = B->pinned[p][B->pinned_n[p] - 1];
            B->pinned_n[p]--;
            return;
        }
    }
}

static void cd_pinned_add(Belief *B, int p, Card c) {
    if (B->pinned_n[p] >= MAX_HAND_SIZE) return;
    if (cd_set_contains(B->pinned[p], B->pinned_n[p], c)) return;
    B->pinned[p][B->pinned_n[p]++] = c;
}

// A floor contradiction (p plays a non-trump card below their inferred
// floor) means p is not a lowest-first attacker: distrust their floors.
static void cd_floor_check(Belief *B, const Game *g, int p, Card c) {
    if (p < 0 || B->floor_v[p] <= 0) return;
    if (c.suit != g->power_suit && c.value < B->floor_v[p]) {
        B->floor_v[p] = 0;
        B->distrust_floor[p] = true;
    }
}

// Chronological scan over logs: pinned cards, flipped-trump holder, void
// constraints, rank floors and trust flags, all in one pass.
static void cd_build_belief(const Game *g, int bot_idx, Belief *B) {
    memset(B, 0, sizeof(*B));

    // Last draw event: holds the flipped trump if the deck is exhausted, and
    // marks the moment the deck died (for "deck alive at log i" tests).
    int last_draw_idx = -1;
    for (int i = 0; i < g->num_logs; i++) {
        if (g->logs[i].log_type == LOG_DRAW) last_draw_idx = i;
    }
    bool deck_alive_now = (g->deck_count > 0 || g->has_flipped);
    int flip_log_idx = (!deck_alive_now && !cd_no_flip) ? last_draw_idx : -1;

    int trump_viol[MAX_PLAYERS] = {0};
    int in_now = g->num_players;

    // Replayed table state (per-move events only; PICKUP/DISCARD card lists
    // silently truncate at MAX_LOG_PAIRS=16 and must not be trusted).
    Card tbl[80];
    int  tbl_n = 0;
    Card unc[MAX_BATTLES * 2];  // uncovered attack cards only
    int  unc_n = 0;
    Card discards[160];
    int  disc_n = 0;

    for (int i = 0; i < g->num_logs; i++) {
        const GameLog *L = &g->logs[i];
        int p = L->player_idx;
        bool deck_alive_at = deck_alive_now || i <= last_draw_idx;
        switch (L->log_type) {
            case LOG_ATTACK:
            case LOG_PASS: {
                bool first_attack = (L->log_type == LOG_ATTACK && tbl_n == 0);
                bool any_trump = false;
                for (int k = 0; k < L->num_pairs; k++) {
                    Card c = L->pairs[k].primary;
                    if (c.suit == g->power_suit) any_trump = true;
                    if (unc_n < (int)(sizeof(unc) / sizeof(unc[0]))) unc[unc_n++] = c;
                    if (tbl_n < (int)(sizeof(tbl) / sizeof(tbl[0]))) tbl[tbl_n++] = c;
                    if (p >= 0 && p != bot_idx) {
                        cd_floor_check(B, g, p, c);
                        cd_pinned_remove(B, p, c);
                    }
                }
                if (p >= 0 && p != bot_idx && L->log_type == LOG_ATTACK) {
                    if (any_trump && deck_alive_at) trump_viol[p]++;
                    if (first_attack && L->num_pairs == 1 && !any_trump
                        && in_now > 2 && !cd_no_floors) {
                        B->floor_v[p] = L->pairs[0].primary.value;
                    }
                }
                break;
            }
            case LOG_COVER:
                for (int k = 0; k < L->num_pairs; k++) {
                    Card c = L->pairs[k].primary;
                    if (tbl_n < (int)(sizeof(tbl) / sizeof(tbl[0]))) tbl[tbl_n++] = c;
                    if (p >= 0 && p != bot_idx) {
                        cd_floor_check(B, g, p, c);
                        cd_pinned_remove(B, p, c);
                    }
                    if (!card_is_none(L->pairs[k].target)) {
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
                    // Exactly one uncovered attack => defender held no cover.
                    if (unc_n == 1 && B->void_n[p] < CD_MAX_VOIDS) {
                        B->voids[p][B->void_n[p]++] = unc[0];
                    }
                    for (int k = 0; k < tbl_n; k++) cd_pinned_add(B, p, tbl[k]);
                    B->floor_v[p] = 0;   // hand gained cards
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
                    B->void_n[p] = 0;    // new unknown cards: constraints expire
                    B->floor_v[p] = 0;
                    if (i == flip_log_idx) cd_pinned_add(B, p, g->flipped);
                }
                break;
            case LOG_PLAYER_OUT:
                in_now--;
                break;
            default:
                break;
        }
    }

    // Behavior-based trust: lowest-first attackers almost never lead trump
    // while the deck is alive, so one strike kills floors. Voids are NOT
    // gated this way — a trump lead can be forced (all-trump hand), and the
    // 1-in-4 unconstrained world mixture already absorbs void violators.
    for (int p = 0; p < g->num_players; p++) {
        if (trump_viol[p] >= 1) { B->distrust_floor[p] = true; B->floor_v[p] = 0; }
    }

    // Players that left the game hold nothing.
    for (int p = 0; p < g->num_players; p++) {
        if (p == bot_idx) { B->pinned_n[p] = 0; B->void_n[p] = 0; B->floor_v[p] = 0; continue; }
        if (g->players[p].status != PLAYER_STATUS_IN) {
            B->pinned_n[p] = 0;
            B->void_n[p] = 0;
            B->floor_v[p] = 0;
        }
        if (B->pinned_n[p] > g->players[p].hand_count) {
            B->pinned_n[p] = g->players[p].hand_count;  // defensive clamp
        }
    }

    // Unseen pool = full deck minus everything publicly located.
    Card known[160];
    int kn = 0;
    const Player *bot = &g->players[bot_idx];
    for (int j = 0; j < bot->hand_count; j++) known[kn++] = bot->hand[j];
    for (int i = 0; i < g->num_battles; i++) {
        known[kn++] = g->table_battles[i].attack;
        if (!card_is_none(g->table_battles[i].defense)) known[kn++] = g->table_battles[i].defense;
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
            if (!cd_set_contains(known, kn, c)) B->pool[B->n++] = c;
        }
    }

    // Feasibility: if constraints leave fewer allowed pool cards than the
    // player's unknown count, relax floors first, then voids.
    for (int p = 0; p < g->num_players; p++) {
        if (B->void_n[p] == 0 && B->floor_v[p] == 0) continue;
        int unknown = g->players[p].hand_count - B->pinned_n[p];
        if (unknown <= 0) continue;
        int allowed = 0;
        for (int i = 0; i < B->n; i++) {
            if (!cd_void_forbidden(B, g, p, B->pool[i])
                && !cd_floor_forbidden(B, g, p, B->pool[i])) allowed++;
        }
        if (allowed < unknown && B->floor_v[p] > 0) {
            B->floor_v[p] = 0;
            allowed = 0;
            for (int i = 0; i < B->n; i++) {
                if (!cd_void_forbidden(B, g, p, B->pool[i])) allowed++;
            }
        }
        if (allowed < unknown) B->void_n[p] = 0;
    }
}

// ---------- world sampling ------------------------------------------------

// Sample one consistent world. The world keeps only LOG_DISCARD entries of
// the real log (all any rollout policy reads), so downstream trial clones
// stay small. Constraint-violating dealt cards are repaired by swapping with
// compatible deck cards; impossible repairs degrade gracefully.
static _Thread_local int cd_full_logs = 0;   // CD_FULL_LOGS=1: bp-style worlds

static void cd_sample_world(Game *g_out, const Game *g_in, int my_idx,
                            const Belief *B, uint32_t seed,
                            bool apply_voids, bool apply_floors) {
    if (cd_full_logs) {
        game_clone(g_out, g_in);
    } else {
        memcpy(g_out, g_in, offsetof(Game, logs));
        int nl = 0;
        for (int i = 0; i < g_in->num_logs; i++) {
            if (g_in->logs[i].log_type == LOG_DISCARD) g_out->logs[nl++] = g_in->logs[i];
        }
        g_out->num_logs = nl;
    }

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
        s = cd_xorshift(s);
        int j = (int)(s % (uint32_t)(i + 1));
        Card sw = hidden[i]; hidden[i] = hidden[j]; hidden[j] = sw;
    }

    // Deal: deck first, then each opponent's unknown slots.
    int k = 0;
    int deck_n = g_in->deck_count;
    for (int i = 0; i < deck_n && k < hn; i++) g_out->deck[i] = hidden[k++];

    typedef struct { int player, slot; } SlotRef;
    SlotRef slots[80];
    int ns = 0;
    for (int i = 0; i < g_in->num_players; i++) {
        if (i == my_idx) continue;
        int need = g_in->players[i].hand_count - B->pinned_n[i];
        for (int j = 0; j < need && k < hn; j++) {
            g_out->players[i].hand[B->pinned_n[i] + j] = hidden[k];
            slots[ns].player = i;
            slots[ns].slot = B->pinned_n[i] + j;
            ns++;
            k++;
        }
    }

    if (!apply_voids && !apply_floors) return;
    for (int si = 0; si < ns; si++) {
        int p = slots[si].player;
        bool use_v = apply_voids && B->void_n[p] > 0;
        bool use_f = apply_floors && B->floor_v[p] > 0;
        if (!use_v && !use_f) continue;
        Card c = g_out->players[p].hand[slots[si].slot];
        bool bad = (use_v && cd_void_forbidden(B, g_in, p, c))
                || (use_f && cd_floor_forbidden(B, g_in, p, c));
        if (!bad) continue;
        for (int d = 0; d < deck_n; d++) {
            Card dc = g_out->deck[d];
            bool dc_bad = (use_v && cd_void_forbidden(B, g_in, p, dc))
                       || (use_f && cd_floor_forbidden(B, g_in, p, dc));
            if (!dc_bad) {
                g_out->deck[d] = c;
                g_out->players[p].hand[slots[si].slot] = dc;
                break;
            }
        }
    }
}

// ---------- exact solver (shared by root + rollout leaves) -----------------

#define CD_SOLVE_MAX_DEPTH   48
#define CD_SOLVE_MAX_MOVES   96
#define CD_SOLVE_BUDGET      200000L
#define CD_AVOID_BUDGET      150000L
#define CD_SOLVE_MAX_CARDS   20
#define CD_LEAF_BUDGET       1500L

typedef struct {
    long budget;
    bool aborted;
    int  me;
    SolveMoves *mv;      // [CD_SOLVE_MAX_DEPTH]
} Solver;

static _Thread_local SolveMoves *cd_solver_mv = NULL;

_Static_assert(CD_SOLVE_MAX_DEPTH <= SOLVE_SCRATCH_DEPTH,
               "shared solver scratch shallower than this family's depth");
static bool cd_solver_ready(void) {
    if (!cd_solver_mv) {
        cd_solver_mv = solve_scratch_mv();
    }
    return cd_solver_mv != NULL;
}

// Value in [-1000, 1000] from `me`'s perspective: positive = me escaping,
// negative = me as durak. Magnitude prefers faster wins / slower losses.
static int cd_solve(Solver *S, const Game *g, int alpha, int beta, int depth) {
    int loser = game_done(g);
    if (loser >= 0) return (loser == S->me) ? -(1000 - depth) : (1000 - depth);
    if (cd_in_count(g) == 0) return 0;   // defensive: simultaneous out
    if (depth >= CD_SOLVE_MAX_DEPTH) { S->aborted = true; return 0; }
    if (--S->budget <= 0) { S->aborted = true; return 0; }

    // Defender-priority sequential abstraction of the real loop.
    int actor = -1;
    if (should_bot_act(g, g->defender)) actor = g->defender;
    else {
        for (int i = 0; i < g->num_players; i++) {
            if (should_bot_act(g, i)) { actor = i; break; }
        }
    }
    if (actor < 0) return 0;

    SolveMoves *mv = &S->mv[depth];
    // Bounded generation into the compact scratch slot (see SOLVE_SCRATCH_MOVES
    // in cordite_sim.h): the cast is safe because SolveMoves shares LegalMoves'
    // leading layout and the cap keeps writes within its {n, moves[]} bounds.
    legal_set_move_cap(SOLVE_SCRATCH_MOVES);
    calculate_legal_moves(g, actor, (LegalMoves *)mv);
    legal_set_move_cap(0);
    if (mv->n == 0) return 0;
    if (mv->n > CD_SOLVE_MAX_MOVES) { S->aborted = true; return 0; }

    bool maximizing = (actor == S->me);
    int best = maximizing ? -2000 : 2000;
    for (int i = 0; i < mv->n; i++) {
        Game *child = solve_scratch_child(depth);
        solve_clone_prefix(child, g);
        if (!cd_apply(child, actor, &mv->moves[i])) continue;
        int v = cd_solve(S, child, alpha, beta, depth + 1);
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

// Test hook: run the struct solver on a position from `me`'s perspective with
// the given window/budget. Returns the value; *aborted set on budget/depth
// blow. Used by tests/solver_difftest.c to validate the bitboard solver.
int cd_struct_solve_test(const Game *g, int me, int alpha, int beta,
                         long budget, int *aborted) {
    if (!cd_solver_ready()) { if (aborted) *aborted = 1; return 0; }
    Game root;
    cd_lite_clone(&root, g);
    root.num_logs = 0;
    Solver S;
    S.budget = budget;
    S.aborted = false;
    S.me = me;
    S.mv = cd_solver_mv;
    int v = cd_solve(&S, &root, alpha, beta, 0);
    if (aborted) *aborted = S.aborted;
    return v;
}

// Exact full-info resolution of a rollout leaf: 2 players in, deck gone,
// few cards. Returns the loser index, or -1 if unresolved (abort/draw).
static _Thread_local long cd_leaf_budget = CD_LEAF_BUDGET;
static _Thread_local int  cd_leaf_max_cards = 0;   // set from env at init
static _Thread_local int  cd_floor_mod = 2;        // floors in 1/mod worlds

static int cd_leaf_solve(const Game *g) {
    if (!cd_solver_ready()) return -1;
    int me = -1;
    for (int i = 0; i < g->num_players; i++) {
        if (g->players[i].status == PLAYER_STATUS_IN) { me = i; break; }
    }
    if (me < 0) return -1;
    int opp = -1;
    for (int i = me + 1; i < g->num_players; i++) {
        if (g->players[i].status == PLAYER_STATUS_IN) { opp = i; break; }
    }
    if (opp < 0) return -1;

    Game root;
    cd_lite_clone(&root, g);
    root.num_logs = 0;   // solver never reads history

    Solver S;
    S.budget  = cd_leaf_budget;
    S.aborted = false;
    S.me      = me;
    S.mv      = cd_solver_mv;

    // Null window around 0: only the sign matters (true values are ±(1000-d)
    // for decided games), and the narrow window maximizes pruning.
    int v = cd_solve(&S, &root, -1, 1, 0);
    if (S.aborted || v == 0) return -1;
    return (v > 0) ? opp : me;
}

// ---------- simulation ---------------------------------------------------

// Stage-aware rollout policy (gunpowder's rule): handwritten while the deck
// is alive or the game is heads-up, espresso for multi-player endgames.
static StrategyFn cd_rollout_for(const Game *g) {
    // CD_ROLLOUT (struct path only): 1 = espresso everywhere, 2 = handwritten
    // everywhere. Research knob for the "rollout-policy bias" hypothesis — vs a
    // strong opponent, a weak (handwritten) rollout policy biases value
    // estimates, so more worlds saturates. A stronger rollout policy may reduce
    // that bias. Run with CD_NO_FASTROLL=1.
    if (cd_rollout_policy == 1) return espresso_strategy_choose;
    if (cd_rollout_policy == 2) return handwritten_strategy_choose;
    bool deck_active = (g->deck_count > 0 || g->has_flipped);
    if (deck_active || cd_in_count(g) == 2) return handwritten_strategy_choose;
    return espresso_strategy_choose;
}

// ---------- fulminate: per-seat rollout-policy override ---------------------
// Port of the seat-weight machinery + archetype sim choosers in
// cordite_core.ts (POL_*/ARCH_POLICIES 1356-1576, setSeatWeights/
// samplePolicyTable 1596-1617, the simulate() per-seat hook 2088-2102 and the
// world-loop sampling call 2749-2751). Everything here is INERT unless
// cordite_set_seat_weights installed a posterior: every new branch is guarded
// by cd_seat_weights_on / cd_seat_policy_dev, so plain cordite stays
// bit-for-bit unchanged (verified against a fixed-seed baseline).
//
// Representation note: the TS archetype choosers run on the enumerated LITE
// move list (calcLegal(g, pi, true)); the C equivalents below run on
// calculate_legal_moves_lite's LegalMoves, which enumerates the same move set
// in the same order (attacks / greedy-cover+pickup+pass / attacks+good). Seats
// whose sampled policy IS handwritten keep cordite's existing dispatch; a
// world whose sampled table deviates runs on the struct rollout (cd_simulate)
// instead of the bitboard fast path — the two are behavior-identical for
// handwritten seats (sim_difftest), so only the deviating seats actually play
// differently, exactly as in the TS per-seat skip of the fast-roll path.

static _Thread_local double cd_seat_weights[MAX_PLAYERS][CORDITE_NUM_POLICIES];
static _Thread_local int    cd_seat_weights_n = 0;
static _Thread_local int    cd_seat_weights_on = 0;
// Per-world sampled policy table + "any seat deviates" flag. Re-sampled at the
// top of every world while weights are installed; cd_simulate reads it.
static _Thread_local int8_t cd_seat_policy[MAX_PLAYERS];
static _Thread_local int    cd_seat_policy_dev = 0;

void cordite_set_seat_weights(const double w[MAX_PLAYERS][CORDITE_NUM_POLICIES],
                              int num_players) {
    if (!w || num_players <= 0) { cordite_clear_seat_weights(); return; }
    if (num_players > MAX_PLAYERS) num_players = MAX_PLAYERS;
    for (int p = 0; p < num_players; p++) {
        for (int k = 0; k < CORDITE_NUM_POLICIES; k++) {
            cd_seat_weights[p][k] = w[p][k];
        }
    }
    cd_seat_weights_n = num_players;
    cd_seat_weights_on = 1;
    cd_seat_policy_dev = 0;   // TS setSeatWeights: seatPolicy = null until sampled
}

void cordite_clear_seat_weights(void) {
    cd_seat_weights_on = 0;
    cd_seat_weights_n = 0;
    cd_seat_policy_dev = 0;
}

// Per-world sampler (cordite_core.ts samplePolicyTable, lines 1605-1617).
// Draws each seat's policy from its weight vector with a small LCG seeded by
// the world seed — independent of the rollout RNG so it never perturbs the
// CRN stream. The recurrence is EXACTLY the TS one (uint32 wrap + /2^32).
// Returns 1 if any seat's drawn policy deviates from handwritten.
static int cd_fm_sample_table(uint32_t seed, int n) {
    uint32_t s = seed ^ 0x9e3779b9u;
    int dev = 0;
    if (n > cd_seat_weights_n) n = cd_seat_weights_n;
    for (int p = 0; p < n; p++) {
        const double *w = cd_seat_weights[p];
        s = s * 1664525u + 1013904223u;
        double r = (double)s / 4294967296.0;
        int k = 0;
        for (; k < CORDITE_NUM_POLICIES - 1; k++) {
            if (r < w[k]) break;
            r -= w[k];
        }
        cd_seat_policy[p] = (int8_t)k;
        if (k != CORDITE_POL_HANDWRITTEN) dev = 1;
    }
    return dev;
}

// ---- archetype choosers (cordite_core.ts 1362-1569) ------------------------
// These mirror the TS sim-level choosers move-for-move: same scores, same
// strict-inequality tie-breaks (first enumerated move wins ties), same
// cascade order, and the same RNG draws (randomChoose consumes exactly one
// game_random(); simple/greedy/passive/human consume none).

// randomChoose (TS 1362-1368): uniform over the legal list, one RNG draw from
// the SIM stream (not random_strategy's separate LCG — the TS chooser uses
// rngNext, which is the C game_random stream inside rollouts).
static int cd_arch_random(const LegalMoves *moves) {
    int idx = (int)(game_random() * moves->n);
    if (idx < 0) idx = 0;
    if (idx >= moves->n) idx = moves->n - 1;
    return idx;
}

// shScore (TS 1375-1379): summed value + flat trump penalty.
static int cd_sh_score(const LegalMove *m, int power, int trump_penalty) {
    int s = 0;
    for (int i = 0; i < m->n_cards; i++) {
        s += m->cards[i].value + (m->cards[i].suit == power ? trump_penalty : 0);
    }
    return s;
}

// shGiveUp (TS 1381-1405): strategic give-up when not every uncovered attack
// is defendable, trumps-needed exceeds half the trump count, or the table is
// heavy relative to the hand.
static bool cd_sh_give_up(const Game *g, int p_idx) {
    const Player *pl = &g->players[p_idx];
    int power = g->power_suit;
    Card unc[MAX_BATTLES];
    int un = 0;
    for (int i = 0; i < g->num_battles; i++) {
        if (!!card_is_none(g->table_battles[i].defense)) unc[un++] = g->table_battles[i].attack;
    }
    if (un == 0) return false;
    int defendable = 0, trumps_needed = 0;
    for (int a = 0; a < un; a++) {
        bool same = false;
        for (int j = 0; j < pl->hand_count; j++) {
            if (pl->hand[j].suit == unc[a].suit && pl->hand[j].value > unc[a].value) {
                same = true;
                break;
            }
        }
        if (same) { defendable++; continue; }
        bool trump = false;
        for (int j = 0; j < pl->hand_count; j++) {
            if (pl->hand[j].suit == power && can_cover(unc[a], pl->hand[j], power)) {
                trump = true;
                break;
            }
        }
        if (trump) { defendable++; trumps_needed++; }
    }
    if (defendable < un) return true;
    int trump_count = 0;
    for (int j = 0; j < pl->hand_count; j++) {
        if (pl->hand[j].suit == power) trump_count++;
    }
    // TS `trumpsNeeded > trumpCount / 2` is FLOAT division (JS number math).
    if ((double)trumps_needed > (double)trump_count / 2.0) return true;
    int att_sum = 0;
    for (int a = 0; a < un; a++) att_sum += unc[a].value;
    int hand_sum = 0;
    for (int j = 0; j < pl->hand_count; j++) hand_sum += pl->hand[j].value;
    double avg_att = (double)att_sum / (double)un;
    double avg_hand = pl->hand_count ? (double)hand_sum / (double)pl->hand_count : 0.0;
    return avg_att > avg_hand + 2 && un >= 3;
}

// simpleHeuristicChoose (TS 1406-1446): behavior-faithful sim port of
// simple_heuristic_strategy — lowest-cost attack/cover/pass with trump
// penalties, strategic give-up (pickup) when overwhelmed.
static int cd_arch_simple(const Game *g, int p_idx, const LegalMoves *moves) {
    int power = g->power_suit;
    bool is_def = (p_idx == g->defender);
    int attack_best = -1, attack_score = INT32_MAX;
    int cover_best = -1, cover_score = INT32_MAX;
    int pass_best = -1, pass_score = INT32_MAX;
    int good_idx = -1, pickup_idx = -1;
    for (int i = 0; i < moves->n; i++) {
        const LegalMove *m = &moves->moves[i];
        switch (m->type) {
            case MOVE_ATTACK: {
                int s = cd_sh_score(m, power, 20);
                if (s < attack_score) { attack_score = s; attack_best = i; }
                break;
            }
            case MOVE_COVER: {
                int s = cd_sh_score(m, power, 10);
                if (s < cover_score) { cover_score = s; cover_best = i; }
                break;
            }
            case MOVE_PASS: {
                int s = cd_sh_score(m, power, 20);
                if (s < pass_score) { pass_score = s; pass_best = i; }
                break;
            }
            case MOVE_GOOD:   good_idx = i;   break;
            case MOVE_PICKUP: pickup_idx = i; break;
            default: break;
        }
    }
    bool is_attacker = (p_idx == g->first_attacker) || !is_def;
    if (attack_best >= 0 && is_attacker) return attack_best;
    if (cover_best >= 0 && is_def) {
        if (pickup_idx >= 0 && cd_sh_give_up(g, p_idx)) return pickup_idx;
        return cover_best;
    }
    if (pass_best >= 0) return pass_best;
    if (good_idx >= 0) return good_idx;
    if (pickup_idx >= 0) return pickup_idx;
    return attack_best >= 0 ? attack_best : 0;
}

// greedyChoose (TS 1453-1484): "dumb greedy" — always piles on with the most
// cards / HIGHEST scores, covers with its highest card, never gives up
// strategically. Deliberately wasteful (the exploitable disposer archetype).
static int cd_arch_greedy(const Game *g, const LegalMoves *moves) {
    int power = g->power_suit;
    int attack_best = -1; int32_t attack_key = INT32_MIN;
    int cover_best = -1;  int32_t cover_key = INT32_MIN;
    int pass_best = -1, good_idx = -1, pickup_idx = -1;
    for (int i = 0; i < moves->n; i++) {
        const LegalMove *m = &moves->moves[i];
        switch (m->type) {
            case MOVE_ATTACK: {
                int32_t s = 0;
                for (int j = 0; j < m->n_cards; j++) s += cd_card_score(m->cards[j], power);
                int32_t key = m->n_cards * 1000 + s;
                if (key > attack_key) { attack_key = key; attack_best = i; }
                break;
            }
            case MOVE_COVER: {
                int32_t s = 0;
                for (int j = 0; j < m->n_cards; j++) s += cd_card_score(m->cards[j], power);
                if (s > cover_key) { cover_key = s; cover_best = i; }
                break;
            }
            case MOVE_PASS: if (pass_best < 0) pass_best = i; break;
            case MOVE_GOOD:   good_idx = i;   break;
            case MOVE_PICKUP: pickup_idx = i; break;
            default: break;
        }
    }
    if (attack_best >= 0) return attack_best;   // greedy: always pile on if able
    if (cover_best >= 0) return cover_best;     // always defend, wastefully
    if (pass_best >= 0) return pass_best;
    if (good_idx >= 0) return good_idx;
    if (pickup_idx >= 0) return pickup_idx;
    return 0;
}

// passiveChoose (TS 1492-1534): timid hoarder — leads the single lowest
// non-trump, prefers to STOP over piling on, refuses to spend trumps on
// defense (takes instead).
static int cd_arch_passive(const Game *g, int p_idx, const LegalMoves *moves) {
    int power = g->power_suit;
    int attack_best = -1; int32_t attack_key = INT32_MAX;
    int cover_best = -1;  int32_t cover_key = INT32_MAX;
    bool cover_trump = false;
    int pass_best = -1;   int32_t pass_key = INT32_MAX;
    int good_idx = -1, pickup_idx = -1;
    for (int i = 0; i < moves->n; i++) {
        const LegalMove *m = &moves->moves[i];
        switch (m->type) {
            case MOVE_ATTACK: {
                int32_t s = 0, tr = 0;
                for (int j = 0; j < m->n_cards; j++) {
                    s += m->cards[j].value;
                    if (m->cards[j].suit == power) tr++;
                }
                int32_t key = m->n_cards * 1000 + s + tr * 200;
                if (key < attack_key) { attack_key = key; attack_best = i; }
                break;
            }
            case MOVE_COVER: {
                int32_t s = 0;
                bool any_tr = false;
                for (int j = 0; j < m->n_cards; j++) {
                    s += m->cards[j].value;
                    if (m->cards[j].suit == power) { any_tr = true; s += 200; }
                }
                if (s < cover_key) { cover_key = s; cover_best = i; cover_trump = any_tr; }
                break;
            }
            case MOVE_PASS: {
                int32_t s = 0;
                for (int j = 0; j < m->n_cards; j++) s += m->cards[j].value;
                if (s < pass_key) { pass_key = s; pass_best = i; }
                break;
            }
            case MOVE_GOOD:   good_idx = i;   break;
            case MOVE_PICKUP: pickup_idx = i; break;
            default: break;
        }
    }
    bool is_def = (p_idx == g->defender);
    if (is_def) {
        if (cover_best >= 0 && !cover_trump) return cover_best;  // cheap non-trump cover only
        if (pickup_idx >= 0) return pickup_idx;                  // else take, don't spend a trump
        if (cover_best >= 0) return cover_best;
        if (pass_best >= 0) return pass_best;
    } else {
        if (good_idx >= 0) return good_idx;                      // prefer to STOP over piling on
        if (attack_best >= 0) return attack_best;                // else a single low lead
    }
    if (attack_best >= 0) return attack_best;
    if (pass_best >= 0) return pass_best;
    if (good_idx >= 0) return good_idx;
    if (pickup_idx >= 0) return pickup_idx;
    return 0;
}

// humanChoose (TS 1542-1569): loss-averse stubborn defender — plays low like
// simple but NEVER makes a strategic pickup (always covers when able) and
// prefers covering to passing.
static int cd_arch_human(const Game *g, int p_idx, const LegalMoves *moves) {
    int power = g->power_suit;
    int attack_best = -1, attack_score = INT32_MAX;
    int cover_best = -1, cover_score = INT32_MAX;
    int pass_best = -1, pass_score = INT32_MAX;
    int good_idx = -1, pickup_idx = -1;
    for (int i = 0; i < moves->n; i++) {
        const LegalMove *m = &moves->moves[i];
        switch (m->type) {
            case MOVE_ATTACK: {
                int s = cd_sh_score(m, power, 20);
                if (s < attack_score) { attack_score = s; attack_best = i; }
                break;
            }
            case MOVE_COVER: {
                int s = cd_sh_score(m, power, 10);
                if (s < cover_score) { cover_score = s; cover_best = i; }
                break;
            }
            case MOVE_PASS: {
                int s = cd_sh_score(m, power, 20);
                if (s < pass_score) { pass_score = s; pass_best = i; }
                break;
            }
            case MOVE_GOOD:   good_idx = i;   break;
            case MOVE_PICKUP: pickup_idx = i; break;
            default: break;
        }
    }
    bool is_def = (p_idx == g->defender);
    if (is_def) {
        if (cover_best >= 0) return cover_best;   // never give up: always cover if able
        if (pass_best >= 0) return pass_best;
        if (pickup_idx >= 0) return pickup_idx;
    }
    if (attack_best >= 0) return attack_best;
    if (pass_best >= 0) return pass_best;
    if (good_idx >= 0) return good_idx;
    if (pickup_idx >= 0) return pickup_idx;
    return 0;
}

// ARCH_POLICIES dispatch (TS 1573-1576). Indices 0-2 reuse the existing C
// equivalents the TS choosers were themselves ported from: handwritten never
// reaches here (the caller keeps cordite's own dispatch for it) and espresso
// is the real espresso_strategy_choose (it reads the sampled world's hands —
// legitimate inside a determinized world, same argument as the TS sim port).
static int cd_arch_choose(int pol, const Game *g, int p_idx, const LegalMoves *moves) {
    switch (pol) {
        case CORDITE_POL_ESPRESSO: return espresso_strategy_choose(g, p_idx, moves, NULL);
        case CORDITE_POL_RANDOM:   return cd_arch_random(moves);
        case CORDITE_POL_SIMPLE:   return cd_arch_simple(g, p_idx, moves);
        case CORDITE_POL_GREEDY:   return cd_arch_greedy(g, moves);
        case CORDITE_POL_PASSIVE:  return cd_arch_passive(g, p_idx, moves);
        case CORDITE_POL_HUMAN:    return cd_arch_human(g, p_idx, moves);
        default: {
            // POL_HANDWRITTEN is filtered by the caller; defensive fallback
            // keeps cordite's own stage-aware dispatch.
            StrategyFn fn = cd_rollout_for(g);
            return fn(g, p_idx, moves, NULL);
        }
    }
}

// Roll a sampled world forward; returns my finish position (1..N), or 0 if
// the simulation didn't terminate. Exits early once my position is known,
// and resolves small 2-player deck-empty endgames exactly (one attempt per
// rollout — a failed solve falls back to policy play for good).
static int cd_simulate(Game *g, int my_idx, int max_turns) {
    int turns = 0;
    bool leaf_tried = false;
    while (game_done(g) < 0 && turns++ < max_turns) {
        // My fate is sealed as soon as I'm out: position = elimination slot.
        if (!cd_no_earlyexit
            && g->players[my_idx].status != PLAYER_STATUS_IN) {
            for (int i = 0; i < g->num_eliminated; i++) {
                if (g->elimination_order[i] == my_idx) return i + 1;
            }
            break;   // not IN and not eliminated: corrupt state, bail
        }

        if (!cd_no_leaf && !leaf_tried && g->deck_count == 0 && !g->has_flipped
            && cd_in_count(g) == 2) {
            int total = 0;
            for (int i = 0; i < g->num_players; i++) {
                if (g->players[i].status == PLAYER_STATUS_IN)
                    total += g->players[i].hand_count;
            }
            if (total <= cd_leaf_max_cards) {
                leaf_tried = true;
                int loser = cd_leaf_solve(g);
                if (loser >= 0) {
                    // 2 left => positions N-1 and N remain.
                    return (loser == my_idx) ? g->num_players : g->num_players - 1;
                }
            }
        }

        bool acted = false;
        for (int pi = 0; pi < g->num_players; pi++) {
            if (!should_bot_act(g, pi)) continue;
            // Static, not stack: LegalMoves is ~10MB at the wasm build's
            // MAX_LEGAL_MOVES and the wasm stack is 1MB — a stack instance
            // traps the module the moment a rollout reaches this ply loop.
            // Consumed fully before the next iteration, so one per thread.
            static _Thread_local LegalMoves moves;
            calculate_legal_moves_lite(g, pi, &moves);
            if (moves.n == 0) continue;
            int idx;
            if (cd_seat_policy_dev && cd_seat_policy[pi] != CORDITE_POL_HANDWRITTEN) {
                // fulminate: a profiled seat with a non-default sampled policy
                // plays with its archetype chooser on the lite move set
                // (cordite_core.ts simulate(), lines 2095-2101). Seats mapped
                // to POL_HANDWRITTEN keep cordite's exact existing dispatch.
                idx = cd_arch_choose(cd_seat_policy[pi], g, pi, &moves);
            } else {
                StrategyFn fn = cd_rollout_for(g);
                idx = fn(g, pi, &moves, NULL);
            }
            if (idx < 0 || idx >= moves.n) continue;
            if (cd_apply(g, pi, &moves.moves[idx])) { acted = true; break; }
        }
        if (!acted) break;
    }
    if (game_done(g) < 0) return 0;
    for (int i = 0; i < g->num_eliminated; i++) {
        if (g->elimination_order[i] == my_idx) return i + 1;
    }
    return g->num_players;
}

// Fast bitboard rollout: convert the determinized world to a compact SimState
// and play it out on bitmasks. ~10x faster per ply than cd_simulate. The
// effective rollout policy is always handwritten (see cordite_sim.c), and the
// exact leaf solver (CD_LEAF, off by default) is not used in the fast path.
static int cd_simulate_fast(const Game *g, int my_idx, int max_turns) {
    SimState s;
    cd_sim_from_game(&s, g);
    return cd_sim_playout(&s, my_idx, max_turns, !cd_no_earlyexit);
}

// Dispatcher: bitboard rollout by default; struct rollout under CD_NO_FASTROLL
// or CD_LEAF (the leaf solver lives on the struct path). CD_DIFFTEST runs both
// and tallies divergences (printed at process exit by the eval harness if it
// hooks cd_difftest_report, else just counted).
static _Thread_local long cd_diff_total = 0, cd_diff_mismatch = 0;
static int cd_rollout(Game *g, int my_idx, int max_turns) {
    if (cd_no_fastroll || !cd_no_leaf) return cd_simulate(g, my_idx, max_turns);
    if (cd_difftest) {
        uint32_t rng0 = game_rng_get();
        int fast = cd_simulate_fast(g, my_idx, max_turns);
        game_rng_set(rng0);
        Game slow_g;
        cd_lite_clone(&slow_g, g);
        int slow = cd_simulate(&slow_g, my_idx, max_turns);
        cd_diff_total++;
        if (fast != slow) {
            cd_diff_mismatch++;
            if (cd_diff_mismatch <= 20) {
                fprintf(stderr, "CD_DIFFTEST mismatch #%ld: fast=%d slow=%d "
                        "(np=%d deck=%d logs=%d)\n", cd_diff_mismatch, fast, slow,
                        g->num_players, g->deck_count, g->num_logs);
            }
        }
        return slow;  // keep slow behavior while difftesting
    }
    return cd_simulate_fast(g, my_idx, max_turns);
}

void cd_difftest_report(void) {
    if (cd_diff_total > 0) {
        fprintf(stderr, "CD_DIFFTEST: %ld/%ld rollouts diverged (%.3f%%)\n",
                cd_diff_mismatch, cd_diff_total,
                100.0 * (double)cd_diff_mismatch / (double)cd_diff_total);
    }
}

// ---------- root endgame solve (win take + loss avoid) ---------------------

// Solve every root move with a full window when 2 players remain and the
// deck is empty (the unseen pool IS the opponent's hand — public deduction).
// Returns the fastest forced-win index, or -1. When no win exists, sets
// forced_loss[i] for root moves that lose under optimal play; the MC stage
// avoids them whenever at least one non-losing move exists.
static int cd_try_endgame_solve(const Game *g, int bot_idx,
                                const LegalMoves *moves, const Belief *B,
                                bool *forced_loss, int *n_safe) {
    *n_safe = moves->n;
    if (g->deck_count > 0 || g->has_flipped) return -1;
    if (cd_in_count(g) != 2) return -1;
    if (g->players[bot_idx].status != PLAYER_STATUS_IN) return -1;

    int opp = -1;
    for (int i = 0; i < g->num_players; i++) {
        if (i != bot_idx && g->players[i].status == PLAYER_STATUS_IN) opp = i;
    }
    if (opp < 0) return -1;

    int unknown = g->players[opp].hand_count - B->pinned_n[opp];
    if (unknown < 0 || unknown != B->n) return -1;  // deduction failed; bail

    int total = g->players[bot_idx].hand_count + g->players[opp].hand_count;
    if (total > CD_SOLVE_MAX_CARDS) return -1;

    if (!cd_solver_ready()) return -1;

    Game root;
    game_clone(&root, g);
    root.num_logs = 0;
    for (int k = 0; k < B->pinned_n[opp]; k++) {
        root.players[opp].hand[k] = B->pinned[opp][k];
    }
    for (int k = 0; k < B->n; k++) {
        root.players[opp].hand[B->pinned_n[opp] + k] = B->pool[k];
    }

    // Fast path: solve on the compact bitboard engine (transposition table +
    // O(1) clone + bitmask move-gen). The bitboard solver returns the exact
    // same value as the struct solver when resolved (validated by
    // tests/solver_difftest.c), and resolves more positions within budget.
    // CD_NO_BBSOLVE=1 falls back to the struct solver for A/B.
    bool bbsolve = !cd_flag("CD_NO_BBSOLVE");
    SimState root_sim;
    if (bbsolve) cd_sim_from_game(&root_sim, &root);
    cd_sim_solve_reset();

    Solver S;
    S.budget  = CD_SOLVE_BUDGET;
    S.aborted = false;
    S.me      = bot_idx;
    S.mv      = cd_solver_mv;
    long win_budget   = bbsolve ? (long)cd_bb_win_budget   : CD_SOLVE_BUDGET;
    long avoid_budget = bbsolve ? (long)cd_bb_avoid_budget : CD_AVOID_BUDGET;
    long budget = win_budget;

    // Pass 1 — win hunt (blackpowder's loop): fail-soft with an accumulating
    // alpha floor at 0, so losing subtrees prune immediately.
    // CD_BP_SOLVE=1 reverts to blackpowder's exact semantics (alpha starts
    // wide open, any abort bails the whole solve) for A/B testing.
    int best_idx = -1;
    int best_v = 0;       // only accept strictly winning lines
    int alpha = cd_flag("CD_BP_SOLVE") ? -2000 : 0;
    bool bail_on_abort = (alpha == -2000);
    bool any_abort = false;
    for (int i = 0; i < moves->n; i++) {
        int v, aborted_i;
        if (bbsolve) {
            SimState child = root_sim;
            if (!cd_sim_apply_root_move(&child, bot_idx, &moves->moves[i])) continue;
            v = cd_sim_solve_d(&child, bot_idx, alpha, 2000, &budget, 1, &aborted_i);
            if (budget <= 0) return -1;   // shared budget drained: no claims
        } else {
            Game child;
            cd_lite_clone(&child, &root);
            if (!cd_apply(&child, bot_idx, &moves->moves[i])) continue;
            S.aborted = false;
            v = cd_solve(&S, &child, alpha, 2000, 1);
            aborted_i = S.aborted;
            if (S.budget <= 0) return -1;
        }
        if (aborted_i) { if (bail_on_abort) return -1; any_abort = true; continue; }
        if (v > best_v) { best_v = v; best_idx = i; }
        if (v > alpha) alpha = v;
    }
    if (best_idx >= 0) return best_idx;
    if (cd_no_avoid || any_abort) return -1;

    // Pass 2 — loss avoidance: no win exists, so classify each move with a
    // null window around 0 (sign only, maximal pruning).
    S.budget = CD_AVOID_BUDGET;
    budget = avoid_budget;
    int n_loss = 0, n_nonloss = 0;
    for (int i = 0; i < moves->n; i++) {
        int v, aborted_i;
        if (bbsolve) {
            SimState child = root_sim;
            if (!cd_sim_apply_root_move(&child, bot_idx, &moves->moves[i])) continue;
            aborted_i = 0;
            v = cd_sim_solve_d(&child, bot_idx, -1, 0, &budget, 1, &aborted_i);
            if (budget <= 0) aborted_i = 1;
        } else {
            Game child;
            cd_lite_clone(&child, &root);
            if (!cd_apply(&child, bot_idx, &moves->moves[i])) continue;
            S.aborted = false;
            v = cd_solve(&S, &child, -1, 0, 1);
            aborted_i = (S.budget <= 0) || S.aborted;
        }
        if (aborted_i) continue;   // unknown
        if (v < 0) { forced_loss[i] = true; n_loss++; }
        else n_nonloss++;
    }
    // Only restrict when some move is PROVEN non-losing — otherwise the
    // "safe" set would just be the moves the solver failed to read (adverse
    // selection), while MC models the imperfect opponent better anyway.
    if (n_loss > 0 && n_nonloss > 0) {
        *n_safe = moves->n - n_loss;
    } else {
        for (int i = 0; i < moves->n; i++) forced_loss[i] = false;
        *n_safe = moves->n;
    }
    return -1;
}

// ---------- candidate selection -------------------------------------------

#define CD_MAX_CANDS 26

typedef struct {
    int idx[CD_MAX_CANDS];
    int n;
} Candidates;

static void cd_ranked_insert(int *idxs, double *keys, int *n, int cap,
                             int idx, double key) {
    int pos = *n;
    while (pos > 0 && keys[pos - 1] > key) pos--;
    if (pos >= cap) return;
    int last = (*n < cap) ? *n : cap - 1;
    for (int i = last; i > pos; i--) { idxs[i] = idxs[i - 1]; keys[i] = keys[i - 1]; }
    idxs[pos] = idx; keys[pos] = key;
    if (*n < cap) (*n)++;
}

static void cd_pick_candidates(const Game *g, const LegalMoves *moves,
                               const bool *excluded, Candidates *out) {
    int power = g->power_suit;

    int atk[12];  double atk_k[12];  int n_atk = 0;
    int cov[10];  double cov_k[10];  int n_cov = 0;
    int pas[3];   double pas_k[3];   int n_pas = 0;
    int good_idx = -1, pickup_idx = -1;

    for (int i = 0; i < moves->n; i++) {
        if (excluded[i]) continue;
        const LegalMove *m = &moves->moves[i];
        switch (m->type) {
            case MOVE_ATTACK: {
                int sum = 0;
                for (int j = 0; j < m->n_cards; j++) sum += cd_card_score(m->cards[j], power);
                cd_ranked_insert(atk, atk_k, &n_atk, 12, i,
                                 -(double)m->n_cards * 10000.0 + (double)sum);
                break;
            }
            case MOVE_COVER: {
                double prod = 1.0;
                for (int j = 0; j < m->n_cards; j++) prod *= (double)cd_card_score(m->cards[j], power);
                cd_ranked_insert(cov, cov_k, &n_cov, 10, i,
                                 prod - (double)m->n_cards * 0.5);
                break;
            }
            case MOVE_PASS: {
                int sum = 0;
                for (int j = 0; j < m->n_cards; j++) sum += cd_card_score(m->cards[j], power);
                cd_ranked_insert(pas, pas_k, &n_pas, 3, i, (double)sum);
                break;
            }
            case MOVE_GOOD:   good_idx = i;   break;
            case MOVE_PICKUP: pickup_idx = i; break;
            default: break;
        }
    }

    out->n = 0;
    for (int i = 0; i < n_atk && out->n < CD_MAX_CANDS; i++) out->idx[out->n++] = atk[i];
    for (int i = 0; i < n_cov && out->n < CD_MAX_CANDS; i++) out->idx[out->n++] = cov[i];
    for (int i = 0; i < n_pas && out->n < CD_MAX_CANDS; i++) out->idx[out->n++] = pas[i];
    if (good_idx >= 0 && out->n < CD_MAX_CANDS)   out->idx[out->n++] = good_idx;
    if (pickup_idx >= 0 && out->n < CD_MAX_CANDS) out->idx[out->n++] = pickup_idx;
}

// ---------- main MC ---------------------------------------------------------

// Worlds per decision (W1: all candidates, W2: surviving third, W3: final
// top-2 duel). Compact worlds + early rollout exits buy ~2-3x blackpowder's
// sampling budget at comparable wall-clock.
static void cd_params(int num_players, int *W1, int *W2, int *W3) {
    // ~2x the blackpowder-era budget. The compact bitboard rollout (~4x faster
    // per ply) pays for it: at this budget cordite is still at or below the old
    // wall-clock yet measurably stronger (more sampled worlds). Measured 400
    // games/pc vs handwritten: pc2 1.165/83.5% -> 1.115/88.5%, pc3 1.548->1.510,
    // pc4 2.007->1.990, pc8 4.183/12.2% -> 4.125/15.0%. Doubling again was not
    // reliably better and costs more (esp. at pc2 where the exact solver, not
    // the rollout, dominates), so the budget stops at 2x.
    if (num_players <= 2)      { *W1 = 32; *W2 = 56; *W3 = 56; }
    else if (num_players <= 4) { *W1 = 28; *W2 = 56; *W3 = 56; }
    else if (num_players <= 6) { *W1 = 40; *W2 = 80; *W3 = 56; }
    else                       { *W1 = 40; *W2 = 80; *W3 = 48; }
    // cordite_old: the pre-change 1x budget (half the above). The only
    // strength-affecting change was doubling the budget — the rollout/solver
    // rewrites are exact — so this gives a faithful "cordite before the changes"
    // to play head-to-head against. Research-only.
    if (cd_old_budget) {
        if (num_players <= 2)      { *W1 = 16; *W2 = 28; *W3 = 28; }
        else if (num_players <= 4) { *W1 = 14; *W2 = 28; *W3 = 28; }
        else if (num_players <= 6) { *W1 = 20; *W2 = 40; *W3 = 28; }
        else                       { *W1 = 20; *W2 = 40; *W3 = 24; }
    }
    // CD_BUDGET=prod: the deployed TS v2.4 player-count-aware budget
    // (cordite_core.ts CORDITE_PARAMS — pc2/pc4 saturated at ~3x the arena
    // table, pc6/pc8 variance-starved so ~6x, plus wider keep counts in the
    // pruning stages). CD_BUDGET=max: the cordite_max tier. The TS ran these
    // under a 2s wall-clock cap it almost never hit; the C engine's per-world
    // cost is ~4x lower, so the cap is dropped rather than ported.
    if (cd_budget_mode == 1) {
        if (num_players <= 2)      { *W1 =  96; *W2 = 168; *W3 = 168; }
        else if (num_players <= 4) { *W1 =  84; *W2 = 168; *W3 = 168; }
        else if (num_players <= 6) { *W1 = 240; *W2 = 480; *W3 = 336; }
        else                       { *W1 = 240; *W2 = 480; *W3 = 288; }
    } else if (cd_budget_mode == 2) {
        *W1 = 120; *W2 = 240; *W3 = 168;
    }
    if (cd_w1_override > 0) *W1 = cd_w1_override;
    if (cd_w2_override > 0) *W2 = cd_w2_override;
    if (cd_w3_override >= 0) *W3 = cd_w3_override;
}

// CD_RACE=1: sequential early stop. The stage schedule spends the same world
// budget whether the decision is a coin flip or a landslide; most are
// landslides. Every 16 worlds (after a 32-world floor) compare the two best
// running means over the alive candidates and stop the whole deliberation
// once the leader's gap clears a distance that shrinks like 1/sqrt(n)
// (Hoeffding-flavored; finish positions span ~num_players, and the constant
// is folded into CD_RACE_C, percent, default 100). Close decisions still
// consume the full budget, so quality concentrates where it matters.
// __builtin_sqrt is the native f64.sqrt instruction on wasm — no libm.
static int cd_race_separated(const double *score, const int *nsim,
                             const bool *alive, int n_cands,
                             int worlds_done, int num_players, int race_c) {
    if (worlds_done < 32 || (worlds_done & 15) != 0) return 0;
    double best = 1e30, second = 1e30;
    int n = 0;
    for (int i = 0; i < n_cands; i++) {
        if (!alive[i] || nsim[i] == 0) continue;
        double v = score[i] / (double)nsim[i];
        if (v < best) { second = best; best = v; }
        else if (v < second) second = v;
        if (nsim[i] > n) n = nsim[i];
    }
    if (second >= 1e29) return 1;   // a single live candidate: nothing to race
    return (second - best) * __builtin_sqrt((double)n)
         > (double)race_c * 0.01 * (double)num_players;
}

// CD_VERIFY=1: oracle self-check (test-only — reads real hands to validate
// the public-info belief, never to play).
static void cd_verify_belief(const Game *g, int bot_idx, const Belief *B) {
    for (int p = 0; p < g->num_players; p++) {
        if (p == bot_idx) continue;
        const Player *pl = &g->players[p];
        for (int k = 0; k < B->pinned_n[p]; k++) {
            bool found = false;
            for (int j = 0; j < pl->hand_count; j++) {
                if (card_eq(pl->hand[j], B->pinned[p][k])) { found = true; break; }
            }
            if (!found) {
                fprintf(stderr, "CD_VERIFY: pinned card v%d s%d NOT in p%d hand (logs=%d)\n",
                        B->pinned[p][k].value, B->pinned[p][k].suit, p, g->num_logs);
            }
        }
        for (int j = 0; j < pl->hand_count; j++) {
            if (cd_set_contains(B->pinned[p], B->pinned_n[p], pl->hand[j])) continue;
            if (B->void_n[p] > 0 && cd_void_forbidden(B, g, p, pl->hand[j])) {
                fprintf(stderr, "CD_VERIFY: void violated: p%d holds v%d s%d\n",
                        p, pl->hand[j].value, pl->hand[j].suit);
            }
            if (cd_floor_forbidden(B, g, p, pl->hand[j])) {
                fprintf(stderr, "CD_VERIFY: floor violated: p%d holds v%d s%d floor=%d\n",
                        p, pl->hand[j].value, pl->hand[j].suit, B->floor_v[p]);
            }
            if (!cd_set_contains(B->pool, B->n, pl->hand[j])) {
                fprintf(stderr, "CD_VERIFY: p%d unknown card v%d s%d missing from pool (logs=%d)\n",
                        p, pl->hand[j].value, pl->hand[j].suit, g->num_logs);
            }
        }
    }
}

int cordite_strategy_choose(const Game *g, int bot_idx,
                            const LegalMoves *moves, void *ctx) {
    (void)ctx;
    if (moves->n == 0) return -1;
    if (moves->n == 1) return 0;

    if (!cd_flags_loaded) {
        cd_no_solve  = cd_flag("CD_NO_SOLVE");
        cd_no_voids  = cd_flag("CD_NO_VOIDS");
        cd_no_flip   = cd_flag("CD_NO_FLIP");
        cd_no_floors = cd_flag("CD_NO_FLOORS");
        // Exact leaf endgames in rollouts are OFF by default: measured both
        // slower and weaker vs the real (imperfect) opponent pool — modeling
        // actual opponents beats assuming perfect play. CD_LEAF=1 re-enables.
        cd_no_leaf   = !cd_flag("CD_LEAF");
        cd_no_avoid  = cd_flag("CD_NO_AVOID");
        cd_verify    = cd_flag("CD_VERIFY");
        cd_w1_override = cd_env_int("CD_W1", 0);
        cd_w2_override = cd_env_int("CD_W2", 0);
        cd_w3_override = cd_env_int("CD_W3", -1);
        cd_keep1 = cd_env_int("CD_KEEP1", 0);
        cd_keep2 = cd_env_int("CD_KEEP2", 0);
        cd_race = cd_env_int("CD_RACE", 0);
        cd_race_c = cd_env_int("CD_RACE_C", 100);
        {
            // CD_BUDGET=prod|max: the world/pruning budgets the production TS
            // port shipped with (see cd_params / the keep counts below). The
            // server's bots.wasm adapter sets this; the arena default stays
            // the C-tuned budget.
            const char *bm = getenv("CD_BUDGET");
            cd_budget_mode = (bm && !strcmp(bm, "prod")) ? 1
                           : (bm && !strcmp(bm, "max"))  ? 2 : 0;
        }
        cd_rollout_policy = cd_env_int("CD_ROLLOUT", 0);
        cd_bb_win_budget = cd_env_int("CD_BB_WIN", 20000);
        cd_bb_avoid_budget = cd_env_int("CD_BB_AVOID", 15000);
        cd_leaf_budget = cd_env_int("CD_LEAF_BUDGET", 1500);
        cd_leaf_max_cards = cd_env_int("CD_LEAF_CARDS", 10);
        cd_floor_mod = cd_env_int("CD_FLOOR_MOD", 2);
        if (cd_floor_mod < 1) cd_floor_mod = 1;
        cd_full_logs = cd_flag("CD_FULL_LOGS");
        cd_no_earlyexit = cd_flag("CD_NO_EARLYEXIT");
        cd_no_fastroll = cd_flag("CD_NO_FASTROLL");
        cd_difftest = cd_flag("CD_DIFFTEST");
        if (cd_difftest) { void cd_difftest_report(void); atexit(cd_difftest_report); }
        cd_flags_loaded = 1;
    }

    uint32_t saved_rng = game_rng_get();

    Belief B;
    cd_build_belief(g, bot_idx, &B);
    if (cd_no_voids) for (int p = 0; p < MAX_PLAYERS; p++) B.void_n[p] = 0;
    if (cd_verify) cd_verify_belief(g, bot_idx, &B);

    // Exact endgame: take a proven win; mark proven losses for exclusion.
    static _Thread_local bool forced_loss[MAX_LEGAL_MOVES];
    memset(forced_loss, 0, (size_t)moves->n * sizeof(bool));
    int n_safe = moves->n;
    int solved = cd_no_solve ? -1
               : cd_try_endgame_solve(g, bot_idx, moves, &B, forced_loss, &n_safe);
    if (solved >= 0) {
        game_rng_set(saved_rng);
        return solved;
    }

    Candidates C;
    cd_pick_candidates(g, moves, forced_loss, &C);
    if (C.n == 0) {
        // Everything we'd consider is a proven loss; fall back to all moves.
        memset(forced_loss, 0, (size_t)moves->n * sizeof(bool));
        cd_pick_candidates(g, moves, forced_loss, &C);
    }
    if (C.n == 0) { game_rng_set(saved_rng); return 0; }
    if (C.n == 1) { game_rng_set(saved_rng); return C.idx[0]; }

    int W1, W2, W3;
    cd_params(g->num_players, &W1, &W2, &W3);

    uint32_t base = cd_mix((uint32_t)g->num_logs * 2654435761u,
                           ((uint32_t)g->deck_count << 8)
                           ^ (uint32_t)g->discard_pile_length
                           ^ ((uint32_t)bot_idx << 20));

    double score[CD_MAX_CANDS] = {0};
    int    nsim [CD_MAX_CANDS] = {0};
    bool   alive[CD_MAX_CANDS];
    for (int i = 0; i < C.n; i++) alive[i] = true;

    static _Thread_local Game world, trial;
    static _Thread_local SimState world_sim, trial_sim;

    // The fast bitboard path: convert each sampled WORLD to a compact SimState
    // ONCE, then each candidate just clones the SimState, applies its move on
    // bitboards, and plays out. The struct path (CD_NO_FASTROLL / CD_LEAF /
    // CD_DIFFTEST) keeps the per-candidate Game clone for the leaf solver and
    // the exact-equivalence difftest.
    bool fast_path = !cd_no_fastroll && cd_no_leaf && !cd_difftest && !cd_flag("CD_NO_WORLDSIM");

    // Stage 1: all candidates on W1 shared worlds.
    // Stage 2: surviving third on W2 more shared worlds.
    // Stage 3: top 2 duel on W3 final shared worlds.
    for (int stage = 0; stage < 3; stage++) {
        int w_lo = (stage == 0) ? 0 : (stage == 1) ? W1 : W1 + W2;
        int w_hi = (stage == 0) ? W1 : (stage == 1) ? W1 + W2 : W1 + W2 + W3;
        for (int w = w_lo; w < w_hi; w++) {
            uint32_t wseed = cd_mix(base, (uint32_t)(w + 1) * 0x85EBCA77u);
            // Belief mixture: voids assume cover-if-you-can pickups (3 of 4
            // worlds), floors assume lowest-first attackers (every other
            // world). Per-player distrust already cleared bogus constraints.
            bool use_voids  = (w & 3) != 3;
            bool use_floors = !cd_no_floors && (w % cd_floor_mod) == 0;
            cd_sample_world(&world, g, bot_idx, &B, wseed, use_voids, use_floors);
            // fulminate: sample this world's per-seat rollout policies from
            // the installed posterior weights (cordite_core.ts:2749-2751),
            // seeded by the WORLD seed so the table is shared by every
            // candidate in this world (preserves CRN) and independent of the
            // rollout RNG stream. Zero work when no weights are installed —
            // cordite's exact path.
            cd_seat_policy_dev = cd_seat_weights_on
                               ? cd_fm_sample_table(wseed, g->num_players) : 0;
            uint32_t sim_rng = cd_mix(wseed, 0x51AB1E5u);

            if (fast_path && !cd_seat_policy_dev) {
                cd_sim_from_game(&world_sim, &world);   // convert world ONCE
                for (int ci = 0; ci < C.n; ci++) {
                    if (!alive[ci]) continue;
                    trial_sim = world_sim;              // cheap struct copy
                    game_rng_set(sim_rng);              // identical stream
                    int fp;
                    if (!cd_sim_apply_root_move(&trial_sim, bot_idx,
                                                &moves->moves[C.idx[ci]])) {
                        fp = g->num_players;
                    } else {
                        fp = cd_sim_playout(&trial_sim, bot_idx, 600, !cd_no_earlyexit);
                        if (fp == 0) fp = g->num_players;
                    }
                    score[ci] += (double)fp;
                    nsim[ci]++;
                }
                if (cd_race && cd_race_separated(score, nsim, alive, C.n,
                                                 w + 1, g->num_players, cd_race_c))
                    goto race_done;
                continue;
            }

            for (int ci = 0; ci < C.n; ci++) {
                if (!alive[ci]) continue;
                cd_lite_clone(&trial, &world);
                game_rng_set(sim_rng);   // identical stream for every move
                if (!cd_apply(&trial, bot_idx, &moves->moves[C.idx[ci]])) {
                    score[ci] += (double)g->num_players;
                    nsim[ci]++;
                    continue;
                }
                // A world whose sampled policy table deviates must roll out on
                // the struct engine (the archetype hook lives in cd_simulate;
                // the bitboard playout has no move list to choose from). It
                // also bypasses cd_rollout's CD_DIFFTEST comparison — the
                // difftest is only meaningful with the override off.
                int fp = cd_seat_policy_dev
                       ? cd_simulate(&trial, bot_idx, 600)
                       : cd_rollout(&trial, bot_idx, 600);
                if (fp == 0) fp = g->num_players;
                score[ci] += (double)fp;
                nsim[ci]++;
            }
            if (cd_race && cd_race_separated(score, nsim, alive, C.n,
                                             w + 1, g->num_players, cd_race_c))
                goto race_done;
        }
        if (stage < 2) {
            int n_alive = 0;
            for (int i = 0; i < C.n; i++) if (alive[i]) n_alive++;
            int keep;
            if (stage == 0) {
                // Budget modes use the TS scheme: keep max(4, ceil(n/2))
                // after stage 0 and 3 after stage 1 — wider survival for the
                // larger world budget (see cordite_core.ts corditeChoose).
                keep = (cd_keep1 > 0) ? cd_keep1
                     : cd_budget_mode ? (C.n + 1) / 2
                     : C.n / 3;
                int keep_floor = cd_budget_mode ? 4 : 3;
                if (keep < keep_floor) keep = keep_floor;
            } else {
                keep = (cd_keep2 > 0) ? cd_keep2 : (cd_budget_mode ? 3 : 2);
            }
            if (keep >= n_alive) continue;
            for (int dropped = n_alive - keep; dropped > 0; dropped--) {
                int worst = -1;
                double worst_v = -1e30;
                for (int i = 0; i < C.n; i++) {
                    if (!alive[i]) continue;
                    double v = score[i] / (double)(nsim[i] ? nsim[i] : 1);
                    // >= : among tied scores drop the LAST candidate. The
                    // candidate list is ranked cheapest-first, so this keeps
                    // the cheap move on ties; dropping the first-tied instead
                    // systematically burns trumps (measured ~0.1 mean finish
                    // vs blackpowder tables at pc4).
                    if (v >= worst_v) { worst_v = v; worst = i; }
                }
                if (worst < 0) break;
                alive[worst] = false;
            }
        }
    }

race_done:;
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

// "cordite before the changes": same engine (fast + exact) but the pre-2x world
// budget, so head-to-head eval isolates whether the budget change actually wins.
int cordite_old_strategy_choose(const Game *g, int bot_idx,
                                const LegalMoves *moves, void *ctx) {
    cd_old_budget = 1;
    int r = cordite_strategy_choose(g, bot_idx, moves, ctx);
    cd_old_budget = 0;
    return r;
}
