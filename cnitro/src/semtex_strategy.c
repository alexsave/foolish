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

#include "semtex_strategy.h"
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

static inline int sx_card_score(Card c, int power) {
    return c.value + (c.suit == power ? 1000 : 0);
}

static bool sx_set_contains(const Card *arr, int n, Card c) {
    for (int i = 0; i < n; i++) if (card_eq(arr[i], c)) return true;
    return false;
}

static uint32_t sx_xorshift(uint32_t s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s ? s : 0xB1A570u;
}

static uint32_t sx_mix(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u);
    h ^= h >> 16; h *= 0x85EBCA77u; h ^= h >> 13;
    return h ? h : 1;
}

// Ablation switches (read once): SX_NO_SOLVE / SX_NO_VOIDS / SX_NO_FLIP /
// SX_NO_FLOORS / SX_NO_LEAF / SX_NO_AVOID / SX_VERIFY, plus SX_W1/SX_W2
// world-count overrides for tuning.
static int sx_flag(const char *name) {
    const char *v = getenv(name);
    return v && v[0] && v[0] != '0';
}
static int sx_env_int(const char *name, int def) {
    const char *v = getenv(name);
    return (v && v[0]) ? atoi(v) : def;
}
static _Thread_local int sx_flags_loaded = 0;
static _Thread_local int sx_no_solve = 0, sx_no_voids = 0, sx_no_flip = 0;
static _Thread_local int sx_no_floors = 0, sx_no_leaf = 0, sx_no_avoid = 0;
static _Thread_local int sx_no_earlyexit = 0;
static _Thread_local int sx_verify = 0;
static _Thread_local int sx_no_fastroll = 0;   // SX_NO_FASTROLL=1: struct rollout
// Bitboard exact-leaf endgames inside rollouts (semtex's own lever): resolve
// small 2-player deck-empty rollout endgames with the fast bitboard solver
// instead of handwritten policy play. Against opponents that themselves play
// endgames exactly (cordite), the exact model is the realistic one.
// SX_BBLEAF: 2 (default) = pc-aware exact leaf endgames in rollouts —
// small (SX_BBLEAF_CARDS2, 8) leaves heads-up only; SX_BBLEAF_CARDS
// (default 0 = off) at 3+ players. Loss analysis showed 12-card leaves at
// 3+ inject "the endgame will be played perfectly" into mid-game values —
// individually terrible calls (trump-burning covers, passive pickups) that
// exactly cancel the good calls (paired mirror delta ~0) while costing 3x
// wall-clock; the replicated win is the heads-up leaf. 1 = SX_BBLEAF_CARDS
// everywhere; 0 = off.
static _Thread_local int sx_bbleaf = 2;
static _Thread_local int sx_bbleaf_cards2 = 8;
// SX_ADAPT: void-contradiction => per-seat distrust of floors+voids (on by
// default — pure evidence, no downside). SX_PROFILE: weak-seat detection +
// LOOSE rollout model for profiled seats.
static _Thread_local int sx_adapt = 1;
// Void world-mixture: voids applied in (mod-1)/mod of sampled worlds
// (cordite: 3 of 4). A softer mixture hedges between heuristic-family
// opponents (voids true) and MC/human strategic pickups (voids misleading).
static _Thread_local int sx_void_mod = 4;
static _Thread_local int sx_profile = 0;
// Root endgame-solve card ceiling (cordite: 20). The bitboard solver + TT can
// resolve bigger endgames within budget; a higher ceiling opens a window where
// semtex plays exactly while cordite still guesses with MC.
static _Thread_local int sx_solve_cards = 20;
static _Thread_local int sx_bbleaf_cards = 12;
static _Thread_local long sx_bbleaf_budget = 3000;
// Per-seat rollout policy map for the current decision (NULL = all
// handwritten). Set by semtex_strategy_choose when profiling flags seats.
static _Thread_local uint8_t sx_polmap_buf[MAX_PLAYERS];
static _Thread_local const uint8_t *sx_polmap = NULL;
static _Thread_local int sx_bbleaf_on = 0;   // effective flag for this decision
static _Thread_local int sx_bbleaf_cards_eff = 12;
static _Thread_local int sx_difftest = 0;      // SX_DIFFTEST=1: assert fast==slow
static _Thread_local int sx_w1_override = 0, sx_w2_override = 0;
static _Thread_local int sx_w3_override = -1;
static _Thread_local int sx_old_budget = 0;    // cordite_old variant: pre-2x worlds
static _Thread_local int sx_keep1 = 0, sx_keep2 = 0;  // SX_KEEP1/2: candidates kept past stage 0/1 (0=default n/3, 2)
// Oracle mode (research): multiply the world budget and widen candidate
// survival for one call. Used by semtex_oracle_strategy_choose to audit
// loss games — a decision the oracle changes was compute-limited.
static _Thread_local int sx_oracle = 0;
static _Thread_local int sx_rollout_policy = 0;       // SX_ROLLOUT: 0=default, 1=espresso, 2=handwritten (struct path)
// Bitboard endgame-solver node budgets (per shared pass). The bitboard solver
// (transposition table + O(1) clone) resolves far more per node than the
// struct solver, so it needs a much smaller node budget to do equivalent work
// in less wall-clock. Tunable via SX_BB_WIN / SX_BB_AVOID for sweeps.
static _Thread_local int sx_bb_win_budget = 20000;
static _Thread_local int sx_bb_avoid_budget = 15000;

static int sx_in_count(const Game *g) {
    int n = 0;
    for (int i = 0; i < g->num_players; i++)
        if (g->players[i].status == PLAYER_STATUS_IN) n++;
    return n;
}

