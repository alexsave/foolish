// Octogen — semtex's successor: the same engine plus a stage-3 REPLY
// TOURNAMENT (the first opponent reply in each final-duel world is chosen
// by search over their legal replies, not assumed from the rollout policy).
// See OCTOGEN.md. Derived from semtex_strategy.c (cordite lineage below).
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

#include "octogen_strategy.h"
#include "strategy.h"
#include "bot_knobs.h"
#include "card.h"
#include "game.h"
#include "cordite_sim.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

// ---------- small utils ------------------------------------------------

static inline int og_card_score(Card c, int power) {
    return c.value + (c.suit == power ? 1000 : 0);
}

static bool og_set_contains(const Card *arr, int n, Card c) {
    for (int i = 0; i < n; i++) if (card_eq(arr[i], c)) return true;
    return false;
}

static uint32_t og_xorshift(uint32_t s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s ? s : 0xB1A570u;
}

static uint32_t og_mix(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u);
    h ^= h >> 16; h *= 0x85EBCA77u; h ^= h >> 13;
    return h ? h : 1;
}

// Ablation switches (read once): OG_NO_SOLVE / OG_NO_VOIDS / OG_NO_FLIP /
// OG_NO_FLOORS / OG_NO_LEAF / OG_NO_AVOID / OG_VERIFY, plus OG_W1/OG_W2
// world-count overrides for tuning.
// Values come from the bot roster's knob spec, with env still overriding for
// research/ablation sweeps (bot_knobs.h).
static int og_flag(const char *name) {
    return bot_knob_flag(name);
}
static int og_env_int(const char *name, int def) {
    return bot_knob_int(name, def);
}
static _Thread_local int og_flags_loaded = 0;
// Drop the latched OG_* values so the next choose re-reads them. Two callers:
// bot_roster_choose, which installs a different knob spec per named bot; and
// the infinite-oracle bridge (client replay analysis,
// docs/INFINITE_ORACLE_DESIGN.md §6.2), which re-reads OG_* between
// deliberation batches so the per-batch world budget can adapt to the measured
// device speed. Mirrors cordite_reload_flags (cordite_strategy.c). Only the
// oracle's *wasm export* is oracle-build-only (wasm_bots_api.c).
void og_reload_flags(void) { og_flags_loaded = 0; }
static _Thread_local int og_no_solve = 0, og_no_voids = 0, og_no_flip = 0;
static _Thread_local int og_no_floors = 0, og_no_leaf = 0, og_no_avoid = 0;
static _Thread_local int og_no_earlyexit = 0;
static _Thread_local int og_verify = 0;
#ifdef OG_HIDE_UNCOVERABLE
// A/B scaffolding for the shipped information-hiding rule
// (docs/OCTOGEN_HIDE_UNCOVERABLE.md). The rule itself ships unconditionally; when
// -DOG_HIDE_UNCOVERABLE is defined (eval builds only) it is instead gated to the
// seats whose bit is set in OG_HIDE_MASK, so hide_eval can isolate one seat and
// measure hide-vs-normal. The shipped bots/oracle wasm never define this macro,
// so they carry none of this scaffolding — only the always-on rule below.
static _Thread_local int og_hide_mask = 0;
long og_hide_fire_count = 0;   // # of partial-cover->pickup overrides (analysis)
#endif
static _Thread_local int og_no_fastroll = 0;   // OG_NO_FASTROLL=1: struct rollout
// Bitboard exact-leaf endgames inside rollouts (semtex's own lever): resolve
// small 2-player deck-empty rollout endgames with the fast bitboard solver
// instead of handwritten policy play. Against opponents that themselves play
// endgames exactly (cordite), the exact model is the realistic one.
// OG_BBLEAF: 2 (default) = pc-aware exact leaf endgames in rollouts —
// small (OG_BBLEAF_CARDS2, 8) leaves heads-up only; OG_BBLEAF_CARDS
// (default 0 = off) at 3+ players. Loss analysis showed 12-card leaves at
// 3+ inject "the endgame will be played perfectly" into mid-game values —
// individually terrible calls (trump-burning covers, passive pickups) that
// exactly cancel the good calls (paired mirror delta ~0) while costing 3x
// wall-clock; the replicated win is the heads-up leaf. 1 = OG_BBLEAF_CARDS
// everywhere; 0 = off.
static _Thread_local int og_bbleaf = 2;
static _Thread_local int og_bbleaf_cards2 = 8;
// OG_ADAPT: void-contradiction => per-seat distrust of floors+voids (on by
// default — pure evidence, no downside). OG_PROFILE: weak-seat detection +
// LOOSE rollout model for profiled seats.
static _Thread_local int og_adapt = 1;
// Reply tournament (octogen's lever): in late-stage worlds the first
// opponent reply is chosen by search over their legal replies instead of
// assumed from the rollout policy. OG_REPLY: 0 off (== semtex), 1 on
// (default). OG_REPLY_CAP: replies searched per world (cheap-first ranked;
// good/pickup always ranked last so they stay in range). OG_REPLY_STAGE:
// first MC stage (0-2) that uses the tournament (default 2 = final duel).
static _Thread_local int og_reply = 0;   // flat vs semtex; research knob
static _Thread_local int og_reply_cap = 6;
static _Thread_local int og_reply_stage = 2;
// Self-rollout ("infinite oracle" recursion, hunt 5 — research only, engaged
// solely through the octogen_self/ogs strategy id so a paired --control run
// can hold plain octogen in the same process): sampled worlds are played out
// by cd_sim_playout_self — every in-world seat deliberates via a nested
// tournament instead of the handwritten policy. OG_SELF_DEPTH: recursion
// depth (1 = tournament over playouts, 2 = tournament over depth-1 self
// playouts, ...). OG_SELF_CAP: moves searched per in-world decision (0 =
// base-case-only: policy play + immediate-win override). OG_SELF_PLIES:
// searched plies per playout level (0 = all). OG_SELF_WIN: the "does this
// move let me win" base case. OG_SELF_STAGE: first MC stage that self-rolls.
static _Thread_local int og_self = 0;          // set by octogen_self wrapper
static _Thread_local int og_self_depth = 1;
static _Thread_local int og_self_cap = 6;
static _Thread_local int og_self_plies = 0;
static _Thread_local int og_self_win = 1;
static _Thread_local int og_self_stage = 0;
// OG_SELF_LEAF_CARDS / OG_SELF_LEAF_BUDGET (-1 = inherit the bbleaf
// defaults): hero-only override of the in-rollout exact-leaf threshold, so
// the recursion LIMIT (perfect in-world endgame play) can be probed directly
// and cheaply — the solver+TT reach the same fixed point as infinite
// depth/cap in the deck-dead heads-up phase. Extends the hunt-4 "deeper
// rollout leaves" axis (10 cards / 8k nodes: flat at 4x cost).
static _Thread_local int og_self_leaf_cards = -1;
static _Thread_local long og_self_leaf_budget = -1;
// OG_MCDEF (default 0 — measured flat-to-harmful vs semtex, pc3
// +0.055±0.039; the 50% pickup rate over-models strategic pickups):
// seats with a proven mc_tell roll out with CD_POL_MCDEF. Research knob.
static _Thread_local int og_mcdef = 1;
// Void world-mixture: voids applied in (mod-1)/mod of sampled worlds
// (cordite: 3 of 4). A softer mixture hedges between heuristic-family
// opponents (voids true) and MC/human strategic pickups (voids misleading).
static _Thread_local int og_void_mod = 4;
static _Thread_local int og_profile = 0;
// Root endgame-solve card ceiling (cordite: 20). The bitboard solver + TT can
// resolve bigger endgames within budget; a higher ceiling opens a window where
// semtex plays exactly while cordite still guesses with MC.
static _Thread_local int og_solve_cards = 20;
// Loss-avoidance ceiling (OG_AVOID_CARDS, default 24 = semtex's window).
// The extended 25-28-card window is WIN-HUNT ONLY: taking a proven win is
// strictly safe (the win re-proves at every subsequent in-window ply), but
// avoiding "proven losing" moves out there measured HARMFUL against a
// near-peer (semtex tables pc2: 0 better/5 worse/195) — under optimal play
// they lose, but against an equally imperfect opponent they carry the best
// swindle equity, which the safe move forfeits. CORDITE.md's
// adverse-selection guard, one level up.
static _Thread_local int og_avoid_cards = 24;
static _Thread_local int og_bbleaf_cards = 12;
static _Thread_local long og_bbleaf_budget = 3000;
// Per-seat rollout policy map for the current decision (NULL = all
// handwritten). Set by octogen_strategy_choose when profiling flags seats.
static _Thread_local uint8_t og_polmap_buf[MAX_PLAYERS];
static _Thread_local const uint8_t *og_polmap = NULL;
static _Thread_local int og_bbleaf_on = 0;   // effective flag for this decision
static _Thread_local int og_bbleaf_cards_eff = 12;
static _Thread_local int og_difftest = 0;      // OG_DIFFTEST=1: assert fast==slow
static _Thread_local int og_w1_override = 0, og_w2_override = 0;
static _Thread_local int og_w3_override = -1;
static _Thread_local int og_old_budget = 0;    // cordite_old variant: pre-2x worlds
static _Thread_local int og_keep1 = 0, og_keep2 = 0;  // OG_KEEP1/2: candidates kept past stage 0/1 (0=default n/3, 2)
// Oracle mode (research): multiply the world budget and widen candidate
// survival for one call. Used by octogen_oracle_strategy_choose to audit
// loss games — a decision the oracle changes was compute-limited.
static _Thread_local int og_oracle = 0;
static _Thread_local int og_rollout_policy = 0;       // OG_ROLLOUT: 0=default, 1=espresso, 2=handwritten (struct path)
// Trump-conservation tie-break: the weak handwritten rollout policy undervalues
// keeping trumps while the deck is alive, so leading a low trump scores ~equal
// to a junk card and MC noise picks it ~half the time (measured: 52.5% -> 36.7%
// under a stronger rollout). A small tax per trump LED (attack only; covers are
// forced defense) added to the mean-finish score at selection tips near-ties
// toward the cheap non-trump WITHOUT overriding a genuine gap. In milli-units of
// mean-finish-position via OG_TRUMP_KEEP (default 40 = 0.040; MC noise between
// near-tied candidates ~0.05). 0 disables. Endgame (deck dead) is never taxed.
static _Thread_local double og_trump_keep = 0.040;
// Bitboard endgame-solver node budgets (per shared pass). The bitboard solver
// (transposition table + O(1) clone) resolves far more per node than the
// struct solver, so it needs a much smaller node budget to do equivalent work
// in less wall-clock. Tunable via OG_BB_WIN / OG_BB_AVOID for sweeps.
static _Thread_local int og_bb_win_budget = 20000;
static _Thread_local int og_bb_avoid_budget = 15000;

