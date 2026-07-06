// Novichok — the CHEATING apex bot (research/eval ONLY, never seeded to
// production; named after something that must never be used in battle).
// Espresso's cheat industrialized: the full semtex/octogen machinery —
// staged determinized MC, exact rollout leaves, the extended exact root
// solver — but the belief-constrained world sampler is replaced by the
// TRUTH. It reads the real opponent hands from the engine state; the only
// remaining uncertainty is deck ORDER (the engine draws at random, so the
// order genuinely does not exist in advance — each world shuffles it).
// Belief inference, floors, voids, tells and profiling are all bypassed:
// there is nothing left to infer. Derived from semtex_strategy.c.
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

#include "novichok_strategy.h"
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

static inline int nv_card_score(Card c, int power) {
    return c.value + (c.suit == power ? 1000 : 0);
}

static bool nv_set_contains(const Card *arr, int n, Card c) {
    for (int i = 0; i < n; i++) if (card_eq(arr[i], c)) return true;
    return false;
}

static uint32_t nv_xorshift(uint32_t s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s ? s : 0xB1A570u;
}

static uint32_t nv_mix(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u);
    h ^= h >> 16; h *= 0x85EBCA77u; h ^= h >> 13;
    return h ? h : 1;
}

// Ablation switches (read once): NV_NO_SOLVE / NV_NO_VOIDS / NV_NO_FLIP /
// NV_NO_FLOORS / NV_NO_LEAF / NV_NO_AVOID / NV_VERIFY, plus NV_W1/NV_W2
// world-count overrides for tuning.
static int nv_flag(const char *name) {
    const char *v = getenv(name);
    return v && v[0] && v[0] != '0';
}
static int nv_env_int(const char *name, int def) {
    const char *v = getenv(name);
    return (v && v[0]) ? atoi(v) : def;
}
static _Thread_local int nv_flags_loaded = 0;
static _Thread_local int nv_no_solve = 0, nv_no_voids = 0, nv_no_flip = 0;
static _Thread_local int nv_no_floors = 0, nv_no_leaf = 0, nv_no_avoid = 0;
static _Thread_local int nv_no_earlyexit = 0;
static _Thread_local int nv_verify = 0;
static _Thread_local int nv_no_fastroll = 0;   // NV_NO_FASTROLL=1: struct rollout
// Bitboard exact-leaf endgames inside rollouts (semtex's own lever): resolve
// small 2-player deck-empty rollout endgames with the fast bitboard solver
// instead of handwritten policy play. Against opponents that themselves play
// endgames exactly (cordite), the exact model is the realistic one.
// NV_BBLEAF: 2 (default) = pc-aware exact leaf endgames in rollouts —
// small (NV_BBLEAF_CARDS2, 8) leaves heads-up only; NV_BBLEAF_CARDS
// (default 0 = off) at 3+ players. Loss analysis showed 12-card leaves at
// 3+ inject "the endgame will be played perfectly" into mid-game values —
// individually terrible calls (trump-burning covers, passive pickups) that
// exactly cancel the good calls (paired mirror delta ~0) while costing 3x
// wall-clock; the replicated win is the heads-up leaf. 1 = NV_BBLEAF_CARDS
// everywhere; 0 = off.
static _Thread_local int nv_bbleaf = 2;
static _Thread_local int nv_bbleaf_cards2 = 8;
// NV_ADAPT: void-contradiction => per-seat distrust of floors+voids (on by
// default — pure evidence, no downside). NV_PROFILE: weak-seat detection +
// LOOSE rollout model for profiled seats.
static _Thread_local int nv_adapt = 1;
// NV_PEEK=1 (whole-game predicted-order trials — measured WORSE than the
// hands-only cheat, kept as the documented failed design, see NOVICHOK.md):
// evaluate each candidate with a PREDICTED-ORDER trial
// instead of shuffled-deck worlds. The engine's future draws are a
// deterministic function of the current RNG state and the number of
// game_random() consumptions before each draw; cordite-family opponents
// consume zero net RNG (they save/restore), so a trial that replicates the
// harness loop exactly — same eligible-actor shuffles, RNG-NEUTRAL
// handwritten stand-ins (choice computed, stream restored), live RNG for
// draw_card — draws the EXACT cards the real game will draw for as long as
// the predicted moves match reality, and decays into an ordinary random
// order after a divergence. Alignment holds vs cordite/semtex/octogen and
// random-strategy seats (private RNG); it does NOT hold vs espresso or
// handwritten seats (they consume game RNG they never restore).
// Measured (paired vs octogen @ cordite pc2, seeds 980001): peek-3 +0.180,
// peek-24 +0.127 vs hands-only +0.035 — the prediction is exact only until
// the first move divergence, and a handful of near-order trials carries far
// more variance than a few hundred shuffled worlds.
// NV_PEEK=2 (refill pinning, the DEFAULT) keeps the full shuffled-world MC
// and pins only what is PROVABLY determined: refill runs synchronously inside
// the battle-ending move handlers (no strategy acts in between), so for any
// candidate that ends the battle the exact refill cards are a deterministic
// function of the live RNG state — zero prediction risk. Each candidate is
// probe-applied once on a full clone with the live stream; the recorded
// draw sequence is force-fed to the sim's refill in every sampled world.
// Measured vs the clean hands-only baseline on the same deals, pinning is
// better-or-equal in EVERY cell, all fields, all player counts (biggest:
// cordite pc2 59.0% -> 63.5% win, handwritten pc3 69.5% -> 78.0%, octogen
// pc4 16% -> 22%; never a worse cell). Heads-up the pinned refill IS the
// opponent's exact next hand, so the next battle's worlds are all-true.
// Sound order-knowledge helps wherever the order is provable; peek-1's
// whole-game trials were the wrong harness for it.
static _Thread_local int nv_peek = 2;
static _Thread_local int nv_peek_trials = 3;   // trials averaged per candidate
// NV_REPLY: the first opponent decision in each playout is chosen by search
// over their legal replies (cd_sim_playout_reply, octogen's hunt-4 lever)
// instead of assumed from the handwritten rollout policy. For the HONEST bot
// this measured null — the opponent's "best reply" computed against a
// guessed hand sharpens noise. Novichok's worlds are TRUE, so the searched
// reply is the real best response: the one place the cheater can predict
// moves by searching rather than guessing. NV_REPLY_STAGE: first MC stage
// (0-2) that uses the tournament. NV_REPLY_CAP: replies searched per world.
static _Thread_local int nv_reply = 0;
static _Thread_local int nv_reply_cap = 6;
static _Thread_local int nv_reply_stage = 2;
// Void world-mixture: voids applied in (mod-1)/mod of sampled worlds
// (cordite: 3 of 4). A softer mixture hedges between heuristic-family
// opponents (voids true) and MC/human strategic pickups (voids misleading).
static _Thread_local int nv_void_mod = 4;
static _Thread_local int nv_profile = 0;
// Root endgame-solve card ceiling (cordite: 20). The bitboard solver + TT can
// resolve bigger endgames within budget; a higher ceiling opens a window where
// semtex plays exactly while cordite still guesses with MC.
static _Thread_local int nv_solve_cards = 20;
static _Thread_local int nv_bbleaf_cards = 12;
static _Thread_local long nv_bbleaf_budget = 3000;
// Per-seat rollout policy map for the current decision (NULL = all
// handwritten). Set by novichok_strategy_choose when profiling flags seats.
static _Thread_local uint8_t nv_polmap_buf[MAX_PLAYERS];
static _Thread_local const uint8_t *nv_polmap = NULL;
static _Thread_local int nv_bbleaf_on = 0;   // effective flag for this decision
static _Thread_local int nv_bbleaf_cards_eff = 12;
static _Thread_local int nv_difftest = 0;      // NV_DIFFTEST=1: assert fast==slow
static _Thread_local int nv_w1_override = 0, nv_w2_override = 0;
static _Thread_local int nv_w3_override = -1;
static _Thread_local int nv_old_budget = 0;    // cordite_old variant: pre-2x worlds
static _Thread_local int nv_keep1 = 0, nv_keep2 = 0;  // NV_KEEP1/2: candidates kept past stage 0/1 (0=default n/3, 2)
// Oracle mode (research): multiply the world budget and widen candidate
// survival for one call. Used by novichok_oracle_strategy_choose to audit
// loss games — a decision the oracle changes was compute-limited.
static _Thread_local int nv_oracle = 0;
static _Thread_local int nv_rollout_policy = 0;       // NV_ROLLOUT: 0=default, 1=espresso, 2=handwritten (struct path)
// Bitboard endgame-solver node budgets (per shared pass). The bitboard solver
// (transposition table + O(1) clone) resolves far more per node than the
// struct solver, so it needs a much smaller node budget to do equivalent work
// in less wall-clock. Tunable via NV_BB_WIN / NV_BB_AVOID for sweeps.
static _Thread_local int nv_bb_win_budget = 20000;
static _Thread_local int nv_bb_avoid_budget = 15000;