static bool sx_apply(Game *g, int p_idx, const LegalMove *m) {
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
static void sx_lite_clone(Game *dst, const Game *src) {
    size_t base = offsetof(Game, logs);
    memcpy(dst, src, base + (size_t)src->num_logs * sizeof(GameLog));
}

// ---------- belief state ------------------------------------------------

#define SX_MAX_VOIDS 6

typedef struct {
    Card pool[80];                  // unseen pool (deck ∪ opp unknowns)
    int  n;
    Card pinned[MAX_PLAYERS][MAX_HAND_SIZE];  // publicly located in p's hand
    int  pinned_n[MAX_PLAYERS];
    // Hard-ish void constraints (see blackpowder): attack cards a defender
    // demonstrably couldn't cover at pickup time. Cleared on their draw.
    Card voids[MAX_PLAYERS][SX_MAX_VOIDS];
    int  void_n[MAX_PLAYERS];
    // Soft rank floor: lowest non-trump value p can hold (0 = none). Set by
    // a single-card non-trump first attack at 3+ players in (lowest-first
    // attacker policies reveal their minimum). Cleared when p gains cards.
    int  floor_v[MAX_PLAYERS];
    // Trust flags, from observed behavior this game.
    bool distrust_floor[MAX_PLAYERS];
    bool distrust_void[MAX_PLAYERS];
    // Per-seat behavior profile from the public log (semtex additions).
    int  cards_played[MAX_PLAYERS];   // cards played while the deck was alive
    int  trumps_played[MAX_PLAYERS];  // of those, trumps
    int  trump_leads[MAX_PLAYERS];    // trump attacks while the deck was alive
    bool mc_tell[MAX_PLAYERS];        // proven void contradiction: strategic
                                      // pickup-while-holding-cover (MC/human)
    bool loose[MAX_PLAYERS];          // profiled weak/random seat
} Belief;

static bool sx_void_forbidden(const Belief *B, const Game *g, int p, Card c) {
    for (int k = 0; k < B->void_n[p]; k++) {
        if (can_cover(B->voids[p][k], c, g->power_suit)) return true;
    }
    return false;
}

static bool sx_floor_forbidden(const Belief *B, const Game *g, int p, Card c) {
    return B->floor_v[p] > 0 && c.suit != g->power_suit && c.value < B->floor_v[p];
}

static void sx_pinned_remove(Belief *B, int p, Card c) {
    for (int q = 0; q < B->pinned_n[p]; q++) {
        if (card_eq(B->pinned[p][q], c)) {
            B->pinned[p][q] = B->pinned[p][B->pinned_n[p] - 1];
            B->pinned_n[p]--;
            return;
        }
    }
}

static void sx_pinned_add(Belief *B, int p, Card c) {
    if (B->pinned_n[p] >= MAX_HAND_SIZE) return;
    if (sx_set_contains(B->pinned[p], B->pinned_n[p], c)) return;
    B->pinned[p][B->pinned_n[p]++] = c;
}

// A floor contradiction (p plays a non-trump card below their inferred
// floor) means p is not a lowest-first attacker: distrust their floors.
static void sx_floor_check(Belief *B, const Game *g, int p, Card c) {
    if (p < 0 || B->floor_v[p] <= 0) return;
    if (c.suit != g->power_suit && c.value < B->floor_v[p]) {
        B->floor_v[p] = 0;
        B->distrust_floor[p] = true;
    }
}

// Chronological scan over logs: pinned cards, flipped-trump holder, void
// constraints, rank floors and trust flags, all in one pass.
static void sx_build_belief(const Game *g, int bot_idx, Belief *B) {
    memset(B, 0, sizeof(*B));

    // Last draw event: holds the flipped trump if the deck is exhausted, and
    // marks the moment the deck died (for "deck alive at log i" tests).
    int last_draw_idx = -1;
    for (int i = 0; i < g->num_logs; i++) {
        if (g->logs[i].log_type == LOG_DRAW) last_draw_idx = i;
    }
    bool deck_alive_now = (g->deck_count > 0 || g->has_flipped);
    int flip_log_idx = (!deck_alive_now && !sx_no_flip) ? last_draw_idx : -1;

    int trump_viol[MAX_PLAYERS] = {0};
    int in_now = g->num_players;

    // Declined-attack tell state: per-seat bitmask of non-trump values the
    // seat said GOOD over (while the defender had spare capacity). A
    // handwritten-family attacker NEVER declines a legal non-trump attack, so
    // later playing a non-trump card of a declined value (before gaining
    // cards) proves the seat attacks strategically — an MC bot or a thinking
    // human. Cleared on draw/pickup (the played card might be a new one).
    uint16_t decl_vals[MAX_PLAYERS] = {0};
    int hand_n[MAX_PLAYERS];
    for (int i = 0; i < g->num_players; i++) hand_n[i] = CARDS_PER_PLAYER;
    int cur_def = -1;

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
                if (p >= 0) hand_n[p] -= L->num_pairs;
                if (first_attack && cur_def < 0 && p >= 0) {
                    cur_def = (p + 1) % g->num_players;   // pre-change rounds
                }
                bool any_trump = false;
                for (int k = 0; k < L->num_pairs; k++) {
                    Card c = L->pairs[k].primary;
                    if (c.suit == g->power_suit) any_trump = true;
                    if (unc_n < (int)(sizeof(unc) / sizeof(unc[0]))) unc[unc_n++] = c;
                    if (tbl_n < (int)(sizeof(tbl) / sizeof(tbl[0]))) tbl[tbl_n++] = c;
                    if (p >= 0 && p != bot_idx) {
                        if (deck_alive_at) {
                            B->cards_played[p]++;
                            if (c.suit == g->power_suit) B->trumps_played[p]++;
                        }
                        // Playing a card a still-active void says they lacked
                        // (and that wasn't publicly picked up) proves the void
                        // wrong: the seat picked up strategically while
                        // holding cover. Handwritten-family never does that.
                        if (!sx_set_contains(B->pinned[p], B->pinned_n[p], c)
                            && sx_void_forbidden(B, g, p, c)) B->mc_tell[p] = true;
                        if (c.suit != g->power_suit
                            && (decl_vals[p] & (1u << c.value))) B->mc_tell[p] = true;
                        sx_floor_check(B, g, p, c);
                        sx_pinned_remove(B, p, c);
                    }
                }
                if (p >= 0 && p != bot_idx && L->log_type == LOG_ATTACK) {
                    if (any_trump && deck_alive_at) trump_viol[p]++;
                    if (first_attack && L->num_pairs == 1 && !any_trump
                        && in_now > 2 && !sx_no_floors) {
                        B->floor_v[p] = L->pairs[0].primary.value;
                    }
                }
                break;
            }
            case LOG_COVER:
                if (p >= 0) hand_n[p] -= L->num_pairs;
                for (int k = 0; k < L->num_pairs; k++) {
                    Card c = L->pairs[k].primary;
                    if (tbl_n < (int)(sizeof(tbl) / sizeof(tbl[0]))) tbl[tbl_n++] = c;
                    if (p >= 0 && p != bot_idx) {
                        if (deck_alive_at) {
                            B->cards_played[p]++;
                            if (c.suit == g->power_suit) B->trumps_played[p]++;
                        }
                        if (!sx_set_contains(B->pinned[p], B->pinned_n[p], c)
                            && sx_void_forbidden(B, g, p, c)) B->mc_tell[p] = true;
                        if (c.suit != g->power_suit
                            && (decl_vals[p] & (1u << c.value))) B->mc_tell[p] = true;
                        sx_floor_check(B, g, p, c);
                        sx_pinned_remove(B, p, c);
                    }
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
            case LOG_GOOD:
                if (p >= 0 && p != bot_idx && in_now > 2 && cur_def >= 0
                    && hand_n[cur_def] - unc_n >= 1) {
                    for (int k = 0; k < tbl_n; k++) {
                        if (tbl[k].suit != g->power_suit)
                            decl_vals[p] |= (uint16_t)(1u << tbl[k].value);
                    }
                }
                break;
            case LOG_DEFENDER_CHANGE:
                cur_def = L->defender_index;
                break;
            case LOG_PICKUP:
                if (p >= 0) { hand_n[p] += tbl_n; decl_vals[p] = 0; }
                if (p >= 0 && p != bot_idx) {
                    // Exactly one uncovered attack => defender held no cover.
                    if (unc_n == 1 && B->void_n[p] < SX_MAX_VOIDS) {
                        B->voids[p][B->void_n[p]++] = unc[0];
                    }
                    for (int k = 0; k < tbl_n; k++) sx_pinned_add(B, p, tbl[k]);
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
                if (p >= 0) { hand_n[p] += L->num_pairs; decl_vals[p] = 0; }
                if (p >= 0 && p != bot_idx) {
                    B->void_n[p] = 0;    // new unknown cards: constraints expire
                    B->floor_v[p] = 0;
                    if (i == flip_log_idx) sx_pinned_add(B, p, g->flipped);
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
        B->trump_leads[p] = trump_viol[p];
        if (trump_viol[p] >= 1) { B->distrust_floor[p] = true; B->floor_v[p] = 0; }
    }

    // Semtex adaptivity: a proven void contradiction means the seat picks up
    // strategically while holding cover — an MC bot (cordite family) or a
    // thinking human. Both floor and void inference model heuristic-family
    // opponents and are wrong for such seats; drop them for the rest of the
    // game (no draw-expiry reset — the tell is about the PLAYER, not the hand).
    if (sx_adapt) {
        for (int p = 0; p < g->num_players; p++) {
            if (!B->mc_tell[p]) continue;
            B->void_n[p] = 0;
            B->floor_v[p] = 0;
            B->distrust_void[p] = true;
            B->distrust_floor[p] = true;
        }
    }

    // Semtex weak-seat profile (fulminate's lever, conservatively gated): a
    // seat that burns trumps while the deck is alive at a rate no strong or
    // heuristic bot exhibits is rolled out with the LOOSE model instead of
    // handwritten. Evidence: deck-alive trump share ramped over [0.40, 0.60]
    // (needs >= 14 observed cards), plus repeated deck-alive trump attacks.
    if (sx_profile) {
        for (int p = 0; p < g->num_players; p++) {
            if (p == bot_idx) continue;
            double conf = 0.0;
            if (B->cards_played[p] >= 14) {
                double r = (double)B->trumps_played[p] / (double)B->cards_played[p];
                double ramp = (r - 0.40) / 0.20;
                if (ramp < 0) ramp = 0;
                if (ramp > 1) ramp = 1;
                conf += ramp;
            }
            int leads = B->trump_leads[p] > 3 ? 3 : B->trump_leads[p];
            conf += 0.25 * leads;
            if (conf >= 0.70) {
                B->loose[p] = true;
                B->void_n[p] = 0;    // weak seats don't obey cover-if-you-can
                B->floor_v[p] = 0;   // ...or lowest-first attacks
            }
        }
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
            if (!sx_set_contains(known, kn, c)) B->pool[B->n++] = c;
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
            if (!sx_void_forbidden(B, g, p, B->pool[i])
                && !sx_floor_forbidden(B, g, p, B->pool[i])) allowed++;
        }
        if (allowed < unknown && B->floor_v[p] > 0) {
            B->floor_v[p] = 0;
            allowed = 0;
            for (int i = 0; i < B->n; i++) {
                if (!sx_void_forbidden(B, g, p, B->pool[i])) allowed++;
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
static _Thread_local int sx_full_logs = 0;   // SX_FULL_LOGS=1: bp-style worlds

static void sx_sample_world(Game *g_out, const Game *g_in, int my_idx,
                            const Belief *B, uint32_t seed,
                            bool apply_voids, bool apply_floors) {
    if (sx_full_logs) {
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
        s = sx_xorshift(s);
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
        bool bad = (use_v && sx_void_forbidden(B, g_in, p, c))
                || (use_f && sx_floor_forbidden(B, g_in, p, c));
        if (!bad) continue;
        for (int d = 0; d < deck_n; d++) {
            Card dc = g_out->deck[d];
            bool dc_bad = (use_v && sx_void_forbidden(B, g_in, p, dc))
                       || (use_f && sx_floor_forbidden(B, g_in, p, dc));
            if (!dc_bad) {
                g_out->deck[d] = c;
                g_out->players[p].hand[slots[si].slot] = dc;
                break;
            }
        }
    }
}

// ---------- exact solver (shared by root + rollout leaves) -----------------

#define SX_SOLVE_MAX_DEPTH   48
#define SX_SOLVE_MAX_MOVES   96
#define SX_SOLVE_BUDGET      200000L
#define SX_AVOID_BUDGET      150000L
#define SX_SOLVE_MAX_CARDS   20
#define SX_LEAF_BUDGET       1500L

typedef struct {
    long budget;
    bool aborted;
    int  me;
    SolveMoves *mv;      // [SX_SOLVE_MAX_DEPTH]
} Solver;

static _Thread_local SolveMoves *sx_solver_mv = NULL;

_Static_assert(SX_SOLVE_MAX_DEPTH <= SOLVE_SCRATCH_DEPTH,
               "shared solver scratch shallower than this family's depth");
static bool sx_solver_ready(void) {
    if (!sx_solver_mv) {
        sx_solver_mv = solve_scratch_mv();
    }
    return sx_solver_mv != NULL;
}

// Value in [-1000, 1000] from `me`'s perspective: positive = me escaping,
// negative = me as durak. Magnitude prefers faster wins / slower losses.
static int sx_solve(Solver *S, const Game *g, int alpha, int beta, int depth) {
    int loser = game_done(g);
    if (loser >= 0) return (loser == S->me) ? -(1000 - depth) : (1000 - depth);
    if (sx_in_count(g) == 0) return 0;   // defensive: simultaneous out
    if (depth >= SX_SOLVE_MAX_DEPTH) { S->aborted = true; return 0; }
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
    if (mv->n > SX_SOLVE_MAX_MOVES) { S->aborted = true; return 0; }

    bool maximizing = (actor == S->me);
    int best = maximizing ? -2000 : 2000;
    for (int i = 0; i < mv->n; i++) {
        Game *child = solve_scratch_child(depth);
        solve_clone_prefix(child, g);
        if (!sx_apply(child, actor, &mv->moves[i])) continue;
        int v = sx_solve(S, child, alpha, beta, depth + 1);
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


// Exact full-info resolution of a rollout leaf: 2 players in, deck gone,
// few cards. Returns the loser index, or -1 if unresolved (abort/draw).
static _Thread_local long sx_leaf_budget = SX_LEAF_BUDGET;
static _Thread_local int  sx_leaf_max_cards = 0;   // set from env at init
static _Thread_local int  sx_floor_mod = 2;        // floors in 1/mod worlds

static int sx_leaf_solve(const Game *g) {
    if (!sx_solver_ready()) return -1;
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
    sx_lite_clone(&root, g);
    root.num_logs = 0;   // solver never reads history

    Solver S;
    S.budget  = sx_leaf_budget;
    S.aborted = false;
    S.me      = me;
    S.mv      = sx_solver_mv;

    // Null window around 0: only the sign matters (true values are ±(1000-d)
    // for decided games), and the narrow window maximizes pruning.
    int v = sx_solve(&S, &root, -1, 1, 0);
    if (S.aborted || v == 0) return -1;
    return (v > 0) ? opp : me;
}

// ---------- simulation ---------------------------------------------------

// Stage-aware rollout policy (gunpowder's rule): handwritten while the deck
// is alive or the game is heads-up, espresso for multi-player endgames.
static StrategyFn sx_rollout_for(const Game *g) {
    // SX_ROLLOUT (struct path only): 1 = espresso everywhere, 2 = handwritten
    // everywhere. Research knob for the "rollout-policy bias" hypothesis — vs a
    // strong opponent, a weak (handwritten) rollout policy biases value
    // estimates, so more worlds saturates. A stronger rollout policy may reduce
    // that bias. Run with SX_NO_FASTROLL=1.
    if (sx_rollout_policy == 1) return espresso_strategy_choose;
    if (sx_rollout_policy == 2) return handwritten_strategy_choose;
    bool deck_active = (g->deck_count > 0 || g->has_flipped);
    if (deck_active || sx_in_count(g) == 2) return handwritten_strategy_choose;
    return espresso_strategy_choose;
}

// Roll a sampled world forward; returns my finish position (1..N), or 0 if
// the simulation didn't terminate. Exits early once my position is known,
// and resolves small 2-player deck-empty endgames exactly (one attempt per
// rollout — a failed solve falls back to policy play for good).
static int sx_simulate(Game *g, int my_idx, int max_turns) {
    int turns = 0;
    bool leaf_tried = false;
    while (game_done(g) < 0 && turns++ < max_turns) {
        // My fate is sealed as soon as I'm out: position = elimination slot.
        if (!sx_no_earlyexit
            && g->players[my_idx].status != PLAYER_STATUS_IN) {
            for (int i = 0; i < g->num_eliminated; i++) {
                if (g->elimination_order[i] == my_idx) return i + 1;
            }
            break;   // not IN and not eliminated: corrupt state, bail
        }

        if (!sx_no_leaf && !leaf_tried && g->deck_count == 0 && !g->has_flipped
            && sx_in_count(g) == 2) {
            int total = 0;
            for (int i = 0; i < g->num_players; i++) {
                if (g->players[i].status == PLAYER_STATUS_IN)
                    total += g->players[i].hand_count;
            }
            if (total <= sx_leaf_max_cards) {
                leaf_tried = true;
                int loser = sx_leaf_solve(g);
                if (loser >= 0) {
                    // 2 left => positions N-1 and N remain.
                    return (loser == my_idx) ? g->num_players : g->num_players - 1;
                }
            }
        }

        bool acted = false;
        for (int pi = 0; pi < g->num_players; pi++) {
            if (!should_bot_act(g, pi)) continue;
            LegalMoves moves;
            calculate_legal_moves_lite(g, pi, &moves);
            if (moves.n == 0) continue;
            StrategyFn fn = sx_rollout_for(g);
            int idx = fn(g, pi, &moves, NULL);
            if (idx < 0 || idx >= moves.n) continue;
            if (sx_apply(g, pi, &moves.moves[idx])) { acted = true; break; }
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
// and play it out on bitmasks. ~10x faster per ply than sx_simulate. The
// effective rollout policy is always handwritten (see cordite_sim.c), and the
// exact leaf solver (SX_LEAF, off by default) is not used in the fast path.
static int sx_simulate_fast(const Game *g, int my_idx, int max_turns) {
    SimState s;
    cd_sim_from_game(&s, g);
    if (sx_bbleaf_on || sx_polmap)
        return cd_sim_playout_pol(&s, my_idx, max_turns, !sx_no_earlyexit,
                                  sx_bbleaf_on ? sx_bbleaf_cards_eff : 0,
                                  sx_bbleaf_budget, sx_polmap);
    return cd_sim_playout(&s, my_idx, max_turns, !sx_no_earlyexit);
}

// Dispatcher: bitboard rollout by default; struct rollout under SX_NO_FASTROLL
// or SX_LEAF (the leaf solver lives on the struct path). SX_DIFFTEST runs both
// and tallies divergences (printed at process exit by the eval harness if it
// hooks sx_difftest_report, else just counted).
static _Thread_local long sx_diff_total = 0, sx_diff_mismatch = 0;
static int sx_rollout(Game *g, int my_idx, int max_turns) {
    if (sx_no_fastroll || !sx_no_leaf) return sx_simulate(g, my_idx, max_turns);
    if (sx_difftest) {
        uint32_t rng0 = game_rng_get();
        int fast = sx_simulate_fast(g, my_idx, max_turns);
        game_rng_set(rng0);
        Game slow_g;
        sx_lite_clone(&slow_g, g);
        int slow = sx_simulate(&slow_g, my_idx, max_turns);
        sx_diff_total++;
        if (fast != slow) {
            sx_diff_mismatch++;
            if (sx_diff_mismatch <= 20) {
                fprintf(stderr, "SX_DIFFTEST mismatch #%ld: fast=%d slow=%d "
                        "(np=%d deck=%d logs=%d)\n", sx_diff_mismatch, fast, slow,
                        g->num_players, g->deck_count, g->num_logs);
            }
        }
        return slow;  // keep slow behavior while difftesting
    }
    return sx_simulate_fast(g, my_idx, max_turns);
}

void sx_difftest_report(void) {
    if (sx_diff_total > 0) {
        fprintf(stderr, "SX_DIFFTEST: %ld/%ld rollouts diverged (%.3f%%)\n",
                sx_diff_mismatch, sx_diff_total,
                100.0 * (double)sx_diff_mismatch / (double)sx_diff_total);
    }
}

// ---------- root endgame solve (win take + loss avoid) ---------------------

// Solve every root move with a full window when 2 players remain and the
// deck is empty (the unseen pool IS the opponent's hand — public deduction).
// Returns the fastest forced-win index, or -1. When no win exists, sets
// forced_loss[i] for root moves that lose under optimal play; the MC stage
// avoids them whenever at least one non-losing move exists.
static int sx_try_endgame_solve(const Game *g, int bot_idx,
                                const LegalMoves *moves, const Belief *B,
                                bool *forced_loss, int *n_safe) {
    *n_safe = moves->n;
    if (g->deck_count > 0 || g->has_flipped) return -1;
    if (sx_in_count(g) != 2) return -1;
    if (g->players[bot_idx].status != PLAYER_STATUS_IN) return -1;

    int opp = -1;
    for (int i = 0; i < g->num_players; i++) {
        if (i != bot_idx && g->players[i].status == PLAYER_STATUS_IN) opp = i;
    }
    if (opp < 0) return -1;

    int unknown = g->players[opp].hand_count - B->pinned_n[opp];
    if (unknown < 0 || unknown != B->n) return -1;  // deduction failed; bail

    int total = g->players[bot_idx].hand_count + g->players[opp].hand_count;
    if (total > sx_solve_cards) return -1;

    if (!sx_solver_ready()) return -1;

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
    // SX_NO_BBSOLVE=1 falls back to the struct solver for A/B.
    bool bbsolve = !sx_flag("SX_NO_BBSOLVE");
    SimState root_sim;
    if (bbsolve) cd_sim_from_game(&root_sim, &root);
    cd_sim_solve_reset();

    Solver S;
    S.budget  = SX_SOLVE_BUDGET;
    S.aborted = false;
    S.me      = bot_idx;
    S.mv      = sx_solver_mv;
    long win_budget   = bbsolve ? (long)sx_bb_win_budget   : SX_SOLVE_BUDGET;
    long avoid_budget = bbsolve ? (long)sx_bb_avoid_budget : SX_AVOID_BUDGET;
    long budget = win_budget;

    // Pass 1 — win hunt (blackpowder's loop): fail-soft with an accumulating
    // alpha floor at 0, so losing subtrees prune immediately.
    // SX_BP_SOLVE=1 reverts to blackpowder's exact semantics (alpha starts
    // wide open, any abort bails the whole solve) for A/B testing.
    int best_idx = -1;
    int best_v = 0;       // only accept strictly winning lines
    int alpha = sx_flag("SX_BP_SOLVE") ? -2000 : 0;
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
            sx_lite_clone(&child, &root);
            if (!sx_apply(&child, bot_idx, &moves->moves[i])) continue;
            S.aborted = false;
            v = sx_solve(&S, &child, alpha, 2000, 1);
            aborted_i = S.aborted;
            if (S.budget <= 0) return -1;
        }
        if (aborted_i) { if (bail_on_abort) return -1; any_abort = true; continue; }
        if (v > best_v) { best_v = v; best_idx = i; }
        if (v > alpha) alpha = v;
    }
    if (best_idx >= 0) return best_idx;
    if (sx_no_avoid || any_abort) return -1;

    // Pass 2 — loss avoidance: no win exists, so classify each move with a
    // null window around 0 (sign only, maximal pruning).
    S.budget = SX_AVOID_BUDGET;
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
            sx_lite_clone(&child, &root);
            if (!sx_apply(&child, bot_idx, &moves->moves[i])) continue;
            S.aborted = false;
            v = sx_solve(&S, &child, -1, 0, 1);
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

#define SX_MAX_CANDS 26

typedef struct {
    int idx[SX_MAX_CANDS];
    int n;
} Candidates;

static void sx_ranked_insert(int *idxs, double *keys, int *n, int cap,
                             int idx, double key) {
    int pos = *n;
    while (pos > 0 && keys[pos - 1] > key) pos--;
    if (pos >= cap) return;
    int last = (*n < cap) ? *n : cap - 1;
    for (int i = last; i > pos; i--) { idxs[i] = idxs[i - 1]; keys[i] = keys[i - 1]; }
    idxs[pos] = idx; keys[pos] = key;
    if (*n < cap) (*n)++;
}

static void sx_pick_candidates(const Game *g, const LegalMoves *moves,
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
                for (int j = 0; j < m->n_cards; j++) sum += sx_card_score(m->cards[j], power);
                sx_ranked_insert(atk, atk_k, &n_atk, 12, i,
                                 -(double)m->n_cards * 10000.0 + (double)sum);
                break;
            }
            case MOVE_COVER: {
                double prod = 1.0;
                for (int j = 0; j < m->n_cards; j++) prod *= (double)sx_card_score(m->cards[j], power);
                sx_ranked_insert(cov, cov_k, &n_cov, 10, i,
                                 prod - (double)m->n_cards * 0.5);
                break;
            }
            case MOVE_PASS: {
                int sum = 0;
                for (int j = 0; j < m->n_cards; j++) sum += sx_card_score(m->cards[j], power);
                sx_ranked_insert(pas, pas_k, &n_pas, 3, i, (double)sum);
                break;
            }
            case MOVE_GOOD:   good_idx = i;   break;
            case MOVE_PICKUP: pickup_idx = i; break;
            default: break;
        }
    }

    out->n = 0;
    for (int i = 0; i < n_atk && out->n < SX_MAX_CANDS; i++) out->idx[out->n++] = atk[i];
    for (int i = 0; i < n_cov && out->n < SX_MAX_CANDS; i++) out->idx[out->n++] = cov[i];
    for (int i = 0; i < n_pas && out->n < SX_MAX_CANDS; i++) out->idx[out->n++] = pas[i];
    if (good_idx >= 0 && out->n < SX_MAX_CANDS)   out->idx[out->n++] = good_idx;
    if (pickup_idx >= 0 && out->n < SX_MAX_CANDS) out->idx[out->n++] = pickup_idx;
}

// ---------- main MC ---------------------------------------------------------

// Worlds per decision (W1: all candidates, W2: surviving third, W3: final
// top-2 duel). Compact worlds + early rollout exits buy ~2-3x blackpowder's
// sampling budget at comparable wall-clock.
static void sx_params(int num_players, int *W1, int *W2, int *W3) {
    // ~2x the blackpowder-era budget. The compact bitboard rollout (~4x faster
    // per ply) pays for it: at this budget cordite is still at or below the old
    // wall-clock yet measurably stronger (more sampled worlds). Measured 400
    // games/pc vs handwritten: pc2 1.165/83.5% -> 1.115/88.5%, pc3 1.548->1.510,
    // pc4 2.007->1.990, pc8 4.183/12.2% -> 4.125/15.0%. Doubling again was not
    // reliably better and costs more (esp. at pc2 where the exact solver, not
    // the rollout, dominates), so the budget stops at 2x.
    // Semtex heads-up budget: 6x cordite's. Loss-audit finding: on the deals
    // semtex lost the pc2 cordite mirror, a 6x-worlds oracle won 66% (noise
    // control: 10%) — heads-up vs a strong opponent was variance-limited,
    // not saturated (the old saturation result was measured on weaker
    // fields / the TS engine, which already ran ~3x). Paired aggregate,
    // three seed sets: -0.168/-0.138/-0.173 mean finish, +14..+17pp win vs
    // the cordite control (vs -0.135/+13.5pp at 1x). Worlds only — wider
    // candidate survival added nothing (keeps-only: -0.133). 2x/3x were NOT
    // reliable stops (-0.130/-0.093); the measured knee is at ~6x.
    if (num_players <= 2)      { *W1 = 192; *W2 = 336; *W3 = 336; }
    // 3+ players: 6x cordite's C budget (hunt-3 finding). The rescue tests
    // showed pc4/pc6 losses were also compute-limited, but the aggregate
    // mirror gain is small (opponent MC noise dominates MC-vs-MC outcomes);
    // the real payoff is against HEURISTIC fields — the human proxies —
    // where the same budget is worth +5..+8pp win at pc3/pc4 (paired vs the
    // cordite control, seeds 950001: pc3 -0.087+-0.041 / 67.2% vs 59.5%,
    // pc4 -0.135+-0.053 / 39.8% vs 34.8%, attribution clean: the default
    // played 400/400 pairs identically to cordite on those deals) and
    // directionally positive at pc6 on both fields. pc5-8 use the ratios
    // production TS cordite already ships (its v2.4 budget = 6x the C one).
    else if (num_players <= 4) { *W1 = 168; *W2 = 336; *W3 = 336; }
    else if (num_players <= 6) { *W1 = 240; *W2 = 480; *W3 = 336; }
    else                       { *W1 = 240; *W2 = 480; *W3 = 288; }
    // cordite_old: the pre-change 1x budget (half the above). The only
    // strength-affecting change was doubling the budget — the rollout/solver
    // rewrites are exact — so this gives a faithful "cordite before the changes"
    // to play head-to-head against. Research-only.
    if (sx_old_budget) {
        if (num_players <= 2)      { *W1 = 16; *W2 = 28; *W3 = 28; }
        else if (num_players <= 4) { *W1 = 14; *W2 = 28; *W3 = 28; }
        else if (num_players <= 6) { *W1 = 20; *W2 = 40; *W3 = 28; }
        else                       { *W1 = 20; *W2 = 40; *W3 = 24; }
    }
    if (sx_oracle) { *W1 *= 6; *W2 *= 6; *W3 *= 6; }
    if (sx_w1_override > 0) *W1 = sx_w1_override;
    if (sx_w2_override > 0) *W2 = sx_w2_override;
    if (sx_w3_override >= 0) *W3 = sx_w3_override;
}

// SX_VERIFY=1: oracle self-check (test-only — reads real hands to validate
// the public-info belief, never to play).
static void sx_verify_belief(const Game *g, int bot_idx, const Belief *B) {
    for (int p = 0; p < g->num_players; p++) {
        if (p == bot_idx) continue;
        const Player *pl = &g->players[p];
        for (int k = 0; k < B->pinned_n[p]; k++) {
            bool found = false;
            for (int j = 0; j < pl->hand_count; j++) {
                if (card_eq(pl->hand[j], B->pinned[p][k])) { found = true; break; }
            }
            if (!found) {
                fprintf(stderr, "SX_VERIFY: pinned card v%d s%d NOT in p%d hand (logs=%d)\n",
                        B->pinned[p][k].value, B->pinned[p][k].suit, p, g->num_logs);
            }
        }
        for (int j = 0; j < pl->hand_count; j++) {
            if (sx_set_contains(B->pinned[p], B->pinned_n[p], pl->hand[j])) continue;
            if (B->void_n[p] > 0 && sx_void_forbidden(B, g, p, pl->hand[j])) {
                fprintf(stderr, "SX_VERIFY: void violated: p%d holds v%d s%d\n",
                        p, pl->hand[j].value, pl->hand[j].suit);
            }
            if (sx_floor_forbidden(B, g, p, pl->hand[j])) {
                fprintf(stderr, "SX_VERIFY: floor violated: p%d holds v%d s%d floor=%d\n",
                        p, pl->hand[j].value, pl->hand[j].suit, B->floor_v[p]);
            }
            if (!sx_set_contains(B->pool, B->n, pl->hand[j])) {
                fprintf(stderr, "SX_VERIFY: p%d unknown card v%d s%d missing from pool (logs=%d)\n",
                        p, pl->hand[j].value, pl->hand[j].suit, g->num_logs);
            }
        }
    }
}

int semtex_strategy_choose(const Game *g, int bot_idx,
                            const LegalMoves *moves, void *ctx) {
    (void)ctx;
    if (moves->n == 0) return -1;
    if (moves->n == 1) return 0;

    if (!sx_flags_loaded) {
        sx_no_solve  = sx_flag("SX_NO_SOLVE");
        sx_no_voids  = sx_flag("SX_NO_VOIDS");
        sx_no_flip   = sx_flag("SX_NO_FLIP");
        sx_no_floors = sx_flag("SX_NO_FLOORS");
        // Exact leaf endgames in rollouts are OFF by default: measured both
        // slower and weaker vs the real (imperfect) opponent pool — modeling
        // actual opponents beats assuming perfect play. SX_LEAF=1 re-enables.
        sx_no_leaf   = !sx_flag("SX_LEAF");
        sx_no_avoid  = sx_flag("SX_NO_AVOID");
        sx_verify    = sx_flag("SX_VERIFY");
        sx_w1_override = sx_env_int("SX_W1", 0);
        sx_w2_override = sx_env_int("SX_W2", 0);
        sx_w3_override = sx_env_int("SX_W3", -1);
        sx_keep1 = sx_env_int("SX_KEEP1", 0);
        sx_keep2 = sx_env_int("SX_KEEP2", 0);
        sx_rollout_policy = sx_env_int("SX_ROLLOUT", 0);
        sx_bb_win_budget = sx_env_int("SX_BB_WIN", 150000);
        sx_solve_cards = sx_env_int("SX_SOLVE_CARDS", 24);
        sx_bb_avoid_budget = sx_env_int("SX_BB_AVOID", 100000);
        sx_leaf_budget = sx_env_int("SX_LEAF_BUDGET", 1500);
        sx_leaf_max_cards = sx_env_int("SX_LEAF_CARDS", 10);
        sx_floor_mod = sx_env_int("SX_FLOOR_MOD", 2);
        if (sx_floor_mod < 1) sx_floor_mod = 1;
        sx_full_logs = sx_flag("SX_FULL_LOGS");
        sx_no_earlyexit = sx_flag("SX_NO_EARLYEXIT");
        sx_no_fastroll = sx_flag("SX_NO_FASTROLL");
        sx_bbleaf = sx_env_int("SX_BBLEAF", 2);
        sx_adapt = sx_env_int("SX_ADAPT", 1);
        sx_void_mod = sx_env_int("SX_VOID_MOD", 4);
        if (sx_void_mod < 2) sx_void_mod = 2;
        sx_profile = sx_env_int("SX_PROFILE", 0);
        sx_bbleaf_cards = sx_env_int("SX_BBLEAF_CARDS", 0);
        sx_bbleaf_cards2 = sx_env_int("SX_BBLEAF_CARDS2", 8);
        sx_bbleaf_budget = sx_env_int("SX_BBLEAF_BUDGET", 3000);
        sx_difftest = sx_flag("SX_DIFFTEST");
        if (sx_difftest) { void sx_difftest_report(void); atexit(sx_difftest_report); }
        sx_flags_loaded = 1;
    }

    uint32_t saved_rng = game_rng_get();

    Belief B;
    sx_build_belief(g, bot_idx, &B);
    if (sx_no_voids) for (int p = 0; p < MAX_PLAYERS; p++) B.void_n[p] = 0;
    if (sx_verify) sx_verify_belief(g, bot_idx, &B);

    // Player-count gate for the leaf lever (see sx_bbleaf comment). In the
    // pc-aware default mode heads-up uses the small-leaf threshold.
    sx_bbleaf_on = (sx_bbleaf != 0);
    sx_bbleaf_cards_eff = (sx_bbleaf == 2 && g->num_players == 2)
                        ? sx_bbleaf_cards2 : sx_bbleaf_cards;

    // Per-seat rollout policies: profiled-weak seats get the LOOSE model.
    sx_polmap = NULL;
    if (sx_profile) {
        bool any = false;
        for (int p = 0; p < g->num_players; p++) {
            sx_polmap_buf[p] = B.loose[p] ? CD_POL_LOOSE : CD_POL_HW;
            if (B.loose[p]) any = true;
        }
        if (any) sx_polmap = sx_polmap_buf;
    }

    // Exact endgame: take a proven win; mark proven losses for exclusion.
    static _Thread_local bool forced_loss[MAX_LEGAL_MOVES];
    memset(forced_loss, 0, (size_t)moves->n * sizeof(bool));
    int n_safe = moves->n;
    int solved = sx_no_solve ? -1
               : sx_try_endgame_solve(g, bot_idx, moves, &B, forced_loss, &n_safe);
    if (solved >= 0) {
        game_rng_set(saved_rng);
        return solved;
    }

    Candidates C;
    sx_pick_candidates(g, moves, forced_loss, &C);
    if (C.n == 0) {
        // Everything we'd consider is a proven loss; fall back to all moves.
        memset(forced_loss, 0, (size_t)moves->n * sizeof(bool));
        sx_pick_candidates(g, moves, forced_loss, &C);
    }
    if (C.n == 0) { game_rng_set(saved_rng); return 0; }
    if (C.n == 1) { game_rng_set(saved_rng); return C.idx[0]; }

    int W1, W2, W3;
    sx_params(g->num_players, &W1, &W2, &W3);

    uint32_t base = sx_mix((uint32_t)g->num_logs * 2654435761u,
                           ((uint32_t)g->deck_count << 8)
                           ^ (uint32_t)g->discard_pile_length
                           ^ ((uint32_t)bot_idx << 20));

    double score[SX_MAX_CANDS] = {0};
    int    nsim [SX_MAX_CANDS] = {0};
    bool   alive[SX_MAX_CANDS];
    for (int i = 0; i < C.n; i++) alive[i] = true;

    static _Thread_local Game world, trial;
    static _Thread_local SimState world_sim, trial_sim;

    // The fast bitboard path: convert each sampled WORLD to a compact SimState
    // ONCE, then each candidate just clones the SimState, applies its move on
    // bitboards, and plays out. The struct path (SX_NO_FASTROLL / SX_LEAF /
    // SX_DIFFTEST) keeps the per-candidate Game clone for the leaf solver and
    // the exact-equivalence difftest.
    bool fast_path = !sx_no_fastroll && sx_no_leaf && !sx_difftest && !sx_flag("SX_NO_WORLDSIM");

    // Stage 1: all candidates on W1 shared worlds.
    // Stage 2: surviving third on W2 more shared worlds.
    // Stage 3: top 2 duel on W3 final shared worlds.
    for (int stage = 0; stage < 3; stage++) {
        int w_lo = (stage == 0) ? 0 : (stage == 1) ? W1 : W1 + W2;
        int w_hi = (stage == 0) ? W1 : (stage == 1) ? W1 + W2 : W1 + W2 + W3;
        for (int w = w_lo; w < w_hi; w++) {
            uint32_t wseed = sx_mix(base, (uint32_t)(w + 1) * 0x85EBCA77u);
            // Belief mixture: voids assume cover-if-you-can pickups (3 of 4
            // worlds), floors assume lowest-first attackers (every other
            // world). Per-player distrust already cleared bogus constraints.
            bool use_voids  = (w % sx_void_mod) != sx_void_mod - 1;
            bool use_floors = !sx_no_floors && (w % sx_floor_mod) == 0;
            sx_sample_world(&world, g, bot_idx, &B, wseed, use_voids, use_floors);
            uint32_t sim_rng = sx_mix(wseed, 0x51AB1E5u);

            if (fast_path) {
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
                        fp = (sx_bbleaf_on || sx_polmap)
                           ? cd_sim_playout_pol(&trial_sim, bot_idx, 600,
                                                !sx_no_earlyexit,
                                                sx_bbleaf_on ? sx_bbleaf_cards_eff : 0,
                                                sx_bbleaf_budget, sx_polmap)
                           : cd_sim_playout(&trial_sim, bot_idx, 600, !sx_no_earlyexit);
                        if (fp == 0) fp = g->num_players;
                    }
                    score[ci] += (double)fp;
                    nsim[ci]++;
                }
                continue;
            }

            for (int ci = 0; ci < C.n; ci++) {
                if (!alive[ci]) continue;
                sx_lite_clone(&trial, &world);
                game_rng_set(sim_rng);   // identical stream for every move
                if (!sx_apply(&trial, bot_idx, &moves->moves[C.idx[ci]])) {
                    score[ci] += (double)g->num_players;
                    nsim[ci]++;
                    continue;
                }
                int fp = sx_rollout(&trial, bot_idx, 600);
                if (fp == 0) fp = g->num_players;
                score[ci] += (double)fp;
                nsim[ci]++;
            }
        }
        if (stage < 2) {
            int n_alive = 0;
            for (int i = 0; i < C.n; i++) if (alive[i]) n_alive++;
            int keep;
            if (stage == 0) {
                keep = (sx_keep1 > 0) ? sx_keep1 : (sx_oracle ? (C.n + 1) / 2 : C.n / 3);
                if (keep < (sx_oracle ? 4 : 3)) keep = sx_oracle ? 4 : 3;
            } else {
                keep = (sx_keep2 > 0) ? sx_keep2 : (sx_oracle ? 3 : 2);
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

// Oracle semtex: the same brain with 6x the sampled-world budget and wider
// candidate survival. Research-only — used to audit losses: where the oracle
// picks a different move, the default budget was the binding constraint.
int semtex_oracle_strategy_choose(const Game *g, int bot_idx,
                                  const LegalMoves *moves, void *ctx) {
    sx_oracle = 1;
    int r = semtex_strategy_choose(g, bot_idx, moves, ctx);
    sx_oracle = 0;
    return r;
}