static int og_in_count(const Game *g) {
    int n = 0;
    for (int i = 0; i < g->num_players; i++)
        if (g->players[i].status == PLAYER_STATUS_IN) n++;
    return n;
}

static bool og_apply(Game *g, int p_idx, const LegalMove *m) {
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
static void og_lite_clone(Game *dst, const Game *src) {
    size_t base = offsetof(Game, logs);
    memcpy(dst, src, base + (size_t)src->num_logs * sizeof(GameLog));
}

// ---------- belief state ------------------------------------------------

#define OG_MAX_VOIDS 6

typedef struct {
    Card pool[80];                  // unseen pool (deck ∪ opp unknowns)
    int  n;
    Card pinned[MAX_PLAYERS][MAX_HAND_SIZE];  // publicly located in p's hand
    int  pinned_n[MAX_PLAYERS];
    // Hard-ish void constraints (see blackpowder): attack cards a defender
    // demonstrably couldn't cover at pickup time. Cleared on their draw.
    Card voids[MAX_PLAYERS][OG_MAX_VOIDS];
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

static bool og_void_forbidden(const Belief *B, const Game *g, int p, Card c) {
    for (int k = 0; k < B->void_n[p]; k++) {
        if (can_cover(B->voids[p][k], c, g->power_suit)) return true;
    }
    return false;
}

static bool og_floor_forbidden(const Belief *B, const Game *g, int p, Card c) {
    return B->floor_v[p] > 0 && c.suit != g->power_suit && c.value < B->floor_v[p];
}

static void og_pinned_remove(Belief *B, int p, Card c) {
    for (int q = 0; q < B->pinned_n[p]; q++) {
        if (card_eq(B->pinned[p][q], c)) {
            B->pinned[p][q] = B->pinned[p][B->pinned_n[p] - 1];
            B->pinned_n[p]--;
            return;
        }
    }
}

static void og_pinned_add(Belief *B, int p, Card c) {
    if (B->pinned_n[p] >= MAX_HAND_SIZE) return;
    if (og_set_contains(B->pinned[p], B->pinned_n[p], c)) return;
    B->pinned[p][B->pinned_n[p]++] = c;
}

// A floor contradiction (p plays a non-trump card below their inferred
// floor) means p is not a lowest-first attacker: distrust their floors.
static void og_floor_check(Belief *B, const Game *g, int p, Card c) {
    if (p < 0 || B->floor_v[p] <= 0) return;
    if (c.suit != g->power_suit && c.value < B->floor_v[p]) {
        B->floor_v[p] = 0;
        B->distrust_floor[p] = true;
    }
}

// Chronological scan over logs: pinned cards, flipped-trump holder, void
// constraints, rank floors and trust flags, all in one pass.
static void og_build_belief(const Game *g, int bot_idx, Belief *B) {
    memset(B, 0, sizeof(*B));

    // Last draw event: holds the flipped trump if the deck is exhausted, and
    // marks the moment the deck died (for "deck alive at log i" tests).
    int last_draw_idx = -1;
    for (int i = 0; i < g->num_logs; i++) {
        if (g->logs[i].log_type == LOG_DRAW) last_draw_idx = i;
    }
    bool deck_alive_now = (g->deck_count > 0 || g->has_flipped);
    int flip_log_idx = (!deck_alive_now && !og_no_flip) ? last_draw_idx : -1;

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
                        if (!og_set_contains(B->pinned[p], B->pinned_n[p], c)
                            && og_void_forbidden(B, g, p, c)) B->mc_tell[p] = true;
                        if (c.suit != g->power_suit
                            && (decl_vals[p] & (1u << c.value))) B->mc_tell[p] = true;
                        og_floor_check(B, g, p, c);
                        og_pinned_remove(B, p, c);
                    }
                }
                if (p >= 0 && p != bot_idx && L->log_type == LOG_ATTACK) {
                    if (any_trump && deck_alive_at) trump_viol[p]++;
                    if (first_attack && L->num_pairs == 1 && !any_trump
                        && in_now > 2 && !og_no_floors) {
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
                        if (!og_set_contains(B->pinned[p], B->pinned_n[p], c)
                            && og_void_forbidden(B, g, p, c)) B->mc_tell[p] = true;
                        if (c.suit != g->power_suit
                            && (decl_vals[p] & (1u << c.value))) B->mc_tell[p] = true;
                        og_floor_check(B, g, p, c);
                        og_pinned_remove(B, p, c);
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
                    if (unc_n == 1 && B->void_n[p] < OG_MAX_VOIDS) {
                        B->voids[p][B->void_n[p]++] = unc[0];
                    }
                    for (int k = 0; k < tbl_n; k++) og_pinned_add(B, p, tbl[k]);
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
                    if (i == flip_log_idx) {
                        // Deck just ran dry: this seat picked up the face-up
                        // trump, watched all game, so pin it. Its identity is the
                        // LAST card of this record (drawn once the deck emptied)
                        // and is the one REVEALED draw in the masked belief stream
                        // (other draws arrive {-1,-1}). Read it from the LOG, not
                        // g->flipped: the wire serializer zeroes g->flipped to
                        // {0,0} once has_flipped goes false (view.c), so the
                        // resident kernel state no longer carries the real card —
                        // pinning g->flipped there phantom-pins {0,0}.
                        if (L->num_pairs > 0) {
                            Card fc = L->pairs[L->num_pairs - 1].primary;
                            if (fc.value >= 0 && fc.suit >= 0) og_pinned_add(B, p, fc);
                        }
                    }
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
    if (og_adapt) {
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
    if (og_profile) {
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
            if (!og_set_contains(known, kn, c)) B->pool[B->n++] = c;
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
            if (!og_void_forbidden(B, g, p, B->pool[i])
                && !og_floor_forbidden(B, g, p, B->pool[i])) allowed++;
        }
        if (allowed < unknown && B->floor_v[p] > 0) {
            B->floor_v[p] = 0;
            allowed = 0;
            for (int i = 0; i < B->n; i++) {
                if (!og_void_forbidden(B, g, p, B->pool[i])) allowed++;
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
static _Thread_local int og_full_logs = 0;   // OG_FULL_LOGS=1: bp-style worlds

static void og_sample_world(Game *g_out, const Game *g_in, int my_idx,
                            const Belief *B, uint32_t seed,
                            bool apply_voids, bool apply_floors) {
    // OG_FULL_LOGS (research, bp-style worlds) needs the whole log array —
    // only possible when the world slot is full-size (WORLD_LOG_CAP == 0,
    // every native build); under short slots it degrades to the filter.
    if (og_full_logs && WORLD_LOG_CAP == 0) {
        game_clone(g_out, g_in);
    } else {
        memcpy(g_out, g_in, offsetof(Game, logs));
        int nl = 0;
        for (int i = 0; i < g_in->num_logs; i++) {
            if (g_in->logs[i].log_type != LOG_DISCARD) continue;
            if (WORLD_LOG_CAP > 0 && nl >= WORLD_LOG_CAP) break;  // crafted states only
            g_out->logs[nl++] = g_in->logs[i];
        }
        g_out->num_logs = nl;
        // Mark short slots so log_alloc applies the same discard filter to
        // rollout appends (see game.h); 0 on native keeps full capacity.
        g_out->log_cap  = WORLD_LOG_CAP;
        g_out->log_virt = (int16_t)nl;
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
        s = og_xorshift(s);
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
        bool bad = (use_v && og_void_forbidden(B, g_in, p, c))
                || (use_f && og_floor_forbidden(B, g_in, p, c));
        if (!bad) continue;
        for (int d = 0; d < deck_n; d++) {
            Card dc = g_out->deck[d];
            bool dc_bad = (use_v && og_void_forbidden(B, g_in, p, dc))
                       || (use_f && og_floor_forbidden(B, g_in, p, dc));
            if (!dc_bad) {
                g_out->deck[d] = c;
                g_out->players[p].hand[slots[si].slot] = dc;
                break;
            }
        }
    }
}

// ---------- exact solver (shared by root + rollout leaves) -----------------

#define OG_SOLVE_MAX_DEPTH   48
#define OG_SOLVE_MAX_MOVES   96
#define OG_SOLVE_BUDGET      200000L
#define OG_AVOID_BUDGET      150000L
#define OG_SOLVE_MAX_CARDS   20
#define OG_LEAF_BUDGET       1500L

typedef struct {
    long budget;
    bool aborted;
    int  me;
    SolveMoves *mv;      // [OG_SOLVE_MAX_DEPTH]
} Solver;

static _Thread_local SolveMoves *og_solver_mv = NULL;

_Static_assert(OG_SOLVE_MAX_DEPTH <= SOLVE_SCRATCH_DEPTH,
               "shared solver scratch shallower than this family's depth");
static bool og_solver_ready(void) {
    if (!og_solver_mv) {
        og_solver_mv = solve_scratch_mv();
    }
    return og_solver_mv != NULL;
}

// Value in [-1000, 1000] from `me`'s perspective: positive = me escaping,
// negative = me as durak. Magnitude prefers faster wins / slower losses.
static int og_solve(Solver *S, const Game *g, int alpha, int beta, int depth) {
    int loser = game_done(g);
    if (loser >= 0) return (loser == S->me) ? -(1000 - depth) : (1000 - depth);
    if (og_in_count(g) == 0) return 0;   // defensive: simultaneous out
    if (depth >= OG_SOLVE_MAX_DEPTH) { S->aborted = true; return 0; }
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
    if (mv->n > OG_SOLVE_MAX_MOVES) { S->aborted = true; return 0; }

    bool maximizing = (actor == S->me);
    int best = maximizing ? -2000 : 2000;
    for (int i = 0; i < mv->n; i++) {
        Game *child = solve_scratch_child(depth);
        solve_clone_prefix(child, g);
        if (!og_apply(child, actor, &mv->moves[i])) continue;
        int v = og_solve(S, child, alpha, beta, depth + 1);
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
static _Thread_local long og_leaf_budget = OG_LEAF_BUDGET;
static _Thread_local int  og_leaf_max_cards = 0;   // set from env at init
static _Thread_local int  og_floor_mod = 2;        // floors in 1/mod worlds

static int og_leaf_solve(const Game *g) {
    if (!og_solver_ready()) return -1;
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

    Game *root = solve_scratch_root();
    solve_clone_root(root, g);

    Solver S;
    S.budget  = og_leaf_budget;
    S.aborted = false;
    S.me      = me;
    S.mv      = og_solver_mv;

    // Null window around 0: only the sign matters (true values are ±(1000-d)
    // for decided games), and the narrow window maximizes pruning.
    int v = og_solve(&S, root, -1, 1, 0);
    if (S.aborted || v == 0) return -1;
    return (v > 0) ? opp : me;
}

// ---------- simulation ---------------------------------------------------

// Stage-aware rollout policy (gunpowder's rule): handwritten while the deck
// is alive or the game is heads-up, espresso for multi-player endgames.
static StrategyFn og_rollout_for(const Game *g) {
    // OG_ROLLOUT (struct path only): 1 = espresso everywhere, 2 = handwritten
    // everywhere. Research knob for the "rollout-policy bias" hypothesis — vs a
    // strong opponent, a weak (handwritten) rollout policy biases value
    // estimates, so more worlds saturates. A stronger rollout policy may reduce
    // that bias. Run with OG_NO_FASTROLL=1.
    if (og_rollout_policy == 1) return espresso_strategy_choose;
    if (og_rollout_policy == 2) return handwritten_strategy_choose;
    bool deck_active = (g->deck_count > 0 || g->has_flipped);
    if (deck_active || og_in_count(g) == 2) return handwritten_strategy_choose;
    return espresso_strategy_choose;
}

// Roll a sampled world forward; returns my finish position (1..N), or 0 if
// the simulation didn't terminate. Exits early once my position is known,
// and resolves small 2-player deck-empty endgames exactly (one attempt per
// rollout — a failed solve falls back to policy play for good).
static int og_simulate(Game *g, int my_idx, int max_turns) {
    int turns = 0;
    bool leaf_tried = false;
    while (game_done(g) < 0 && turns++ < max_turns) {
        // My fate is sealed as soon as I'm out: position = elimination slot.
        if (!og_no_earlyexit
            && g->players[my_idx].status != PLAYER_STATUS_IN) {
            for (int i = 0; i < g->num_eliminated; i++) {
                if (g->elimination_order[i] == my_idx) return i + 1;
            }
            break;   // not IN and not eliminated: corrupt state, bail
        }

        if (!og_no_leaf && !leaf_tried && g->deck_count == 0 && !g->has_flipped
            && og_in_count(g) == 2) {
            int total = 0;
            for (int i = 0; i < g->num_players; i++) {
                if (g->players[i].status == PLAYER_STATUS_IN)
                    total += g->players[i].hand_count;
            }
            if (total <= og_leaf_max_cards) {
                leaf_tried = true;
                int loser = og_leaf_solve(g);
                if (loser >= 0) {
                    // 2 left => positions N-1 and N remain.
                    return (loser == my_idx) ? g->num_players : g->num_players - 1;
                }
            }
        }

        bool acted = false;
        for (int pi = 0; pi < g->num_players; pi++) {
            if (!should_bot_act(g, pi)) continue;
            LegalMoves *moves = rollout_moves_scratch();
            calculate_legal_moves_lite(g, pi, moves);
            if (moves->n == 0) continue;
            StrategyFn fn = og_rollout_for(g);
            int idx = fn(g, pi, moves, NULL);
            if (idx < 0 || idx >= moves->n) continue;
            if (og_apply(g, pi, &moves->moves[idx])) { acted = true; break; }
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
// and play it out on bitmasks. ~10x faster per ply than og_simulate. The
// effective rollout policy is always handwritten (see cordite_sim.c), and the
// exact leaf solver (OG_LEAF, off by default) is not used in the fast path.
static int og_simulate_fast(const Game *g, int my_idx, int max_turns) {
    SimState s;
    cd_sim_from_game(&s, g);
    if (og_bbleaf_on || og_polmap)
        return cd_sim_playout_pol(&s, my_idx, max_turns, !og_no_earlyexit,
                                  og_bbleaf_on ? og_bbleaf_cards_eff : 0,
                                  og_bbleaf_budget, og_polmap);
    return cd_sim_playout(&s, my_idx, max_turns, !og_no_earlyexit);
}

// Dispatcher: bitboard rollout by default; struct rollout under OG_NO_FASTROLL
// or OG_LEAF (the leaf solver lives on the struct path). OG_DIFFTEST runs both
// and tallies divergences (printed at process exit by the eval harness if it
// hooks og_difftest_report, else just counted).
static _Thread_local long og_diff_total = 0, og_diff_mismatch = 0;
static int og_rollout(Game *g, int my_idx, int max_turns) {
    if (og_no_fastroll || !og_no_leaf) return og_simulate(g, my_idx, max_turns);
    if (og_difftest) {
        uint32_t rng0 = game_rng_get();
        int fast = og_simulate_fast(g, my_idx, max_turns);
        game_rng_set(rng0);
        Game *slow_g = rollout_scratch_diff();
        og_lite_clone(slow_g, g);
        int slow = og_simulate(slow_g, my_idx, max_turns);
        og_diff_total++;
        if (fast != slow) {
            og_diff_mismatch++;
            if (og_diff_mismatch <= 20) {
                fprintf(stderr, "OG_DIFFTEST mismatch #%ld: fast=%d slow=%d "
                        "(np=%d deck=%d logs=%d)\n", og_diff_mismatch, fast, slow,
                        g->num_players, g->deck_count, g->num_logs);
            }
        }
        return slow;  // keep slow behavior while difftesting
    }
    return og_simulate_fast(g, my_idx, max_turns);
}

void og_difftest_report(void) {
    if (og_diff_total > 0) {
        fprintf(stderr, "OG_DIFFTEST: %ld/%ld rollouts diverged (%.3f%%)\n",
                og_diff_mismatch, og_diff_total,
                100.0 * (double)og_diff_mismatch / (double)og_diff_total);
    }
}

// ---------- OG_EXPLAIN: deliberation dump (analysis build only) -------------
//
// COMPILED OUT of every normal build: the entire deliberation-dump machinery
// (this block, the solver-verdict probe, og_ex_emit, and the emit hooks in
// octogen_choose_impl) lives behind OG_EXPLAIN_BUILD, so the shipped native +
// wasm bots carry ZERO of it — no code-size cost. It is enabled only by the
// `make og_explain` analysis tool (c/tools/og_explain), which defines
// -DOG_EXPLAIN_BUILD.
//
// When built, set OG_EXPLAIN=1 (dump to stderr) or OG_EXPLAIN=/path/to/file to
// emit one JSON-lines record per deciding-seat decision: the seat's hand, the
// table, the candidate moves with their MC average-finish scores
// (score[i]/nsim[i]) OR the exact endgame-solver verdict per move (win/loss/
// draw/unknown), and the chosen move. Nothing runs when the var is unset.
#ifdef OG_EXPLAIN_BUILD
#ifdef CD_WASM_OVERLAY
// Freestanding snprintf/atol for the analysis-only wasm build (-nostdlib has no
// libc). Supports exactly what og_ex_emit uses: %d, %ld, %s, %c, %.Nf, %%.
// Returns bytes actually written (clamped), which keeps the `w += snprintf(...)`
// accumulators inside the 16 KiB record buffer.
#include <stdarg.h>
static long atol(const char *s) {
    long v = 0; int neg = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}
static int og_ex_putl(char *b, int cap, long v) {
    char t[24]; int n = 0, w = 0; unsigned long u;
    if (v < 0) { if (w < cap) b[w] = '-'; w++; u = (unsigned long)(-(v + 1)) + 1; }
    else u = (unsigned long)v;
    do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (n) { if (w < cap) b[w] = t[--n]; w++; }
    return w;
}
static int wasm_vsnprintf(char *buf, unsigned long n, const char *fmt, va_list ap) {
    int w = 0; int cap = (int)n - 1; if (cap < 0) cap = 0;
    for (; *fmt; fmt++) {
        if (*fmt != '%') { if (w < cap) buf[w] = *fmt; w++; continue; }
        fmt++;
        int prec = -1, islong = 0;
        if (*fmt == '.') { fmt++; prec = 0; while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0'); }
        if (*fmt == 'l') { islong = 1; fmt++; }
        if (*fmt == 'd') { long v = islong ? va_arg(ap, long) : va_arg(ap, int); w += og_ex_putl(buf + (w < cap ? w : cap), (w < cap ? cap - w : 0), v); }
        else if (*fmt == 's') { const char *s = va_arg(ap, const char *); for (; *s; s++) { if (w < cap) buf[w] = *s; w++; } }
        else if (*fmt == 'c') { char c = (char)va_arg(ap, int); if (w < cap) buf[w] = c; w++; }
        else if (*fmt == 'f') {
            double d = va_arg(ap, double); if (prec < 0) prec = 6;
            if (d < 0) { if (w < cap) buf[w] = '-'; w++; d = -d; }
            double scale = 1; for (int i = 0; i < prec; i++) scale *= 10;
            double rounded = d * scale + 0.5;
            long long whole = (long long)(rounded / scale);
            long long frac = (long long)rounded - whole * (long long)scale;
            w += og_ex_putl(buf + (w < cap ? w : cap), (w < cap ? cap - w : 0), (long)whole);
            if (prec > 0) {
                if (w < cap) buf[w] = '.'; w++;
                for (int i = prec - 1, ff = (int)frac; i >= 0; i--) {
                    int dv = 1; for (int j = 0; j < i; j++) dv *= 10;
                    if (w < cap) buf[w] = (char)('0' + (ff / dv) % 10); w++;
                }
            }
        } else if (*fmt == '%') { if (w < cap) buf[w] = '%'; w++; }
    }
    if ((int)n > 0) buf[w < cap ? w : cap] = 0;
    return w < cap ? w : cap;
}
static int snprintf(char *buf, unsigned long n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = wasm_vsnprintf(buf, n, fmt, ap); va_end(ap); return r;
}
#endif
enum { OG_EX_NONE_V = 2000001, OG_EX_UNKNOWN_V = 2000002, OG_EX_ILLEGAL_V = 2000003 };
static int   og_ex_solve_applied = 0;   // did the endgame solver fire this decision
static int   og_ex_verdict[600];        // exact per-root-move solver value / sentinel

#ifdef CD_WASM_OVERLAY
// wasm has no getenv/FILE*: the dump goes to a static buffer the JS harness reads
// via the exported accessors below. Always on in this (analysis-only) build.
static char og_ex_wbuf[1 << 20];
static int  og_ex_wlen = 0;
const char *wasm_og_explain_ptr(void)   { return og_ex_wbuf; }
int         wasm_og_explain_len(void)   { return og_ex_wlen; }
void        wasm_og_explain_reset(void) { og_ex_wlen = 0; }
static int og_explain_on(void) { return 1; }
#else
static FILE *og_ex_out = NULL;
static int   og_ex_on  = -1;            // -1 unloaded, 0 off, 1 on
static int og_explain_on(void) {
    if (og_ex_on < 0) {
        const char *e = getenv("OG_EXPLAIN");
        og_ex_on = (e && e[0] && strcmp(e, "0") != 0) ? 1 : 0;
        if (og_ex_on) {
            if (strcmp(e, "1") != 0) og_ex_out = fopen(e, "w");
            if (!og_ex_out) og_ex_out = stderr;
        }
    }
    return og_ex_on;
}
#endif
// og_ex_emit (the per-decision JSON dump) is defined after the Candidates
// typedef, just above octogen_choose_impl.
static void og_ex_emit(const Game *g, int bot_idx, const LegalMoves *moves,
                       const Belief *B, const void *C, const double *score, const int *nsim,
                       const bool *alive, const bool *forced_loss,
                       int chosen_idx, int solver_applied, int solver_win_idx);
#endif  // OG_EXPLAIN_BUILD

// ---------- root endgame solve (win take + loss avoid) ---------------------

// Solve every root move with a full window when 2 players remain and the
// deck is empty (the unseen pool IS the opponent's hand — public deduction).
// Returns the fastest forced-win index, or -1. When no win exists, sets
// forced_loss[i] for root moves that lose under optimal play; the MC stage
// avoids them whenever at least one non-losing move exists.
static int og_try_endgame_solve(const Game *g, int bot_idx,
                                const LegalMoves *moves, const Belief *B,
                                bool *forced_loss, int *n_safe) {
    *n_safe = moves->n;
    if (g->deck_count > 0 || g->has_flipped) return -1;
    if (og_in_count(g) != 2) return -1;
    if (g->players[bot_idx].status != PLAYER_STATUS_IN) return -1;

    int opp = -1;
    for (int i = 0; i < g->num_players; i++) {
        if (i != bot_idx && g->players[i].status == PLAYER_STATUS_IN) opp = i;
    }
    if (opp < 0) return -1;

    int unknown = g->players[opp].hand_count - B->pinned_n[opp];
    if (unknown < 0 || unknown != B->n) return -1;  // deduction failed; bail

    int total = g->players[bot_idx].hand_count + g->players[opp].hand_count;
    if (total > og_solve_cards) return -1;

    if (!og_solver_ready()) return -1;

    Game *root = solve_scratch_root();
    solve_clone_root(root, g);
    for (int k = 0; k < B->pinned_n[opp]; k++) {
        root->players[opp].hand[k] = B->pinned[opp][k];
    }
    for (int k = 0; k < B->n; k++) {
        root->players[opp].hand[B->pinned_n[opp] + k] = B->pool[k];
    }

    // Fast path: solve on the compact bitboard engine (transposition table +
    // O(1) clone + bitmask move-gen). The bitboard solver returns the exact
    // same value as the struct solver when resolved (validated by
    // tests/solver_difftest.c), and resolves more positions within budget.
    // OG_NO_BBSOLVE=1 falls back to the struct solver for A/B.
    bool bbsolve = !og_flag("OG_NO_BBSOLVE");
    SimState root_sim;
    if (bbsolve) cd_sim_from_game(&root_sim, root);
    cd_sim_solve_reset();

    // OG_EXPLAIN: record an EXACT full-window verdict for every root move
    // (win/loss/draw/unknown), for the deliberation dump. This is a pure probe
    // — it runs its own budget, then resets the transposition table so the real
    // pruned win-hunt below starts cold exactly as it would with the var unset.
    // Nothing here executes when OG_EXPLAIN is not set.
#ifdef OG_EXPLAIN_BUILD
    // The verdict probe runs a full exact solve per root move BEFORE the real
    // decision, to record win/loss/draw per move for the dump. It must be
    // behavior-neutral: the real decision reads the strategy RNG
    // (random_strategy_rng_get) to seed its Monte-Carlo world sampling, and
    // cd_sim_solve_d advances that RNG — so save it and restore it after, and
    // reset the transposition table, leaving the real solve/sampling exactly as
    // the shipped build would run it (verified: exact-pick reconstruction stays
    // 0-differ with this on).
    if (og_explain_on() && bbsolve) {
        og_ex_solve_applied = 1;
        uint32_t og_ex_rng_save = random_strategy_rng_get();
        int cap = (int)(sizeof(og_ex_verdict) / sizeof(og_ex_verdict[0]));
        for (int i = 0; i < moves->n && i < cap; i++) {
            SimState child = root_sim;
            if (!cd_sim_apply_root_move(&child, bot_idx, &moves->moves[i])) {
                og_ex_verdict[i] = OG_EX_ILLEGAL_V; continue;
            }
            int ab = 0;
            const char *ebs = getenv("OG_EXPLAIN_SOLVE_BUDGET");
            long eb = ebs ? atol(ebs) : 4000000L;
            int v = cd_sim_solve_d(&child, bot_idx, -2000, 2000, &eb, 1, &ab);
            og_ex_verdict[i] = (ab || eb <= 0) ? OG_EX_UNKNOWN_V : v;
        }
        cd_sim_solve_reset();                       // cold TT for the real solve
        random_strategy_set_seed(og_ex_rng_save);   // unperturbed strategy RNG
    }
#endif  // OG_EXPLAIN_BUILD

    Solver S;
    S.budget  = OG_SOLVE_BUDGET;
    S.aborted = false;
    S.me      = bot_idx;
    S.mv      = og_solver_mv;
    long win_budget   = bbsolve ? (long)og_bb_win_budget   : OG_SOLVE_BUDGET;
    long avoid_budget = bbsolve ? (long)og_bb_avoid_budget : OG_AVOID_BUDGET;
    long budget = win_budget;

    // Pass 1 — win hunt (blackpowder's loop): fail-soft with an accumulating
    // alpha floor at 0, so losing subtrees prune immediately.
    // OG_BP_SOLVE=1 reverts to blackpowder's exact semantics (alpha starts
    // wide open, any abort bails the whole solve) for A/B testing.
    int best_idx = -1;
    int best_v = 0;       // only accept strictly winning lines
    int alpha = og_flag("OG_BP_SOLVE") ? -2000 : 0;
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
            // Slot 0 is free here: og_solve starts the recursion at depth 1.
            Game *child = solve_scratch_child(0);
            solve_clone_prefix(child, root);
            if (!og_apply(child, bot_idx, &moves->moves[i])) continue;
            S.aborted = false;
            v = og_solve(&S, child, alpha, 2000, 1);
            aborted_i = S.aborted;
            if (S.budget <= 0) return -1;
        }
        if (aborted_i) { if (bail_on_abort) return -1; any_abort = true; continue; }
        if (v > best_v) { best_v = v; best_idx = i; }
        if (v > alpha) alpha = v;
    }
    if (best_idx >= 0) return best_idx;
    if (og_no_avoid || any_abort) return -1;
    if (total > og_avoid_cards) return -1;   // extended window: win hunt only

    // Pass 2 — loss avoidance: no win exists, so classify each move with a
    // null window around 0 (sign only, maximal pruning).
    S.budget = OG_AVOID_BUDGET;
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
            Game *child = solve_scratch_child(0);
            solve_clone_prefix(child, root);
            if (!og_apply(child, bot_idx, &moves->moves[i])) continue;
            S.aborted = false;
            v = og_solve(&S, child, -1, 0, 1);
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

#define OG_MAX_CANDS 26

typedef struct {
    int idx[OG_MAX_CANDS];
    int n;
} Candidates;

static void og_ranked_insert(int *idxs, double *keys, int *n, int cap,
                             int idx, double key) {
    int pos = *n;
    while (pos > 0 && keys[pos - 1] > key) pos--;
    if (pos >= cap) return;
    int last = (*n < cap) ? *n : cap - 1;
    for (int i = last; i > pos; i--) { idxs[i] = idxs[i - 1]; keys[i] = keys[i - 1]; }
    idxs[pos] = idx; keys[pos] = key;
    if (*n < cap) (*n)++;
}

static void og_pick_candidates(const Game *g, const LegalMoves *moves,
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
                for (int j = 0; j < m->n_cards; j++) sum += og_card_score(m->cards[j], power);
                og_ranked_insert(atk, atk_k, &n_atk, 12, i,
                                 -(double)m->n_cards * 10000.0 + (double)sum);
                break;
            }
            case MOVE_COVER: {
                double prod = 1.0;
                for (int j = 0; j < m->n_cards; j++) prod *= (double)og_card_score(m->cards[j], power);
                og_ranked_insert(cov, cov_k, &n_cov, 10, i,
                                 prod - (double)m->n_cards * 0.5);
                break;
            }
            case MOVE_PASS: {
                int sum = 0;
                for (int j = 0; j < m->n_cards; j++) sum += og_card_score(m->cards[j], power);
                og_ranked_insert(pas, pas_k, &n_pas, 3, i, (double)sum);
                break;
            }
            case MOVE_GOOD:   good_idx = i;   break;
            case MOVE_PICKUP: pickup_idx = i; break;
            default: break;
        }
    }

    out->n = 0;
    for (int i = 0; i < n_atk && out->n < OG_MAX_CANDS; i++) out->idx[out->n++] = atk[i];
    for (int i = 0; i < n_cov && out->n < OG_MAX_CANDS; i++) out->idx[out->n++] = cov[i];
    for (int i = 0; i < n_pas && out->n < OG_MAX_CANDS; i++) out->idx[out->n++] = pas[i];
    if (good_idx >= 0 && out->n < OG_MAX_CANDS)   out->idx[out->n++] = good_idx;
    if (pickup_idx >= 0 && out->n < OG_MAX_CANDS) out->idx[out->n++] = pickup_idx;
}

// ---------- main MC ---------------------------------------------------------

// Worlds per decision (W1: all candidates, W2: surviving third, W3: final
// top-2 duel). Compact worlds + early rollout exits buy ~2-3x blackpowder's
// sampling budget at comparable wall-clock.
static void og_params(int num_players, int *W1, int *W2, int *W3) {
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
    if (og_old_budget) {
        if (num_players <= 2)      { *W1 = 16; *W2 = 28; *W3 = 28; }
        else if (num_players <= 4) { *W1 = 14; *W2 = 28; *W3 = 28; }
        else if (num_players <= 6) { *W1 = 20; *W2 = 40; *W3 = 28; }
        else                       { *W1 = 20; *W2 = 40; *W3 = 24; }
    }
    if (og_oracle) { *W1 *= 6; *W2 *= 6; *W3 *= 6; }
    if (og_w1_override > 0) *W1 = og_w1_override;
    if (og_w2_override > 0) *W2 = og_w2_override;
    if (og_w3_override >= 0) *W3 = og_w3_override;
}

// OG_VERIFY=1: oracle self-check (test-only — reads real hands to validate
// the public-info belief, never to play).
static void og_verify_belief(const Game *g, int bot_idx, const Belief *B) {
    for (int p = 0; p < g->num_players; p++) {
        if (p == bot_idx) continue;
        const Player *pl = &g->players[p];
        for (int k = 0; k < B->pinned_n[p]; k++) {
            bool found = false;
            for (int j = 0; j < pl->hand_count; j++) {
                if (card_eq(pl->hand[j], B->pinned[p][k])) { found = true; break; }
            }
            if (!found) {
                fprintf(stderr, "OG_VERIFY: pinned card v%d s%d NOT in p%d hand (logs=%d)\n",
                        B->pinned[p][k].value, B->pinned[p][k].suit, p, g->num_logs);
            }
        }
        for (int j = 0; j < pl->hand_count; j++) {
            if (og_set_contains(B->pinned[p], B->pinned_n[p], pl->hand[j])) continue;
            if (B->void_n[p] > 0 && og_void_forbidden(B, g, p, pl->hand[j])) {
                fprintf(stderr, "OG_VERIFY: void violated: p%d holds v%d s%d\n",
                        p, pl->hand[j].value, pl->hand[j].suit);
            }
            if (og_floor_forbidden(B, g, p, pl->hand[j])) {
                fprintf(stderr, "OG_VERIFY: floor violated: p%d holds v%d s%d floor=%d\n",
                        p, pl->hand[j].value, pl->hand[j].suit, B->floor_v[p]);
            }
            if (!og_set_contains(B->pool, B->n, pl->hand[j])) {
                fprintf(stderr, "OG_VERIFY: p%d unknown card v%d s%d missing from pool (logs=%d)\n",
                        p, pl->hand[j].value, pl->hand[j].suit, g->num_logs);
            }
        }
    }
}

// ---------- OG_EXPLAIN emit (defined here: needs Candidates + MOVE_*) -------
#ifdef OG_EXPLAIN_BUILD
static const char *OG_EX_VAL[14] = {
    "?","2","3","4","5","6","7","8","9","10","J","Q","K","A"
};
// Canonical durak rank convention (wire v13 = A), trump marked with '*'.
static void og_ex_fmt_card(char *buf, size_t n, Card c, int trump) {
    const char *v = (c.value >= 1 && c.value <= 13) ? OG_EX_VAL[c.value] : "?";
    char s = (c.suit >= 0 && c.suit < 4) ? "SHCD"[(int)c.suit] : '?';
    snprintf(buf, n, "%s%c%s", v, s, (c.suit == trump) ? "*" : "");
}
static int og_ex_cards_json(char *out, size_t cap, const Card *cards, int n, int trump) {
    int w = 0;
    w += snprintf(out + w, cap - w, "[");
    for (int i = 0; i < n; i++) {
        char cb[8]; og_ex_fmt_card(cb, sizeof cb, cards[i], trump);
        w += snprintf(out + w, cap - w, "%s\"%s\"", i ? "," : "", cb);
    }
    w += snprintf(out + w, cap - w, "]");
    return w;
}
static const char *OG_EX_MTYPE[6] = { "attack","cover","pass","pickup","good","wait" };
static void og_ex_move_label(char *label, size_t cap, const LegalMove *m, int trump) {
    const char *mt = (m->type >= 0 && m->type < 6) ? OG_EX_MTYPE[m->type] : "?";
    if (m->type == MOVE_COVER && m->n_cards >= 1) {
        int w = snprintf(label, cap, "cover");
        for (int k = 0; k < m->n_cards; k++) {
            char cc[8], tt[8];
            og_ex_fmt_card(cc, sizeof cc, m->cards[k], trump);
            og_ex_fmt_card(tt, sizeof tt, m->attack_cards[k], trump);
            w += snprintf(label + w, cap - w, "%s%s->%s", k ? "," : " ", cc, tt);
        }
    } else if (m->type == MOVE_PICKUP) {
        snprintf(label, cap, "pickup");
    } else if (m->type == MOVE_GOOD) {
        snprintf(label, cap, "good");
    } else {
        char cl[64]; cl[0] = 0; int cw = 0;
        for (int c = 0; c < m->n_cards; c++) {
            char cb[8]; og_ex_fmt_card(cb, sizeof cb, m->cards[c], trump);
            cw += snprintf(cl + cw, sizeof(cl) - cw, "%s%s", c ? " " : "", cb);
        }
        snprintf(label, cap, "%s %s", mt, cl);
    }
}
// Emit one decision record as a JSON line. score/nsim/alive/forced_loss/Cv may
// be NULL (endgame-win and single-move paths). score[k]/nsim[k]/alive[k] index
// by CANDIDATE position k (matching C->idx[k]); verdicts/forced_loss index by
// the move index into moves->moves[].
static void og_ex_emit(const Game *g, int bot_idx, const LegalMoves *moves,
                       const Belief *B, const void *Cv, const double *score, const int *nsim,
                       const bool *alive, const bool *forced_loss,
                       int chosen_idx, int solver_applied, int solver_win_idx) {
    (void)solver_win_idx;
#ifndef CD_WASM_OVERLAY
    if (!og_ex_out) return;
#endif
    const Candidates *C = (const Candidates *)Cv;
    int trump = g->power_suit;
#ifdef FOOLISH_ORACLE_BUILD
    // §6.3: the oracle appends verdict entries for pruned moves; heads-up
    // ≤28-card positions can enumerate ~75-97 root moves, pushing a record past
    // the 16 KiB staging cap where the freestanding snprintf TRUNCATES (not
    // drops) into malformed JSON. Give the oracle build a big file-scope buffer
    // (og_ex_emit is non-recursive, single-threaded in Mode A).
    static char buf[65536]; int w = 0;
#else
    char buf[16384]; int w = 0;
#endif
    #define OGA(...) do { w += snprintf(buf + w, sizeof(buf) - w, __VA_ARGS__); } while (0)

    OGA("{\"ply\":%d,\"seat\":%d,\"deck\":%d,\"defender\":%d,\"trump\":%d,\"nlogs\":%d",
        g->num_logs, bot_idx, g->deck_count, g->defender, trump, g->num_logs);

    // octogen's actual belief about the hidden cards (public deduction): the
    // cards it has PINNED to each opponent's hand (watched them pick up, not yet
    // replayed) vs the genuinely-unknown pool (deck + un-pinned opponent cards),
    // plus demonstrated suit voids and rank floors. This is what octogen knows,
    // straight from og_build_belief — not reconstructed downstream.
    if (B) {
        OGA(",\"belief\":{\"pinned\":[");
        for (int p = 0; p < g->num_players; p++) {
            OGA("%s", p ? "," : "");
            w += og_ex_cards_json(buf + w, sizeof(buf) - w, B->pinned[p], B->pinned_n[p], trump);
        }
        OGA("],\"pool\":");
        w += og_ex_cards_json(buf + w, sizeof(buf) - w, B->pool, B->n, trump);
        // voids[p] = attack cards p demonstrably could NOT cover at a pickup, so
        // p holds nothing that beats them (a deduced constraint, not a card).
        OGA(",\"voids\":[");
        for (int p = 0; p < g->num_players; p++) {
            OGA("%s", p ? "," : "");
            w += og_ex_cards_json(buf + w, sizeof(buf) - w, B->voids[p], B->void_n[p], trump);
        }
        OGA("],\"floor\":[");
        for (int p = 0; p < g->num_players; p++) OGA("%s%d", p ? "," : "", B->floor_v[p]);
        OGA("]}");
    }

    OGA(",\"hand\":");
    { const Player *pl = &g->players[bot_idx];
      w += og_ex_cards_json(buf + w, sizeof(buf) - w, pl->hand, pl->hand_count, trump); }
    OGA(",\"hand_count\":%d", g->players[bot_idx].hand_count);
    OGA(",\"opp_counts\":[");
    for (int p = 0; p < g->num_players; p++) OGA("%s%d", p ? "," : "", g->players[p].hand_count);
    OGA("]");

    OGA(",\"table\":[");
    for (int b = 0; b < g->num_battles; b++) {
        char ab[8], db[8];
        og_ex_fmt_card(ab, sizeof ab, g->table_battles[b].attack, trump);
        bool cov = !card_is_none(g->table_battles[b].defense);
        if (cov) { og_ex_fmt_card(db, sizeof db, g->table_battles[b].defense, trump);
                   OGA("%s{\"attack\":\"%s\",\"defense\":\"%s\"}", b ? "," : "", ab, db); }
        else       OGA("%s{\"attack\":\"%s\",\"defense\":null}", b ? "," : "", ab);
    }
    OGA("]");

    const char *sres = !solver_applied ? "none" : (chosen_idx >= 0 && solver_win_idx >= 0 ? "win" : "eval");
    OGA(",\"solver\":{\"applied\":%d,\"result\":\"%s\"}", solver_applied ? 1 : 0, sres);

    int ncand = C ? C->n : moves->n;
    OGA(",\"candidates\":[");
    for (int k = 0; k < ncand; k++) {
        int mi = C ? C->idx[k] : k;
        const LegalMove *m = &moves->moves[mi];
        const char *mt = (m->type >= 0 && m->type < 6) ? OG_EX_MTYPE[m->type] : "?";
        char label[96]; og_ex_move_label(label, sizeof label, m, trump);
        OGA("%s{\"type\":\"%s\",\"label\":\"%s\",\"cards\":", k ? "," : "", mt, label);
        w += og_ex_cards_json(buf + w, sizeof(buf) - w, m->cards, m->n_cards, trump);
        if (m->type == MOVE_COVER) {
            OGA(",\"target\":");
            w += og_ex_cards_json(buf + w, sizeof(buf) - w, m->attack_cards, m->n_cards, trump);
        }
        if (score && nsim && nsim[k] > 0)
            OGA(",\"score\":%.4f,\"nsim\":%d", score[k] / (double)nsim[k], nsim[k]);
        else
            OGA(",\"score\":null,\"nsim\":%d", nsim ? nsim[k] : 0);
        OGA(",\"alive\":%d", (alive ? (alive[k] ? 1 : 0) : 1));
        OGA(",\"forced_loss\":%d", (forced_loss ? (forced_loss[mi] ? 1 : 0) : 0));
        const char *vd = "none"; int vv = 0, has_vv = 0;
        if (solver_applied && mi < (int)(sizeof(og_ex_verdict)/sizeof(int))) {
            int val = og_ex_verdict[mi];
            if (val == OG_EX_UNKNOWN_V) vd = "unknown";
            else if (val == OG_EX_ILLEGAL_V) vd = "illegal";
            else if (val == OG_EX_NONE_V) vd = "none";
            else { has_vv = 1; vv = val; vd = (val > 0) ? "win" : (val < 0) ? "loss" : "draw"; }
        }
        OGA(",\"verdict\":\"%s\"", vd);
        if (has_vv) OGA(",\"verdict_val\":%d", vv);
        OGA(",\"chosen\":%d}", (mi == chosen_idx) ? 1 : 0);
    }
#ifdef FOOLISH_ORACLE_BUILD
    // §6.3: in the exact regime, the move the player actually made may be
    // excluded from candidacy as a proven loss — the very move a reviewer most
    // wants judged. The probe already filled og_ex_verdict[mi] by MOVE index;
    // emit a scoreless, pruned entry for every legal move with a PROVEN verdict
    // (win/loss/draw) that is not already a candidate. C==NULL means the
    // candidate set IS every move, so nothing is pruned; the MC regime
    // (!solver_applied) emits none — mid-game menus can be thousands of moves.
    if (solver_applied && C) {
        int k2 = ncand;
        for (int mi = 0; mi < moves->n; mi++) {
            int is_cand = 0;
            for (int k = 0; k < C->n; k++) if (C->idx[k] == mi) { is_cand = 1; break; }
            if (is_cand) continue;
            if (mi >= (int)(sizeof(og_ex_verdict)/sizeof(int))) continue;
            int val = og_ex_verdict[mi];
            if (val == OG_EX_NONE_V || val == OG_EX_UNKNOWN_V || val == OG_EX_ILLEGAL_V) continue;
            const LegalMove *m = &moves->moves[mi];
            const char *mt = (m->type >= 0 && m->type < 6) ? OG_EX_MTYPE[m->type] : "?";
            char label[96]; og_ex_move_label(label, sizeof label, m, trump);
            OGA("%s{\"type\":\"%s\",\"label\":\"%s\",\"cards\":", k2 ? "," : "", mt, label);
            w += og_ex_cards_json(buf + w, sizeof(buf) - w, m->cards, m->n_cards, trump);
            if (m->type == MOVE_COVER) {
                OGA(",\"target\":");
                w += og_ex_cards_json(buf + w, sizeof(buf) - w, m->attack_cards, m->n_cards, trump);
            }
            const char *vd = (val > 0) ? "win" : (val < 0) ? "loss" : "draw";
            OGA(",\"score\":null,\"nsim\":0,\"alive\":0,\"pruned\":1,\"forced_loss\":%d",
                (forced_loss ? (forced_loss[mi] ? 1 : 0) : 0));
            OGA(",\"verdict\":\"%s\",\"verdict_val\":%d,\"chosen\":%d}",
                vd, val, (mi == chosen_idx) ? 1 : 0);
            k2++;
        }
    }
#endif
    OGA("]");

    { char label[96] = "?";
      if (chosen_idx >= 0 && chosen_idx < moves->n)
          og_ex_move_label(label, sizeof label, &moves->moves[chosen_idx], trump);
      OGA(",\"chosen\":\"%s\"}", label); }
    #undef OGA
#ifdef CD_WASM_OVERLAY
#ifdef FOOLISH_ORACLE_BUILD
    // §6.3: the freestanding snprintf clamps (saturating w at sizeof(buf)-1)
    // rather than failing, so a record that hit the staging cap is truncated
    // malformed JSON. Detect the saturation and append an explicit overflow
    // marker the worker recognizes (§8.5 step 9) instead of a broken line.
    if (w >= (int)sizeof(buf) - 1) {
        static const char ov[] = "{\"overflow\":1}";
        int ol = (int)sizeof(ov) - 1;
        if (og_ex_wlen + ol + 1 < (int)sizeof(og_ex_wbuf)) {
            for (int i = 0; i < ol; i++) og_ex_wbuf[og_ex_wlen++] = ov[i];
            og_ex_wbuf[og_ex_wlen++] = '\n';
        }
    } else
#endif
    // append "buf\n" to the JS-readable buffer (drop the record if it won't fit)
    if (og_ex_wlen + w + 1 < (int)sizeof(og_ex_wbuf)) {
        for (int i = 0; i < w; i++) og_ex_wbuf[og_ex_wlen++] = buf[i];
        og_ex_wbuf[og_ex_wlen++] = '\n';
    }
#else
    fputs(buf, og_ex_out);
    fputc('\n', og_ex_out);
    fflush(og_ex_out);
#endif
}
#endif  // OG_EXPLAIN_BUILD

static int octogen_choose_impl(const Game *g, int bot_idx,
                            const LegalMoves *moves, void *ctx) {
    (void)ctx;
    if (moves->n == 0) return -1;
    if (moves->n == 1) {
#ifdef OG_EXPLAIN_BUILD
        if (og_explain_on()) {
            og_ex_solve_applied = 0;
            Belief lb; og_build_belief(g, bot_idx, &lb);   // for the known-state dump only
            og_ex_emit(g, bot_idx, moves, &lb, NULL, NULL, NULL, NULL, NULL, 0, 0, -1);
        }
#endif
        return 0;
    }

    if (!og_flags_loaded) {
#ifdef OG_HIDE_UNCOVERABLE
        og_hide_mask = og_env_int("OG_HIDE_MASK", 0);
#endif
        og_no_solve  = og_flag("OG_NO_SOLVE");
        og_no_voids  = og_flag("OG_NO_VOIDS");
        og_no_flip   = og_flag("OG_NO_FLIP");
        og_no_floors = og_flag("OG_NO_FLOORS");
        // Exact leaf endgames in rollouts are OFF by default: measured both
        // slower and weaker vs the real (imperfect) opponent pool — modeling
        // actual opponents beats assuming perfect play. OG_LEAF=1 re-enables.
        og_no_leaf   = !og_flag("OG_LEAF");
        og_no_avoid  = og_flag("OG_NO_AVOID");
        og_verify    = og_flag("OG_VERIFY");
        og_w1_override = og_env_int("OG_W1", 0);
        og_w2_override = og_env_int("OG_W2", 0);
        og_w3_override = og_env_int("OG_W3", -1);
        og_keep1 = og_env_int("OG_KEEP1", 0);
        og_keep2 = og_env_int("OG_KEEP2", 0);
        og_rollout_policy = og_env_int("OG_ROLLOUT", 0);
        og_trump_keep = og_env_int("OG_TRUMP_KEEP", 40) / 1000.0;
        og_bb_win_budget = og_env_int("OG_BB_WIN", 400000);
        og_solve_cards = og_env_int("OG_SOLVE_CARDS", 28);
        og_avoid_cards = og_env_int("OG_AVOID_CARDS", 24);
        og_bb_avoid_budget = og_env_int("OG_BB_AVOID", 250000);
        og_leaf_budget = og_env_int("OG_LEAF_BUDGET", 1500);
        og_leaf_max_cards = og_env_int("OG_LEAF_CARDS", 10);
        og_floor_mod = og_env_int("OG_FLOOR_MOD", 2);
        if (og_floor_mod < 1) og_floor_mod = 1;
        og_full_logs = og_flag("OG_FULL_LOGS");
        og_no_earlyexit = og_flag("OG_NO_EARLYEXIT");
        og_no_fastroll = og_flag("OG_NO_FASTROLL");
        og_bbleaf = og_env_int("OG_BBLEAF", 2);
        og_adapt = og_env_int("OG_ADAPT", 1);
        og_reply = og_env_int("OG_REPLY", 0);
        og_mcdef = og_env_int("OG_MCDEF", 0);
        og_reply_cap = og_env_int("OG_REPLY_CAP", 6);
        og_reply_stage = og_env_int("OG_REPLY_STAGE", 2);
        og_self_depth = og_env_int("OG_SELF_DEPTH", 1);
        og_self_cap = og_env_int("OG_SELF_CAP", 6);
        og_self_plies = og_env_int("OG_SELF_PLIES", 0);
        og_self_win = og_env_int("OG_SELF_WIN", 1);
        og_self_stage = og_env_int("OG_SELF_STAGE", 0);
        og_self_leaf_cards = og_env_int("OG_SELF_LEAF_CARDS", -1);
        og_self_leaf_budget = og_env_int("OG_SELF_LEAF_BUDGET", -1);
        og_void_mod = og_env_int("OG_VOID_MOD", 4);
        if (og_void_mod < 2) og_void_mod = 2;
        og_profile = og_env_int("OG_PROFILE", 0);
        og_bbleaf_cards = og_env_int("OG_BBLEAF_CARDS", 0);
        og_bbleaf_cards2 = og_env_int("OG_BBLEAF_CARDS2", 8);
        og_bbleaf_budget = og_env_int("OG_BBLEAF_BUDGET", 3000);
        og_difftest = og_flag("OG_DIFFTEST");
        if (og_difftest) { void og_difftest_report(void); atexit(og_difftest_report); }
        og_flags_loaded = 1;
    }

    uint32_t saved_rng = game_rng_get();

    Belief B;
    og_build_belief(g, bot_idx, &B);
    if (og_no_voids) for (int p = 0; p < MAX_PLAYERS; p++) B.void_n[p] = 0;
    if (og_verify) og_verify_belief(g, bot_idx, &B);

    // Player-count gate for the leaf lever (see og_bbleaf comment). In the
    // pc-aware default mode heads-up uses the small-leaf threshold.
    og_bbleaf_on = (og_bbleaf != 0);
    og_bbleaf_cards_eff = (og_bbleaf == 2 && g->num_players == 2)
                        ? og_bbleaf_cards2 : og_bbleaf_cards;

    // Per-seat rollout policies: profiled-weak seats get the LOOSE model;
    // proven-strategic seats (mc_tell) get the MC-defender model.
    og_polmap = NULL;
    {
        bool any = false;
        for (int p = 0; p < g->num_players; p++) {
            og_polmap_buf[p] = CD_POL_HW;
            if (og_profile && B.loose[p]) { og_polmap_buf[p] = CD_POL_LOOSE; any = true; }
            else if (og_mcdef && B.mc_tell[p] && p != bot_idx) {
                og_polmap_buf[p] = CD_POL_MCDEF; any = true;
            }
        }
        if (any) og_polmap = og_polmap_buf;
    }

    // Exact endgame: take a proven win; mark proven losses for exclusion.
    bool *forced_loss = forced_loss_scratch();
    memset(forced_loss, 0, (size_t)moves->n * sizeof(bool));
    int n_safe = moves->n;
#ifdef OG_EXPLAIN_BUILD
    if (og_explain_on()) {
        og_ex_solve_applied = 0;
        for (int i = 0; i < (int)(sizeof(og_ex_verdict)/sizeof(og_ex_verdict[0])); i++)
            og_ex_verdict[i] = OG_EX_NONE_V;
    }
#endif
    int solved = og_no_solve ? -1
               : og_try_endgame_solve(g, bot_idx, moves, &B, forced_loss, &n_safe);
    if (solved >= 0) {
#ifdef OG_EXPLAIN_BUILD
        if (og_explain_on()) {
            Candidates EC; og_pick_candidates(g, moves, forced_loss, &EC);
            og_ex_emit(g, bot_idx, moves, &B, &EC, NULL, NULL, NULL, forced_loss,
                       solved, og_ex_solve_applied, solved);
        }
#endif
        game_rng_set(saved_rng);
        return solved;
    }

    Candidates C;
    og_pick_candidates(g, moves, forced_loss, &C);
    if (C.n == 0) {
        // Everything we'd consider is a proven loss; fall back to all moves.
        memset(forced_loss, 0, (size_t)moves->n * sizeof(bool));
        og_pick_candidates(g, moves, forced_loss, &C);
    }
    if (C.n == 0) {
#ifdef OG_EXPLAIN_BUILD
        if (og_explain_on())
            og_ex_emit(g, bot_idx, moves, &B, &C, NULL, NULL, NULL, forced_loss,
                       0, og_ex_solve_applied, -1);
#endif
        game_rng_set(saved_rng); return 0;
    }
    if (C.n == 1) {
#ifdef OG_EXPLAIN_BUILD
        if (og_explain_on())
            og_ex_emit(g, bot_idx, moves, &B, &C, NULL, NULL, NULL, forced_loss,
                       C.idx[0], og_ex_solve_applied, -1);
#endif
        game_rng_set(saved_rng); return C.idx[0];
    }

    int W1, W2, W3;
    og_params(g->num_players, &W1, &W2, &W3);

    // World-sampling seed. Depends ONLY on the SERVER-ONLY secret — the strategy
    // LCG, reseeded per decision from state_fnv(g_rng_base) (see wasm_api.c) —
    // and the deciding seat. It deliberately folds in NO other game state.
    // Rationale: anything that seeds the RNG but is NOT recoverable from a shared
    // replay makes a recorded game impossible to reproduce bit-exactly. num_logs
    // (records the codec may drop), the ordered hands and the face-down deck are
    // all either non-reproducible or hidden from the bot; a player could even
    // perturb them (hand rearrange). Unpredictability still rests entirely on the
    // secret deal seed — a source holder can't recompute it — and the W diverse
    // wseeds within a decision come from (w+1) below, so MC coverage is intact.
    // bot_idx only decorrelates co-seated bots in multi-bot games.
    uint32_t base = og_mix((uint32_t)bot_idx * 2654435761u, random_strategy_rng_get());

    double score[OG_MAX_CANDS] = {0};
    int    nsim [OG_MAX_CANDS] = {0};
    bool   alive[OG_MAX_CANDS];
    for (int i = 0; i < C.n; i++) alive[i] = true;

    // Trump-conservation tie-break tax (added to a candidate's mean-finish score
    // ONLY at selection; the raw MC scores in score[]/nsim[] are left untouched so
    // the OG_EXPLAIN dump still shows true rollout values). Attacks only, deck
    // alive only — leading/throwing a trump is discretionary spend; covering with
    // one is forced defense and endgame trumps must be used.
    double keep_pen[OG_MAX_CANDS];
    {
        bool deck_alive = (g->deck_count > 0 || g->has_flipped);
        for (int i = 0; i < C.n; i++) {
            keep_pen[i] = 0.0;
            if (og_trump_keep > 0.0 && deck_alive) {
                const LegalMove *m = &moves->moves[C.idx[i]];
                if (m->type == MOVE_ATTACK)
                    for (int k = 0; k < m->n_cards; k++)
                        if (m->cards[k].suit == g->power_suit)
                            keep_pen[i] += og_trump_keep;
            }
        }
    }

    Game *world = world_scratch_game(), *trial = trial_scratch_game();
    SimState *world_sim = world_scratch_sim(), *trial_sim = trial_scratch_sim();

    // The fast bitboard path: convert each sampled WORLD to a compact SimState
    // ONCE, then each candidate just clones the SimState, applies its move on
    // bitboards, and plays out. The struct path (OG_NO_FASTROLL / OG_LEAF /
    // OG_DIFFTEST) keeps the per-candidate Game clone for the leaf solver and
    // the exact-equivalence difftest.
    bool fast_path = !og_no_fastroll && og_no_leaf && !og_difftest && !og_flag("OG_NO_WORLDSIM");

    // Stage 1: all candidates on W1 shared worlds.
    // Stage 2: surviving third on W2 more shared worlds.
    // Stage 3: top 2 duel on W3 final shared worlds.
    for (int stage = 0; stage < 3; stage++) {
        int w_lo = (stage == 0) ? 0 : (stage == 1) ? W1 : W1 + W2;
        int w_hi = (stage == 0) ? W1 : (stage == 1) ? W1 + W2 : W1 + W2 + W3;
        for (int w = w_lo; w < w_hi; w++) {
            uint32_t wseed = og_mix(base, (uint32_t)(w + 1) * 0x85EBCA77u);
            // Belief mixture: voids assume cover-if-you-can pickups (3 of 4
            // worlds), floors assume lowest-first attackers (every other
            // world). Per-player distrust already cleared bogus constraints.
            bool use_voids  = (w % og_void_mod) != og_void_mod - 1;
            bool use_floors = !og_no_floors && (w % og_floor_mod) == 0;
            og_sample_world(world, g, bot_idx, &B, wseed, use_voids, use_floors);
            uint32_t sim_rng = og_mix(wseed, 0x51AB1E5u);

            if (fast_path) {
                cd_sim_from_game(world_sim, world);     // convert world ONCE
                bool reply_stage = og_reply && stage >= og_reply_stage;
                bool self_stage = og_self && stage >= og_self_stage;
                for (int ci = 0; ci < C.n; ci++) {
                    if (!alive[ci]) continue;
                    *trial_sim = *world_sim;            // cheap struct copy
                    game_rng_set(sim_rng);              // identical stream
                    int fp;
                    if (!cd_sim_apply_root_move(trial_sim, bot_idx,
                                                &moves->moves[C.idx[ci]])) {
                        fp = g->num_players;
                    } else if (self_stage) {
                        int slc = og_self_leaf_cards >= 0 ? og_self_leaf_cards
                                : (og_bbleaf_on ? og_bbleaf_cards_eff : 0);
                        long slb = og_self_leaf_budget >= 0 ? og_self_leaf_budget
                                 : og_bbleaf_budget;
                        fp = cd_sim_playout_self(trial_sim, bot_idx, 600,
                                                 slc, slb, og_polmap,
                                                 og_self_depth, og_self_cap,
                                                 og_self_plies, og_self_win);
                        if (fp == 0) fp = g->num_players;
                    } else if (reply_stage) {
                        fp = cd_sim_playout_reply(trial_sim, bot_idx, 600,
                                                  og_bbleaf_on ? og_bbleaf_cards_eff : 0,
                                                  og_bbleaf_budget, og_polmap,
                                                  og_reply_cap);
                        if (fp == 0) fp = g->num_players;
                    } else {
                        fp = (og_bbleaf_on || og_polmap)
                           ? cd_sim_playout_pol(trial_sim, bot_idx, 600,
                                                !og_no_earlyexit,
                                                og_bbleaf_on ? og_bbleaf_cards_eff : 0,
                                                og_bbleaf_budget, og_polmap)
                           : cd_sim_playout(trial_sim, bot_idx, 600, !og_no_earlyexit);
                        if (fp == 0) fp = g->num_players;
                    }
                    score[ci] += (double)fp;
                    nsim[ci]++;
                }
                continue;
            }

            for (int ci = 0; ci < C.n; ci++) {
                if (!alive[ci]) continue;
                og_lite_clone(trial, world);
                game_rng_set(sim_rng);   // identical stream for every move
                if (!og_apply(trial, bot_idx, &moves->moves[C.idx[ci]])) {
                    score[ci] += (double)g->num_players;
                    nsim[ci]++;
                    continue;
                }
                int fp = og_rollout(trial, bot_idx, 600);
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
                keep = (og_keep1 > 0) ? og_keep1 : (og_oracle ? (C.n + 1) / 2 : C.n / 3);
                if (keep < (og_oracle ? 4 : 3)) keep = og_oracle ? 4 : 3;
            } else {
                keep = (og_keep2 > 0) ? og_keep2 : (og_oracle ? 3 : 2);
            }
            if (keep >= n_alive) continue;
            for (int dropped = n_alive - keep; dropped > 0; dropped--) {
                int worst = -1;
                double worst_v = -1e30;
                for (int i = 0; i < C.n; i++) {
                    if (!alive[i]) continue;
                    double v = score[i] / (double)(nsim[i] ? nsim[i] : 1) + keep_pen[i];
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
        double v = score[i] / (double)nsim[i] + keep_pen[i];
        if (v < best_v) { best_v = v; best = i; }
    }

    int chosen = best >= 0 ? C.idx[best] : 0;
    // Information-hiding rule (docs/OCTOGEN_HIDE_UNCOVERABLE.md). If octogen is
    // about to play a COVER while it cannot cover EVERY uncovered card on the
    // table (no full cover exists in the legal set), pick up immediately. A
    // doomed partial cover only leaks its cover cards — returned to hand and
    // logged by the pickup that is coming anyway — to the memory-keeping MC
    // opponents, and covering opens throw-in windows that grow the pickup.
    // Weakly dominant: never costs a card or tempo. ON for every seat in the
    // shipped build; -DOG_HIDE_UNCOVERABLE instead gates it per-seat via
    // OG_HIDE_MASK (and counts fires) so the A/B eval can isolate one seat.
    if (chosen >= 0 && chosen < moves->n && moves->moves[chosen].type == MOVE_COVER
#ifdef OG_HIDE_UNCOVERABLE
        && ((og_hide_mask >> bot_idx) & 1)
#endif
       ) {
        int n_uncov = 0;
        for (int i = 0; i < g->num_battles; i++)
            if (card_is_none(g->table_battles[i].defense)) n_uncov++;
        int full_cover = 0, pickup_idx = -1;
        for (int i = 0; i < moves->n; i++) {
            if (moves->moves[i].type == MOVE_COVER && moves->moves[i].n_cards == n_uncov)
                full_cover = 1;
            else if (moves->moves[i].type == MOVE_PICKUP)
                pickup_idx = i;
        }
        if (!full_cover && pickup_idx >= 0) {
            chosen = pickup_idx;
#ifdef OG_HIDE_UNCOVERABLE
            og_hide_fire_count++;
#endif
        }
    }
#ifdef OG_EXPLAIN_BUILD
    if (og_explain_on())
        og_ex_emit(g, bot_idx, moves, &B, &C, score, nsim, alive, forced_loss,
                   chosen, og_ex_solve_applied, -1);
#endif
    game_rng_set(saved_rng);
    return chosen;
}

// Oracle semtex: the same brain with 6x the sampled-world budget and wider
// candidate survival. Research-only — used to audit losses: where the oracle
// picks a different move, the default budget was the binding constraint.
// octogen's public entry: octogen's brain PLUS the LEAFBOOK endgame oracle
// (c/LEAFBOOK.md, docs/L1_SPEND_PLAN.md §4). A book hit terminates a
// round-boundary ≤K-card subtree with a proven-exact value, so octogen resolves
// the same lines with fewer solver nodes — ~15% faster e2e p50 (measured in the
// wasm bot bench), strength-neutral (the book only shifts play through the
// shared node-budget channel, like a bigger TT; 4,000-game paired vs espresso:
// wash). No-op unless built -DCD_LEAFBOOK.
int octogen_strategy_choose(const Game *g, int bot_idx,
                            const LegalMoves *moves, void *ctx) {
    cd_sim_set_leafbook(1);
    int r = octogen_choose_impl(g, bot_idx, moves, ctx);
    cd_sim_set_leafbook(0);
    return r;
}

int octogen_oracle_strategy_choose(const Game *g, int bot_idx,
                                  const LegalMoves *moves, void *ctx) {
    og_oracle = 1;
    int r = octogen_strategy_choose(g, bot_idx, moves, ctx);
    og_oracle = 0;
    return r;
}

// octogen_self: octogen with ITSELF (approximated) as the rollout policy —
// the "infinite oracle" recursion, hunt 5 (OG_SELF_* knobs; see
// cd_sim_playout_self in cordite_sim.c and OCTOGEN.md). Research-only: the
// nested tournament multiplies playout cost by ~(cap x plies)^depth, and
// inside a determinized world the recursion limit is perfect full-info play
// of that world — the opponent model CORDITE.md's leaf study and the OG_REPLY
// hunt both measured as worse than modeling the real, imperfect opponent.
// A separate strategy id (not an env gate on octogen) so the paired
// --control harness can run it against plain octogen in one process.
// Requires the fast bitboard path (the default; OG_NO_FASTROLL/OG_LEAF/
// OG_DIFFTEST silently fall back to the plain rollout).
int octogen_self_strategy_choose(const Game *g, int bot_idx,
                                 const LegalMoves *moves, void *ctx) {
    og_self = 1;
    int r = octogen_strategy_choose(g, bot_idx, moves, ctx);
    og_self = 0;
    return r;
}