static int nv_in_count(const Game *g) {
    int n = 0;
    for (int i = 0; i < g->num_players; i++)
        if (g->players[i].status == PLAYER_STATUS_IN) n++;
    return n;
}

static bool nv_apply(Game *g, int p_idx, const LegalMove *m) {
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
static void nv_lite_clone(Game *dst, const Game *src) {
    size_t base = offsetof(Game, logs);
    memcpy(dst, src, base + (size_t)src->num_logs * sizeof(GameLog));
}

// ---------- belief state ------------------------------------------------

#define NV_MAX_VOIDS 6

typedef struct {
    Card pool[80];                  // unseen pool (deck ∪ opp unknowns)
    int  n;
    Card pinned[MAX_PLAYERS][MAX_HAND_SIZE];  // publicly located in p's hand
    int  pinned_n[MAX_PLAYERS];
    // Hard-ish void constraints (see blackpowder): attack cards a defender
    // demonstrably couldn't cover at pickup time. Cleared on their draw.
    Card voids[MAX_PLAYERS][NV_MAX_VOIDS];
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

static bool nv_void_forbidden(const Belief *B, const Game *g, int p, Card c) {
    for (int k = 0; k < B->void_n[p]; k++) {
        if (can_cover(B->voids[p][k], c, g->power_suit)) return true;
    }
    return false;
}

static bool nv_floor_forbidden(const Belief *B, const Game *g, int p, Card c) {
    return B->floor_v[p] > 0 && c.suit != g->power_suit && c.value < B->floor_v[p];
}

static void nv_pinned_remove(Belief *B, int p, Card c) {
    for (int q = 0; q < B->pinned_n[p]; q++) {
        if (card_eq(B->pinned[p][q], c)) {
            B->pinned[p][q] = B->pinned[p][B->pinned_n[p] - 1];
            B->pinned_n[p]--;
            return;
        }
    }
}

static void nv_pinned_add(Belief *B, int p, Card c) {
    if (B->pinned_n[p] >= MAX_HAND_SIZE) return;
    if (nv_set_contains(B->pinned[p], B->pinned_n[p], c)) return;
    B->pinned[p][B->pinned_n[p]++] = c;
}

// A floor contradiction (p plays a non-trump card below their inferred
// floor) means p is not a lowest-first attacker: distrust their floors.
static void nv_floor_check(Belief *B, const Game *g, int p, Card c) {
    if (p < 0 || B->floor_v[p] <= 0) return;
    if (c.suit != g->power_suit && c.value < B->floor_v[p]) {
        B->floor_v[p] = 0;
        B->distrust_floor[p] = true;
    }
}

// Chronological scan over logs: pinned cards, flipped-trump holder, void
// constraints, rank floors and trust flags, all in one pass.
static void nv_build_belief(const Game *g, int bot_idx, Belief *B) {
    memset(B, 0, sizeof(*B));

    // Last draw event: holds the flipped trump if the deck is exhausted, and
    // marks the moment the deck died (for "deck alive at log i" tests).
    int last_draw_idx = -1;
    for (int i = 0; i < g->num_logs; i++) {
        if (g->logs[i].log_type == LOG_DRAW) last_draw_idx = i;
    }
    bool deck_alive_now = (g->deck_count > 0 || g->has_flipped);
    int flip_log_idx = (!deck_alive_now && !nv_no_flip) ? last_draw_idx : -1;

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
                        if (!nv_set_contains(B->pinned[p], B->pinned_n[p], c)
                            && nv_void_forbidden(B, g, p, c)) B->mc_tell[p] = true;
                        if (c.suit != g->power_suit
                            && (decl_vals[p] & (1u << c.value))) B->mc_tell[p] = true;
                        nv_floor_check(B, g, p, c);
                        nv_pinned_remove(B, p, c);
                    }
                }
                if (p >= 0 && p != bot_idx && L->log_type == LOG_ATTACK) {
                    if (any_trump && deck_alive_at) trump_viol[p]++;
                    if (first_attack && L->num_pairs == 1 && !any_trump
                        && in_now > 2 && !nv_no_floors) {
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
                        if (!nv_set_contains(B->pinned[p], B->pinned_n[p], c)
                            && nv_void_forbidden(B, g, p, c)) B->mc_tell[p] = true;
                        if (c.suit != g->power_suit
                            && (decl_vals[p] & (1u << c.value))) B->mc_tell[p] = true;
                        nv_floor_check(B, g, p, c);
                        nv_pinned_remove(B, p, c);
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
                    if (unc_n == 1 && B->void_n[p] < NV_MAX_VOIDS) {
                        B->voids[p][B->void_n[p]++] = unc[0];
                    }
                    for (int k = 0; k < tbl_n; k++) nv_pinned_add(B, p, tbl[k]);
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
                    if (i == flip_log_idx) nv_pinned_add(B, p, g->flipped);
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
    if (nv_adapt) {
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
    if (nv_profile) {
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
            if (!nv_set_contains(known, kn, c)) B->pool[B->n++] = c;
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
            if (!nv_void_forbidden(B, g, p, B->pool[i])
                && !nv_floor_forbidden(B, g, p, B->pool[i])) allowed++;
        }
        if (allowed < unknown && B->floor_v[p] > 0) {
            B->floor_v[p] = 0;
            allowed = 0;
            for (int i = 0; i < B->n; i++) {
                if (!nv_void_forbidden(B, g, p, B->pool[i])) allowed++;
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
static _Thread_local int nv_full_logs = 0;   // NV_FULL_LOGS=1: bp-style worlds

static void nv_sample_world(Game *g_out, const Game *g_in, int my_idx,
                            const Belief *B, uint32_t seed,
                            bool apply_voids, bool apply_floors) {
    if (nv_full_logs) {
        game_clone(g_out, g_in);
    } else {
        memcpy(g_out, g_in, offsetof(Game, logs));
        int nl = 0;
        for (int i = 0; i < g_in->num_logs; i++) {
            if (g_in->logs[i].log_type == LOG_DISCARD) g_out->logs[nl++] = g_in->logs[i];
        }
        g_out->num_logs = nl;
    }

    // CHEAT: the clone above already carries every player's REAL hand and
    // the real deck contents. The only genuine unknown is the draw order
    // (the engine draws random indices), so a "world" is just a per-seed
    // shuffle of the deck. Everything below this block in the honest
    // sampler (pinned fills, pool deals, void/floor repairs) is skipped.
    {
        uint32_t s2 = seed ? seed : 0xCAFEu;
        for (int i = g_out->deck_count - 1; i > 0; i--) {
            s2 = nv_xorshift(s2);
            int j = (int)(s2 % (uint32_t)(i + 1));
            Card sw = g_out->deck[i]; g_out->deck[i] = g_out->deck[j]; g_out->deck[j] = sw;
        }
        (void)my_idx; (void)B; (void)apply_voids; (void)apply_floors;
        return;
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
        s = nv_xorshift(s);
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
        bool bad = (use_v && nv_void_forbidden(B, g_in, p, c))
                || (use_f && nv_floor_forbidden(B, g_in, p, c));
        if (!bad) continue;
        for (int d = 0; d < deck_n; d++) {
            Card dc = g_out->deck[d];
            bool dc_bad = (use_v && nv_void_forbidden(B, g_in, p, dc))
                       || (use_f && nv_floor_forbidden(B, g_in, p, dc));
            if (!dc_bad) {
                g_out->deck[d] = c;
                g_out->players[p].hand[slots[si].slot] = dc;
                break;
            }
        }
    }
}

// ---------- exact solver (shared by root + rollout leaves) -----------------

#define NV_SOLVE_MAX_DEPTH   48
#define NV_SOLVE_MAX_MOVES   96
#define NV_SOLVE_BUDGET      200000L
#define NV_AVOID_BUDGET      150000L
#define NV_SOLVE_MAX_CARDS   20
#define NV_LEAF_BUDGET       1500L

typedef struct {
    long budget;
    bool aborted;
    int  me;
    Game       *child;   // [NV_SOLVE_MAX_DEPTH]
    LegalMoves *mv;      // [NV_SOLVE_MAX_DEPTH]
} Solver;

static _Thread_local Game       *nv_solver_child = NULL;
static _Thread_local LegalMoves *nv_solver_mv = NULL;

static bool nv_solver_ready(void) {
    if (!nv_solver_child) {
        nv_solver_child = malloc(sizeof(Game) * NV_SOLVE_MAX_DEPTH);
        nv_solver_mv    = malloc(sizeof(LegalMoves) * NV_SOLVE_MAX_DEPTH);
    }
    return nv_solver_child && nv_solver_mv;
}

// Value in [-1000, 1000] from `me`'s perspective: positive = me escaping,
// negative = me as durak. Magnitude prefers faster wins / slower losses.
static int nv_solve(Solver *S, const Game *g, int alpha, int beta, int depth) {
    int loser = game_done(g);
    if (loser >= 0) return (loser == S->me) ? -(1000 - depth) : (1000 - depth);
    if (nv_in_count(g) == 0) return 0;   // defensive: simultaneous out
    if (depth >= NV_SOLVE_MAX_DEPTH) { S->aborted = true; return 0; }
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

    LegalMoves *mv = &S->mv[depth];
    calculate_legal_moves(g, actor, mv);
    if (mv->n == 0) return 0;
    if (mv->n > NV_SOLVE_MAX_MOVES) { S->aborted = true; return 0; }

    bool maximizing = (actor == S->me);
    int best = maximizing ? -2000 : 2000;
    for (int i = 0; i < mv->n; i++) {
        Game *child = &S->child[depth];
        nv_lite_clone(child, g);
        if (!nv_apply(child, actor, &mv->moves[i])) continue;
        int v = nv_solve(S, child, alpha, beta, depth + 1);
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
static _Thread_local long nv_leaf_budget = NV_LEAF_BUDGET;
static _Thread_local int  nv_leaf_max_cards = 0;   // set from env at init
static _Thread_local int  nv_floor_mod = 2;        // floors in 1/mod worlds

static int nv_leaf_solve(const Game *g) {
    if (!nv_solver_ready()) return -1;
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
    nv_lite_clone(&root, g);
    root.num_logs = 0;   // solver never reads history

    Solver S;
    S.budget  = nv_leaf_budget;
    S.aborted = false;
    S.me      = me;
    S.child   = nv_solver_child;
    S.mv      = nv_solver_mv;

    // Null window around 0: only the sign matters (true values are ±(1000-d)
    // for decided games), and the narrow window maximizes pruning.
    int v = nv_solve(&S, &root, -1, 1, 0);
    if (S.aborted || v == 0) return -1;
    return (v > 0) ? opp : me;
}

// ---------- simulation ---------------------------------------------------

// Stage-aware rollout policy (gunpowder's rule): handwritten while the deck
// is alive or the game is heads-up, espresso for multi-player endgames.
static StrategyFn nv_rollout_for(const Game *g) {
    // NV_ROLLOUT (struct path only): 1 = espresso everywhere, 2 = handwritten
    // everywhere. Research knob for the "rollout-policy bias" hypothesis — vs a
    // strong opponent, a weak (handwritten) rollout policy biases value
    // estimates, so more worlds saturates. A stronger rollout policy may reduce
    // that bias. Run with NV_NO_FASTROLL=1.
    if (nv_rollout_policy == 1) return espresso_strategy_choose;
    if (nv_rollout_policy == 2) return handwritten_strategy_choose;
    bool deck_active = (g->deck_count > 0 || g->has_flipped);
    if (deck_active || nv_in_count(g) == 2) return handwritten_strategy_choose;
    return espresso_strategy_choose;
}

// Roll a sampled world forward; returns my finish position (1..N), or 0 if
// the simulation didn't terminate. Exits early once my position is known,
// and resolves small 2-player deck-empty endgames exactly (one attempt per
// rollout — a failed solve falls back to policy play for good).
static int nv_simulate(Game *g, int my_idx, int max_turns) {
    int turns = 0;
    bool leaf_tried = false;
    while (game_done(g) < 0 && turns++ < max_turns) {
        // My fate is sealed as soon as I'm out: position = elimination slot.
        if (!nv_no_earlyexit
            && g->players[my_idx].status != PLAYER_STATUS_IN) {
            for (int i = 0; i < g->num_eliminated; i++) {
                if (g->elimination_order[i] == my_idx) return i + 1;
            }
            break;   // not IN and not eliminated: corrupt state, bail
        }

        if (!nv_no_leaf && !leaf_tried && g->deck_count == 0 && !g->has_flipped
            && nv_in_count(g) == 2) {
            int total = 0;
            for (int i = 0; i < g->num_players; i++) {
                if (g->players[i].status == PLAYER_STATUS_IN)
                    total += g->players[i].hand_count;
            }
            if (total <= nv_leaf_max_cards) {
                leaf_tried = true;
                int loser = nv_leaf_solve(g);
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
            StrategyFn fn = nv_rollout_for(g);
            int idx = fn(g, pi, &moves, NULL);
            if (idx < 0 || idx >= moves.n) continue;
            if (nv_apply(g, pi, &moves.moves[idx])) { acted = true; break; }
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
// and play it out on bitmasks. ~10x faster per ply than nv_simulate. The
// effective rollout policy is always handwritten (see cordite_sim.c), and the
// exact leaf solver (NV_LEAF, off by default) is not used in the fast path.
static int nv_simulate_fast(const Game *g, int my_idx, int max_turns) {
    SimState s;
    cd_sim_from_game(&s, g);
    if (nv_bbleaf_on || nv_polmap)
        return cd_sim_playout_pol(&s, my_idx, max_turns, !nv_no_earlyexit,
                                  nv_bbleaf_on ? nv_bbleaf_cards_eff : 0,
                                  nv_bbleaf_budget, nv_polmap);
    return cd_sim_playout(&s, my_idx, max_turns, !nv_no_earlyexit);
}

// Dispatcher: bitboard rollout by default; struct rollout under NV_NO_FASTROLL
// or NV_LEAF (the leaf solver lives on the struct path). NV_DIFFTEST runs both
// and tallies divergences (printed at process exit by the eval harness if it
// hooks nv_difftest_report, else just counted).
static _Thread_local long nv_diff_total = 0, nv_diff_mismatch = 0;
static int nv_rollout(Game *g, int my_idx, int max_turns) {
    if (nv_no_fastroll || !nv_no_leaf) return nv_simulate(g, my_idx, max_turns);
    if (nv_difftest) {
        uint32_t rng0 = game_rng_get();
        int fast = nv_simulate_fast(g, my_idx, max_turns);
        game_rng_set(rng0);
        Game slow_g;
        nv_lite_clone(&slow_g, g);
        int slow = nv_simulate(&slow_g, my_idx, max_turns);
        nv_diff_total++;
        if (fast != slow) {
            nv_diff_mismatch++;
            if (nv_diff_mismatch <= 20) {
                fprintf(stderr, "NV_DIFFTEST mismatch #%ld: fast=%d slow=%d "
                        "(np=%d deck=%d logs=%d)\n", nv_diff_mismatch, fast, slow,
                        g->num_players, g->deck_count, g->num_logs);
            }
        }
        return slow;  // keep slow behavior while difftesting
    }
    return nv_simulate_fast(g, my_idx, max_turns);
}

void nv_difftest_report(void) {
    if (nv_diff_total > 0) {
        fprintf(stderr, "NV_DIFFTEST: %ld/%ld rollouts diverged (%.3f%%)\n",
                nv_diff_mismatch, nv_diff_total,
                100.0 * (double)nv_diff_mismatch / (double)nv_diff_total);
    }
}

// ---------- root endgame solve (win take + loss avoid) ---------------------

// Solve every root move with a full window when 2 players remain and the
// deck is empty (the unseen pool IS the opponent's hand — public deduction).
// Returns the fastest forced-win index, or -1. When no win exists, sets
// forced_loss[i] for root moves that lose under optimal play; the MC stage
// avoids them whenever at least one non-losing move exists.
static int nv_try_endgame_solve(const Game *g, int bot_idx,
                                const LegalMoves *moves, const Belief *B,
                                bool *forced_loss, int *n_safe) {
    *n_safe = moves->n;
    if (g->deck_count > 0 || g->has_flipped) return -1;
    if (nv_in_count(g) != 2) return -1;
    if (g->players[bot_idx].status != PLAYER_STATUS_IN) return -1;

    int opp = -1;
    for (int i = 0; i < g->num_players; i++) {
        if (i != bot_idx && g->players[i].status == PLAYER_STATUS_IN) opp = i;
    }
    if (opp < 0) return -1;

    // CHEAT: no deduction needed — the real hand is read directly below.

    int total = g->players[bot_idx].hand_count + g->players[opp].hand_count;
    if (total > nv_solve_cards) return -1;

    if (!nv_solver_ready()) return -1;

    Game root;
    game_clone(&root, g);   // CHEAT: the clone already holds the real hands
    root.num_logs = 0;

    // Fast path: solve on the compact bitboard engine (transposition table +
    // O(1) clone + bitmask move-gen). The bitboard solver returns the exact
    // same value as the struct solver when resolved (validated by
    // tests/solver_difftest.c), and resolves more positions within budget.
    // NV_NO_BBSOLVE=1 falls back to the struct solver for A/B.
    bool bbsolve = !nv_flag("NV_NO_BBSOLVE");
    SimState root_sim;
    if (bbsolve) cd_sim_from_game(&root_sim, &root);
    cd_sim_solve_reset();

    Solver S;
    S.budget  = NV_SOLVE_BUDGET;
    S.aborted = false;
    S.me      = bot_idx;
    S.child   = nv_solver_child;
    S.mv      = nv_solver_mv;
    long win_budget   = bbsolve ? (long)nv_bb_win_budget   : NV_SOLVE_BUDGET;
    long avoid_budget = bbsolve ? (long)nv_bb_avoid_budget : NV_AVOID_BUDGET;
    long budget = win_budget;

    // Pass 1 — win hunt (blackpowder's loop): fail-soft with an accumulating
    // alpha floor at 0, so losing subtrees prune immediately.
    // NV_BP_SOLVE=1 reverts to blackpowder's exact semantics (alpha starts
    // wide open, any abort bails the whole solve) for A/B testing.
    int best_idx = -1;
    int best_v = 0;       // only accept strictly winning lines
    int alpha = nv_flag("NV_BP_SOLVE") ? -2000 : 0;
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
            nv_lite_clone(&child, &root);
            if (!nv_apply(&child, bot_idx, &moves->moves[i])) continue;
            S.aborted = false;
            v = nv_solve(&S, &child, alpha, 2000, 1);
            aborted_i = S.aborted;
            if (S.budget <= 0) return -1;
        }
        if (aborted_i) { if (bail_on_abort) return -1; any_abort = true; continue; }
        if (v > best_v) { best_v = v; best_idx = i; }
        if (v > alpha) alpha = v;
    }
    if (best_idx >= 0) return best_idx;
    if (nv_no_avoid || any_abort) return -1;

    // Pass 2 — loss avoidance: no win exists, so classify each move with a
    // null window around 0 (sign only, maximal pruning).
    S.budget = NV_AVOID_BUDGET;
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
            nv_lite_clone(&child, &root);
            if (!nv_apply(&child, bot_idx, &moves->moves[i])) continue;
            S.aborted = false;
            v = nv_solve(&S, &child, -1, 0, 1);
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

// ---------- predicted-order peek trial (NV_PEEK) ---------------------------

// Play the future out on a full Game clone, replicating the REAL harness
// loop move-for-move: the same eligible-actor shuffle (consuming the live
// game RNG exactly as the harness will), actors chosen the same way, and
// draw_card consuming the live stream — so the cards drawn are the cards
// the real game will draw while the move prediction holds. All seats
// (including ours after the root move) are played by an RNG-NEUTRAL
// handwritten stand-in: the choice is computed, then the stream is restored,
// matching the zero net consumption of cordite-family opponents.
// Returns my finish position 1..N, or 0 if unterminated.
static int nv_peek_trial(const Game *g_in, int my_idx, const LegalMove *root_m,
                         uint32_t live_rng) {
    static _Thread_local Game g;
    game_clone(&g, g_in);
    game_rng_set(live_rng);
    if (!nv_apply(&g, my_idx, root_m)) return 0;

    int iters = 0;
    while (game_done(&g) < 0 && iters++ < 4000) {
        int elig[MAX_PLAYERS]; int n_e = 0;
        for (int i = 0; i < g.num_players; i++)
            if (should_bot_act(&g, i)) elig[n_e++] = i;
        if (n_e == 0) break;
        for (int i = n_e - 1; i > 0; i--) {   // the harness shuffle, verbatim
            int j = (int)(game_random() * (i + 1));
            if (j < 0) j = 0; if (j > i) j = i;
            int t = elig[i]; elig[i] = elig[j]; elig[j] = t;
        }
        bool acted = false;
        for (int k = 0; k < n_e; k++) {
            int pi = elig[k];
            LegalMoves moves;
            calculate_legal_moves(&g, pi, &moves);
            if (moves.n == 0) continue;
            uint32_t r = game_rng_get();   // RNG-neutral stand-in
            int idx = handwritten_strategy_choose(&g, pi, &moves, NULL);
            game_rng_set(r);
            if (idx < 0 || idx >= moves.n) continue;
            if (nv_apply(&g, pi, &moves.moves[idx])) { acted = true; break; }
        }
        if (!acted) break;
    }
    if (game_done(&g) < 0) return 0;
    for (int i = 0; i < g.num_eliminated; i++)
        if (g.elimination_order[i] == my_idx) return i + 1;
    return g.num_players;
}

// ---------- candidate selection -------------------------------------------

#define NV_MAX_CANDS 26

typedef struct {
    int idx[NV_MAX_CANDS];
    int n;
} Candidates;

static void nv_ranked_insert(int *idxs, double *keys, int *n, int cap,
                             int idx, double key) {
    int pos = *n;
    while (pos > 0 && keys[pos - 1] > key) pos--;
    if (pos >= cap) return;
    int last = (*n < cap) ? *n : cap - 1;
    for (int i = last; i > pos; i--) { idxs[i] = idxs[i - 1]; keys[i] = keys[i - 1]; }
    idxs[pos] = idx; keys[pos] = key;
    if (*n < cap) (*n)++;
}

static void nv_pick_candidates(const Game *g, const LegalMoves *moves,
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
                for (int j = 0; j < m->n_cards; j++) sum += nv_card_score(m->cards[j], power);
                nv_ranked_insert(atk, atk_k, &n_atk, 12, i,
                                 -(double)m->n_cards * 10000.0 + (double)sum);
                break;
            }
            case MOVE_COVER: {
                double prod = 1.0;
                for (int j = 0; j < m->n_cards; j++) prod *= (double)nv_card_score(m->cards[j], power);
                nv_ranked_insert(cov, cov_k, &n_cov, 10, i,
                                 prod - (double)m->n_cards * 0.5);
                break;
            }
            case MOVE_PASS: {
                int sum = 0;
                for (int j = 0; j < m->n_cards; j++) sum += nv_card_score(m->cards[j], power);
                nv_ranked_insert(pas, pas_k, &n_pas, 3, i, (double)sum);
                break;
            }
            case MOVE_GOOD:   good_idx = i;   break;
            case MOVE_PICKUP: pickup_idx = i; break;
            default: break;
        }
    }

    out->n = 0;
    for (int i = 0; i < n_atk && out->n < NV_MAX_CANDS; i++) out->idx[out->n++] = atk[i];
    for (int i = 0; i < n_cov && out->n < NV_MAX_CANDS; i++) out->idx[out->n++] = cov[i];
    for (int i = 0; i < n_pas && out->n < NV_MAX_CANDS; i++) out->idx[out->n++] = pas[i];
    if (good_idx >= 0 && out->n < NV_MAX_CANDS)   out->idx[out->n++] = good_idx;
    if (pickup_idx >= 0 && out->n < NV_MAX_CANDS) out->idx[out->n++] = pickup_idx;
}

// ---------- main MC ---------------------------------------------------------

// Worlds per decision (W1: all candidates, W2: surviving third, W3: final
// top-2 duel). Compact worlds + early rollout exits buy ~2-3x blackpowder's
// sampling budget at comparable wall-clock.
static void nv_params(int num_players, int *W1, int *W2, int *W3) {
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
    if (nv_old_budget) {
        if (num_players <= 2)      { *W1 = 16; *W2 = 28; *W3 = 28; }
        else if (num_players <= 4) { *W1 = 14; *W2 = 28; *W3 = 28; }
        else if (num_players <= 6) { *W1 = 20; *W2 = 40; *W3 = 28; }
        else                       { *W1 = 20; *W2 = 40; *W3 = 24; }
    }
    if (nv_oracle) { *W1 *= 6; *W2 *= 6; *W3 *= 6; }
    if (nv_w1_override > 0) *W1 = nv_w1_override;
    if (nv_w2_override > 0) *W2 = nv_w2_override;
    if (nv_w3_override >= 0) *W3 = nv_w3_override;
}

// NV_VERIFY=1: oracle self-check (test-only — reads real hands to validate
// the public-info belief, never to play).
static void nv_verify_belief(const Game *g, int bot_idx, const Belief *B) {
    for (int p = 0; p < g->num_players; p++) {
        if (p == bot_idx) continue;
        const Player *pl = &g->players[p];
        for (int k = 0; k < B->pinned_n[p]; k++) {
            bool found = false;
            for (int j = 0; j < pl->hand_count; j++) {
                if (card_eq(pl->hand[j], B->pinned[p][k])) { found = true; break; }
            }
            if (!found) {
                fprintf(stderr, "NV_VERIFY: pinned card v%d s%d NOT in p%d hand (logs=%d)\n",
                        B->pinned[p][k].value, B->pinned[p][k].suit, p, g->num_logs);
            }
        }
        for (int j = 0; j < pl->hand_count; j++) {
            if (nv_set_contains(B->pinned[p], B->pinned_n[p], pl->hand[j])) continue;
            if (B->void_n[p] > 0 && nv_void_forbidden(B, g, p, pl->hand[j])) {
                fprintf(stderr, "NV_VERIFY: void violated: p%d holds v%d s%d\n",
                        p, pl->hand[j].value, pl->hand[j].suit);
            }
            if (nv_floor_forbidden(B, g, p, pl->hand[j])) {
                fprintf(stderr, "NV_VERIFY: floor violated: p%d holds v%d s%d floor=%d\n",
                        p, pl->hand[j].value, pl->hand[j].suit, B->floor_v[p]);
            }
            if (!nv_set_contains(B->pool, B->n, pl->hand[j])) {
                fprintf(stderr, "NV_VERIFY: p%d unknown card v%d s%d missing from pool (logs=%d)\n",
                        p, pl->hand[j].value, pl->hand[j].suit, g->num_logs);
            }
        }
    }
}

int novichok_strategy_choose(const Game *g, int bot_idx,
                            const LegalMoves *moves, void *ctx) {
    (void)ctx;
    if (moves->n == 0) return -1;
    if (moves->n == 1) return 0;

    if (!nv_flags_loaded) {
        nv_no_solve  = nv_flag("NV_NO_SOLVE");
        nv_no_voids  = 1;
        nv_no_flip   = nv_flag("NV_NO_FLIP");
        nv_no_floors = 1;   // cheat: constraints are meaningless on true worlds
        // Exact leaf endgames in rollouts are OFF by default: measured both
        // slower and weaker vs the real (imperfect) opponent pool — modeling
        // actual opponents beats assuming perfect play. NV_LEAF=1 re-enables.
        nv_no_leaf   = !nv_flag("NV_LEAF");
        nv_no_avoid  = nv_flag("NV_NO_AVOID");
        nv_verify    = nv_flag("NV_VERIFY");
        nv_w1_override = nv_env_int("NV_W1", 0);
        nv_w2_override = nv_env_int("NV_W2", 0);
        nv_w3_override = nv_env_int("NV_W3", -1);
        nv_keep1 = nv_env_int("NV_KEEP1", 0);
        nv_keep2 = nv_env_int("NV_KEEP2", 0);
        nv_rollout_policy = nv_env_int("NV_ROLLOUT", 0);
        nv_bb_win_budget = nv_env_int("NV_BB_WIN", 400000);
        nv_solve_cards = nv_env_int("NV_SOLVE_CARDS", 28);
        nv_bb_avoid_budget = nv_env_int("NV_BB_AVOID", 250000);
        nv_leaf_budget = nv_env_int("NV_LEAF_BUDGET", 1500);
        nv_leaf_max_cards = nv_env_int("NV_LEAF_CARDS", 10);
        nv_floor_mod = nv_env_int("NV_FLOOR_MOD", 2);
        if (nv_floor_mod < 1) nv_floor_mod = 1;
        nv_full_logs = nv_flag("NV_FULL_LOGS");
        nv_no_earlyexit = nv_flag("NV_NO_EARLYEXIT");
        nv_no_fastroll = nv_flag("NV_NO_FASTROLL");
        nv_bbleaf = nv_env_int("NV_BBLEAF", 2);
        nv_adapt = nv_env_int("NV_ADAPT", 0);   // nothing to infer: worlds are true
        nv_peek = nv_env_int("NV_PEEK", 2);
        nv_peek_trials = nv_env_int("NV_PEEK_TRIALS", 3);
        nv_reply = nv_env_int("NV_REPLY", 0);
        nv_reply_cap = nv_env_int("NV_REPLY_CAP", 6);
        nv_reply_stage = nv_env_int("NV_REPLY_STAGE", 2);
        nv_void_mod = nv_env_int("NV_VOID_MOD", 4);
        if (nv_void_mod < 2) nv_void_mod = 2;
        nv_profile = nv_env_int("NV_PROFILE", 0);
        nv_bbleaf_cards = nv_env_int("NV_BBLEAF_CARDS", 0);
        nv_bbleaf_cards2 = nv_env_int("NV_BBLEAF_CARDS2", 8);
        nv_bbleaf_budget = nv_env_int("NV_BBLEAF_BUDGET", 3000);
        nv_difftest = nv_flag("NV_DIFFTEST");
        if (nv_difftest) { void nv_difftest_report(void); atexit(nv_difftest_report); }
        nv_flags_loaded = 1;
    }

    uint32_t saved_rng = game_rng_get();

    Belief B;
    nv_build_belief(g, bot_idx, &B);
    if (nv_no_voids) for (int p = 0; p < MAX_PLAYERS; p++) B.void_n[p] = 0;
    if (nv_verify) nv_verify_belief(g, bot_idx, &B);

    // Player-count gate for the leaf lever (see nv_bbleaf comment). In the
    // pc-aware default mode heads-up uses the small-leaf threshold.
    nv_bbleaf_on = (nv_bbleaf != 0);
    nv_bbleaf_cards_eff = (nv_bbleaf == 2 && g->num_players == 2)
                        ? nv_bbleaf_cards2 : nv_bbleaf_cards;

    // Per-seat rollout policies: profiled-weak seats get the LOOSE model.
    nv_polmap = NULL;
    if (nv_profile) {
        bool any = false;
        for (int p = 0; p < g->num_players; p++) {
            nv_polmap_buf[p] = B.loose[p] ? CD_POL_LOOSE : CD_POL_HW;
            if (B.loose[p]) any = true;
        }
        if (any) nv_polmap = nv_polmap_buf;
    }

    // Exact endgame: take a proven win; mark proven losses for exclusion.
    static _Thread_local bool forced_loss[MAX_LEGAL_MOVES];
    memset(forced_loss, 0, (size_t)moves->n * sizeof(bool));
    int n_safe = moves->n;
    int solved = nv_no_solve ? -1
               : nv_try_endgame_solve(g, bot_idx, moves, &B, forced_loss, &n_safe);
    if (solved >= 0) {
        game_rng_set(saved_rng);
        return solved;
    }

    Candidates C;
    nv_pick_candidates(g, moves, forced_loss, &C);
    if (C.n == 0) {
        // Everything we'd consider is a proven loss; fall back to all moves.
        memset(forced_loss, 0, (size_t)moves->n * sizeof(bool));
        nv_pick_candidates(g, moves, forced_loss, &C);
    }
    if (C.n == 0) { game_rng_set(saved_rng); return 0; }
    if (C.n == 1) { game_rng_set(saved_rng); return C.idx[0]; }

    // NV_PEEK: predicted-order trials replace the shuffled-world MC. Trial 0
    // starts from the LIVE RNG state (the real future while predictions
    // hold); trials t>0 pre-advance the stream by t draws — nearby orders
    // that hedge prediction decay after a move divergence. CRN: candidates
    // share each trial's start state. Ties keep the cheapest-first order.
    if (nv_peek == 1) {
        int best = -1;
        double best_v = 1e30;
        for (int ci = 0; ci < C.n; ci++) {
            double sum = 0;
            int ns = 0;
            for (int t = 0; t < nv_peek_trials; t++) {
                game_rng_set(saved_rng);
                for (int a = 0; a < t; a++) (void)game_random();
                uint32_t st = game_rng_get();
                int fp = nv_peek_trial(g, bot_idx, &moves->moves[C.idx[ci]], st);
                if (fp == 0) fp = g->num_players;
                sum += (double)fp;
                ns++;
            }
            double v = sum / (double)ns;
            if (v < best_v) { best_v = v; best = ci; }
        }
        game_rng_set(saved_rng);
        return best >= 0 ? C.idx[best] : 0;
    }

    // NV_PEEK=2: exact refill pinning. Probe-apply each candidate on a clone
    // with the LIVE RNG stream; any LOG_DRAW entries it appends are the exact
    // cards the real game will draw if this move is chosen (refill runs inside
    // the move handler — no other actor's decision intervenes). Those ids are
    // force-fed to the sim refill in every sampled world below, so battle-
    // ending candidates are evaluated on the true post-refill hands while the
    // deck's later order keeps the full shuffled-world smoothing.
    static _Thread_local uint8_t nv_forced_ids[NV_MAX_CANDS][32];
    static _Thread_local int     nv_forced_n[NV_MAX_CANDS];
    const int peek2_on = (nv_peek == 2);
    if (peek2_on) {
        static _Thread_local Game probe;
        for (int ci = 0; ci < C.n; ci++) {
            nv_forced_n[ci] = 0;
            nv_lite_clone(&probe, g);
            game_rng_set(saved_rng);
            int nl0 = probe.num_logs;
            if (!nv_apply(&probe, bot_idx, &moves->moves[C.idx[ci]])) continue;
            int n = 0;
            for (int li = nl0; li < probe.num_logs; li++) {
                const GameLog *l = &probe.logs[li];
                if (l->log_type != LOG_DRAW) continue;
                for (int cj = 0; cj < l->num_pairs && n < 32; cj++) {
                    Card c = l->pairs[cj].primary;
                    nv_forced_ids[ci][n++] = (uint8_t)(c.suit * 13 + (c.value - 1));
                }
            }
            nv_forced_n[ci] = n;
        }
        game_rng_set(saved_rng);
        if (nv_flag("NV_PEEK_DBG")) {
            int npin = 0, tot = 0;
            for (int ci = 0; ci < C.n; ci++) { if (nv_forced_n[ci]) npin++; tot += nv_forced_n[ci]; }
            fprintf(stderr, "peek2: deck=%d cands=%d pinned=%d cards=%d\n",
                    g->deck_count, C.n, npin, tot);
        }
    }

    int W1, W2, W3;
    nv_params(g->num_players, &W1, &W2, &W3);

    uint32_t base = nv_mix((uint32_t)g->num_logs * 2654435761u,
                           ((uint32_t)g->deck_count << 8)
                           ^ (uint32_t)g->discard_pile_length
                           ^ ((uint32_t)bot_idx << 20));

    double score[NV_MAX_CANDS] = {0};
    int    nsim [NV_MAX_CANDS] = {0};
    bool   alive[NV_MAX_CANDS];
    for (int i = 0; i < C.n; i++) alive[i] = true;

    static _Thread_local Game world, trial;
    static _Thread_local SimState world_sim, trial_sim;

    // The fast bitboard path: convert each sampled WORLD to a compact SimState
    // ONCE, then each candidate just clones the SimState, applies its move on
    // bitboards, and plays out. The struct path (NV_NO_FASTROLL / NV_LEAF /
    // NV_DIFFTEST) keeps the per-candidate Game clone for the leaf solver and
    // the exact-equivalence difftest.
    bool fast_path = !nv_no_fastroll && nv_no_leaf && !nv_difftest && !nv_flag("NV_NO_WORLDSIM");

    // Stage 1: all candidates on W1 shared worlds.
    // Stage 2: surviving third on W2 more shared worlds.
    // Stage 3: top 2 duel on W3 final shared worlds.
    for (int stage = 0; stage < 3; stage++) {
        int w_lo = (stage == 0) ? 0 : (stage == 1) ? W1 : W1 + W2;
        int w_hi = (stage == 0) ? W1 : (stage == 1) ? W1 + W2 : W1 + W2 + W3;
        for (int w = w_lo; w < w_hi; w++) {
            uint32_t wseed = nv_mix(base, (uint32_t)(w + 1) * 0x85EBCA77u);
            // Belief mixture: voids assume cover-if-you-can pickups (3 of 4
            // worlds), floors assume lowest-first attackers (every other
            // world). Per-player distrust already cleared bogus constraints.
            bool use_voids  = (w % nv_void_mod) != nv_void_mod - 1;
            bool use_floors = !nv_no_floors && (w % nv_floor_mod) == 0;
            nv_sample_world(&world, g, bot_idx, &B, wseed, use_voids, use_floors);
            uint32_t sim_rng = nv_mix(wseed, 0x51AB1E5u);

            if (fast_path) {
                cd_sim_from_game(&world_sim, &world);   // convert world ONCE
                bool reply_stage = nv_reply && stage >= nv_reply_stage;
                for (int ci = 0; ci < C.n; ci++) {
                    if (!alive[ci]) continue;
                    trial_sim = world_sim;              // cheap struct copy
                    game_rng_set(sim_rng);              // identical stream
                    if (peek2_on && nv_forced_n[ci] > 0)
                        cd_sim_set_forced_draws(nv_forced_ids[ci], nv_forced_n[ci]);
                    int fp;
                    if (!cd_sim_apply_root_move(&trial_sim, bot_idx,
                                                &moves->moves[C.idx[ci]])) {
                        fp = g->num_players;
                    } else if (reply_stage) {
                        fp = cd_sim_playout_reply(&trial_sim, bot_idx, 600,
                                                  nv_bbleaf_on ? nv_bbleaf_cards_eff : 0,
                                                  nv_bbleaf_budget, nv_polmap,
                                                  nv_reply_cap);
                        if (fp == 0) fp = g->num_players;
                    } else {
                        fp = (nv_bbleaf_on || nv_polmap)
                           ? cd_sim_playout_pol(&trial_sim, bot_idx, 600,
                                                !nv_no_earlyexit,
                                                nv_bbleaf_on ? nv_bbleaf_cards_eff : 0,
                                                nv_bbleaf_budget, nv_polmap)
                           : cd_sim_playout(&trial_sim, bot_idx, 600, !nv_no_earlyexit);
                        if (fp == 0) fp = g->num_players;
                    }
                    if (peek2_on) cd_sim_set_forced_draws(NULL, 0);
                    score[ci] += (double)fp;
                    nsim[ci]++;
                }
                continue;
            }

            for (int ci = 0; ci < C.n; ci++) {
                if (!alive[ci]) continue;
                nv_lite_clone(&trial, &world);
                game_rng_set(sim_rng);   // identical stream for every move
                if (!nv_apply(&trial, bot_idx, &moves->moves[C.idx[ci]])) {
                    score[ci] += (double)g->num_players;
                    nsim[ci]++;
                    continue;
                }
                int fp = nv_rollout(&trial, bot_idx, 600);
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
                keep = (nv_keep1 > 0) ? nv_keep1 : (nv_oracle ? (C.n + 1) / 2 : C.n / 3);
                if (keep < (nv_oracle ? 4 : 3)) keep = nv_oracle ? 4 : 3;
            } else {
                keep = (nv_keep2 > 0) ? nv_keep2 : (nv_oracle ? 3 : 2);
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
int novichok_oracle_strategy_choose(const Game *g, int bot_idx,
                                  const LegalMoves *moves, void *ctx) {
    nv_oracle = 1;
    int r = novichok_strategy_choose(g, bot_idx, moves, ctx);
    nv_oracle = 0;
    return r;
}
