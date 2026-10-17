// Octogen — semtex's successor: the same engine plus a stage-3 REPLY
// TOURNAMENT (the first opponent reply in each final-duel world is chosen
// by search over their legal replies, not assumed from the rollout policy).
// See OCTOGEN.md. Derived from semtex (retired, its file removed; cordite lineage below).
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
#ifdef FOOLISH_ORACLE_MT
#include "legal.h"
#include "oracle_mt.h"     // Mode B: the shared accumulator this file feeds (MT5)
#endif
#ifdef OG_EXPLAIN_BUILD
#include "oracle_answer.h" // Mode A and the research builds: the answer this file fills
#endif

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

// Copy only the live prefix of a Game (header + first num_logs entries).
// logs[] is the final member, so everything else is inside the prefix.
static void og_lite_clone(Game *dst, const Game *src) {
    size_t base = offsetof(Game, logs);
    memcpy(dst, src, base + (size_t)src->num_logs * sizeof(GameLog));
}

// ---------- belief state ------------------------------------------------

#define OG_MAX_VOIDS 6
#ifdef OG_EXPLAIN_BUILD
_Static_assert(OG_MAX_VOIDS <= OG_ANSWER_MAX_VOIDS, "the answer cannot hold a seat's voids");
#endif

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
    mv->n = legal_moves_into(g, actor, mv->moves, SOLVE_SCRATCH_MOVES);
    if (mv->n == 0) return 0;
    if (mv->n > OG_SOLVE_MAX_MOVES) { S->aborted = true; return 0; }

    bool maximizing = (actor == S->me);
    int best = maximizing ? -2000 : 2000;
    for (int i = 0; i < mv->n; i++) {
        Game *child = solve_scratch_child(depth);
        solve_clone_prefix(child, g);
        if (!legal_move_apply(child, actor, &mv->moves[i])) continue;
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
            if (legal_move_apply(g, pi, &moves->moves[idx])) { acted = true; break; }
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

// ---------- OG_EXPLAIN: the deliberation's answer (analysis builds only) ----
//
// COMPILED OUT of every normal build: everything that reads a deliberation back
// out (this block, the solver-verdict probe, og_ex_emit, and og_publish, the
// one hook octogen_choose_impl's exits call) lives behind OG_EXPLAIN_BUILD, so the shipped native +
// wasm bots carry ZERO of it - no code-size cost. It is what the oracle builds
// are made of (c/Makefile, `wasm-oracle`).
//
// One deliberation fills one OgAnswer (oracle_answer.h): the seat's hand, the
// table, octogen's belief, the candidate moves with their MC average-finish
// scores (score[i]/nsim[i]) OR the exact endgame-solver verdict per move, and
// the chosen move. A host reads it through og_answer(); nothing is formatted
// and nothing is parsed.
//
// A wasm build answers every choose. A NATIVE build answers only when asked:
// set OG_EXPLAIN=1 (stderr) or OG_EXPLAIN=/path/to/file and each answer is also
// printed there as one line of JSON, for a person to read (og_answer_print).
// That text is debugging output and exists in no wasm module. Nothing runs
// when the var is unset.
#ifdef OG_EXPLAIN_BUILD
enum { OG_EX_NONE_V = 2000001, OG_EX_UNKNOWN_V = 2000002, OG_EX_ILLEGAL_V = 2000003 };
static int   og_ex_solve_applied = 0;   // did the endgame solver fire this decision
static int   og_ex_verdict[OG_ANSWER_MAX_VERDICTS];   // exact per-root-move solver value / sentinel

// Not thread-local: Mode A is one instance per worker with TLS stripped, and a
// native research run deliberates on one thread. Mode B never compiles this.
static OgAnswer og_ans;
static bool     og_ans_ready = false;
const OgAnswer *og_answer(void)       { return og_ans_ready ? &og_ans : NULL; }
void            og_answer_reset(void) { og_ans_ready = false; }

#ifdef CD_WASM_OVERLAY
// wasm has no getenv to ask and no file to print to: this (analysis-only) build
// always answers, and the host reads the struct.
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
// og_ex_emit (the per-decision answer) is defined after the Candidates
// typedef, just above og_publish.
static void og_ex_emit(const Game *g, int bot_idx, const LegalMoves *moves,
                       const Belief *B, const void *C, const double *score, const int *nsim,
                       const bool *alive, const bool *forced_loss,
                       int chosen_idx, int solver_applied, int solver_win_idx);
#endif  // OG_EXPLAIN_BUILD

#if defined(FOOLISH_ORACLE_MT) && !defined(OG_EXPLAIN_BUILD)
// Mode B (docs/INFINITE_ORACLE_DESIGN.md §8b.5, MT6) reuses the exact-verdict
// probe below but not the answer struct, so declare just the verdict state the probe
// fills. Thread-local: every thread runs its own probe over the same position
// off its own transposition table, so there is no cross-thread race on these
// and no cross-thread reuse of a seat-perspective solver value.
enum { OG_EX_NONE_V = 2000001, OG_EX_UNKNOWN_V = 2000002, OG_EX_ILLEGAL_V = 2000003 };
static _Thread_local int og_ex_solve_applied = 0;
static _Thread_local int og_ex_verdict[600];
#endif

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
            long eb = ebs ? (long)atoi(ebs) : 4000000L;
            int v = cd_sim_solve_d(&child, bot_idx, -2000, 2000, &eb, 1, &ab);
            og_ex_verdict[i] = (ab || eb <= 0) ? OG_EX_UNKNOWN_V : v;
        }
        cd_sim_solve_reset();                       // cold TT for the real solve
        random_strategy_set_seed(og_ex_rng_save);   // unperturbed strategy RNG
    }
#elif defined(FOOLISH_ORACLE_MT)
    // Mode B: the SAME verdict probe (MT6), gated on the shared defuse flag
    // instead of og_explain_on(). Runs per-thread over the shared position, off
    // this thread's own transposition table, into thread-local state. Once the
    // controller decides the position is unprovable at budget it sets
    // defuse_probe and this expensive per-choose solve is skipped, so later
    // batches are pure-MC-priced.
    if (bbsolve && !__atomic_load_n(&g_ogmt.defuse_probe, __ATOMIC_RELAXED)) {
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
            long eb = ebs ? (long)atoi(ebs) : 4000000L;
            int v = cd_sim_solve_d(&child, bot_idx, -2000, 2000, &eb, 1, &ab);
            og_ex_verdict[i] = (ab || eb <= 0) ? OG_EX_UNKNOWN_V : v;
        }
        cd_sim_solve_reset();
        random_strategy_set_seed(og_ex_rng_save);
    }
#endif  // OG_EXPLAIN_BUILD / FOOLISH_ORACLE_MT

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
            if (!legal_move_apply(child, bot_idx, &moves->moves[i])) continue;
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
            if (!legal_move_apply(child, bot_idx, &moves->moves[i])) continue;
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

// HOW MANY MOVES ONE DECISION HOLDS, and it is not the same number for the two
// things that ask. The Infinite Oracle is not on a clock - it runs to
// convergence or ORACLE_HARD_CAP_MS - so a wider table costs it only a longer
// "coming into focus", which is the feature. A bot picking a move in a live
// game is on a clock, and a wider table would only split the same world budget
// thinner and play weaker for it.
//
// So the oracle builds (-DFOOLISH_ORACLE_BUILD, c/Makefile) get 128 and the
// shipped bot keeps the 26 it has always searched. A build flag rather than a
// runtime knob because the number sizes static tables: sizing them at 64
// everywhere and capping at runtime cost bots.wasm.gz ten bytes MORE than its
// 81,920 budget allows, for a width that build can never use.
//
// 128 IS WHERE THE MISSES STOP, and that is measured rather than chosen. Walking
// every decision of 38 bot games plus two shared human replays - 3,687 decisions
// - the 64-wide table said "not considered" under a move that had just been
// played 3 times; 128 says it 0 times. The whole cost is on the handful of
// boards that are genuinely that wide: mean candidates per decision go 7.9 to
// 9.1, and only a board that already had more than 64 legal replies pays for
// the rest.
#ifndef OG_MAX_CANDS
#ifdef FOOLISH_ORACLE_BUILD
#define OG_MAX_CANDS 128
#else
#define OG_MAX_CANDS 26
#endif
#endif
// The per-width table: this many distinct widths, this many places each,
// bounded overall by OG_MAX_CANDS. Measured over a shared 2p replay and 3p/4p/8p
// bot games, legal covers per defender decision run at a median of 3-6; the tail
// is long (p90 10-48, one 4p board at 277), so this is a budget, not a promise.
//
// It was ten places shared across every width, which is how the panel came to
// print "not considered" under a triple cover that had just been animated: four
// triples existed, three were kept, and the one dropped - the dearest, two 9s
// and an Ace - was the one played.
//
// The shipped bot ranks only its covers here, ten places per width. An ORACLE
// build ranks its attacks here too and gives every width as many places as the
// whole table has: the per-width list stops being a second, smaller cap and
// goes back to being only an ordering, which is all it was ever for. The
// round-robin emit below is what actually bounds the table, and it is fair
// across widths, so nothing is dropped for its width while places go unused.
//
// Sixteen places per width was not fair in that sense, because whether sixteen
// was enough depended on what ELSE was on the board. A 22-card hand facing two
// attacks ranked 11 singles, 16 doubles, 16 triples and 2 quads - 46 of a
// 64-wide table, with 18 places going spare - and still dropped the double the
// defender had played, because its own bucket was the full one. See
// og_pick_candidates for why the two builds differ and what it cost to measure.
#define OG_W_SIZES 4
#ifdef FOOLISH_ORACLE_BUILD
#define OG_W_PER_SIZE OG_MAX_CANDS
#else
#define OG_W_PER_SIZE 10
#define OG_ATK_KEEP   12
#endif
// Pass candidates. Ranked width-first like attacks, so the widest pass is always
// among them; the cap is what bounds the reserve the covers leave room for.
//
// An ORACLE build keeps every pass there can be. A pass spends cards of the
// attack's value and a hand holds at most four of one value, so the legal
// passes are the non-empty subsets of those: at most 2^4 - 1 = 15, and fifteen
// places can never drop one. Six could, and did - a 6p game recorded PASS 3C
// with seven passes legal, and the panel said "not considered" under a move
// the defender had just made, because width-first ranking puts every single
// behind every wider pass.
#ifdef FOOLISH_ORACLE_BUILD
#define OG_PAS_KEEP    15
#else
#define OG_PAS_KEEP     6
#endif

typedef struct {
    int idx[OG_MAX_CANDS];
    int n;
} Candidates;
#ifdef OG_EXPLAIN_BUILD
_Static_assert(OG_MAX_CANDS <= OG_ANSWER_MAX_CANDS, "the answer cannot hold every candidate");
#endif

#ifdef FOOLISH_ORACLE_MT
// Mode B helpers (docs/INFINITE_ORACLE_DESIGN.md §8b.5, MT5). One thread
// CAS-publishes the candidate descriptor table for this generation; every later
// batch, on any thread, is CHECKED against it before its per-candidate scores
// are folded in.
//
// The check is not ceremony. Candidate ENUMERATION is deterministic
// (og_pick_candidates, no RNG), but its input is not: forced_loss comes from the
// budget-bounded root endgame solve, which runs off a per-thread transposition
// table, so two threads can legitimately reach this point with different
// candidate SETS. Summing those under a shared index would average two different
// moves into one bar. A disagreeing batch is dropped and counted instead.
_Static_assert(OG_MAX_CANDS <= OG_MT_MAX_CANDS, "candidate table too small for Mode B");

static void og_mt_pack_desc(const LegalMoves *moves, const Candidates *C,
                            OgMtCand *out, int *n_out) {
    int n = C->n; if (n > OG_MT_MAX_CANDS) n = OG_MT_MAX_CANDS;
    memset(out, 0, sizeof(OgMtCand) * (size_t)OG_MT_MAX_CANDS);
    for (int i = 0; i < n; i++) {
        const LegalMove *m = &moves->moves[C->idx[i]];
        OgMtCand *d = &out[i];
        d->type = (uint8_t)m->type;
        int nc = m->n_cards; if (nc > OG_MT_MAX_CARDS) nc = OG_MT_MAX_CARDS;
        d->n_cards = (uint8_t)nc;
        for (int k = 0; k < nc; k++) d->cards[k] = m->cards[k];
        if (m->type == MOVE_COVER) {
            d->n_targets = (uint8_t)nc;
            for (int k = 0; k < nc; k++) d->targets[k] = m->attack_cards[k];
        }
    }
    *n_out = n;
}

// Publish-or-agree. Returns 1 when this batch may contribute, 0 when it must be
// dropped (a disagreeing candidate set, or a generation that moved underneath).
static int og_mt_commit_desc(const LegalMoves *moves, const Candidates *C) {
    OgMtCand local[OG_MT_MAX_CANDS];
    int n = 0;
    og_mt_pack_desc(moves, C, local, &n);

    uint32_t st = 0;
    if (__atomic_compare_exchange_n(&g_ogmt.cand_state, &st, 1u, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        g_ogmt.n_candidates = n;
        memcpy(g_ogmt.cand, local, sizeof(local));
        __atomic_store_n(&g_ogmt.cand_state, 2u, __ATOMIC_RELEASE);
        return 1;
    }
    // Someone else is publishing or has published. A bounded spin covers the
    // memcpy window; anything longer means the generation moved, drop the batch.
    for (int spin = 0; spin < 1000000 &&
         __atomic_load_n(&g_ogmt.cand_state, __ATOMIC_ACQUIRE) == 1u; spin++) { }
    if (__atomic_load_n(&g_ogmt.cand_state, __ATOMIC_ACQUIRE) != 2u) return 0;
    if (g_ogmt.n_candidates != n) {
        __atomic_add_fetch(&g_ogmt.desc_mismatch, 1u, __ATOMIC_RELAXED);
        return 0;
    }
    const unsigned char *a = (const unsigned char *)g_ogmt.cand;
    const unsigned char *b = (const unsigned char *)local;
    for (size_t i = 0; i < sizeof(local); i++) {
        if (a[i] != b[i]) {
            __atomic_add_fetch(&g_ogmt.desc_mismatch, 1u, __ATOMIC_RELAXED);
            return 0;
        }
    }
    return 1;
}

// Copy this thread's exact endgame verdicts into the shared table. Verdicts are
// per-thread because the probe's budget is; a thread that PROVED something
// overwrites a thread that gave up, never the reverse, so a published win/loss
// is always a real proof.
static void og_mt_capture_verdicts(const Candidates *C) {
    if (!og_ex_solve_applied) return;
    __atomic_store_n(&g_ogmt.solver_applied, 1u, __ATOMIC_RELAXED);
    for (int i = 0; i < C->n && i < OG_MT_MAX_CANDS; i++) {
        int v = og_ex_verdict[C->idx[i]];
        if (v == OG_EX_NONE_V || v == OG_EX_UNKNOWN_V) {
            int cur = __atomic_load_n(&g_ogmt.verdict[i], __ATOMIC_RELAXED);
            if (cur != 0 && cur != OG_EX_NONE_V && cur != OG_EX_UNKNOWN_V) continue;
        }
        __atomic_store_n(&g_ogmt.verdict[i], v, __ATOMIC_RELAXED);
    }
}
#endif  // FOOLISH_ORACLE_MT

#ifdef CD_ORC_TRACE
// ---------- oracle path aggregation (the "why this EF" data) ---------------
// Per-candidate aggregation of the cd_orc playout traces across every
// (world x candidate) rollout of ONE deliberation: path-prefix clusters
// (base-5 id over the first CD_ORC_ROUNDS round symbols -> count + finish
// sum) plus whole-playout marginals. Read back per candidate by og_ex_emit
// into the answer's why table below, the raw material for the overlay's
// template proof. ~163 KiB of file-scope BSS - the Mode A oracle build only, where
// memory is free and TLS is stripped (see CD_ORC_TRACE in cordite_sim.h).
#define OG_ORC_PATHS 625   // 5^CD_ORC_ROUNDS
#define OG_ORC_RTYPES 6    // MV_ATTACK..MV_NONE
#define OG_ORC_RCARDS 53   // card id 0..51, 52 = card-less (pickup/good)
static _Thread_local uint32_t og_orc_cnt[OG_MAX_CANDS][OG_ORC_PATHS];
static _Thread_local float    og_orc_fin[OG_MAX_CANDS][OG_ORC_PATHS];
static _Thread_local uint32_t og_orc_reply[OG_MAX_CANDS][OG_ORC_RTYPES][OG_ORC_RCARDS];
static _Thread_local uint32_t og_orc_n[OG_MAX_CANDS];
static _Thread_local uint32_t og_orc_mepk[OG_MAX_CANDS], og_orc_oppk[OG_MAX_CANDS];
static _Thread_local uint32_t og_orc_metr[OG_MAX_CANDS], og_orc_opptr[OG_MAX_CANDS];
static _Thread_local uint32_t og_orc_rounds[OG_MAX_CANDS];

static void og_orc_reset(void) {
    memset(og_orc_cnt, 0, sizeof og_orc_cnt);
    memset(og_orc_fin, 0, sizeof og_orc_fin);
    memset(og_orc_reply, 0, sizeof og_orc_reply);
    memset(og_orc_n, 0, sizeof og_orc_n);
    memset(og_orc_mepk, 0, sizeof og_orc_mepk);
    memset(og_orc_oppk, 0, sizeof og_orc_oppk);
    memset(og_orc_metr, 0, sizeof og_orc_metr);
    memset(og_orc_opptr, 0, sizeof og_orc_opptr);
    memset(og_orc_rounds, 0, sizeof og_orc_rounds);
}

static void og_orc_fold(int ci, int fp) {
    if (ci < 0 || ci >= OG_MAX_CANDS) return;
    int id = 0, m = 1;
    for (int i = 0; i < cd_orc.nsym; i++) { id += m * cd_orc.sym[i]; m *= 5; }
    og_orc_cnt[ci][id]++;
    og_orc_fin[ci][id] += (float)fp;
    if (cd_orc.have_reply) {
        int t = cd_orc.reply_type < OG_ORC_RTYPES ? cd_orc.reply_type : OG_ORC_RTYPES - 1;
        int c = cd_orc.reply_card < 52 ? cd_orc.reply_card : 52;
        og_orc_reply[ci][t][c]++;
    }
    og_orc_n[ci]++;
    og_orc_mepk[ci] += (uint32_t)cd_orc.me_pickups;
    og_orc_oppk[ci] += (uint32_t)cd_orc.opp_pickups;
    og_orc_metr[ci] += (uint32_t)cd_orc.me_trumps;
    og_orc_opptr[ci] += (uint32_t)cd_orc.opp_trumps;
    og_orc_rounds[ci] += (uint32_t)cd_orc.rounds;
}

// ---- the answer's why table (oracle_answer.h OgAnswerWhy) ------------------
// One entry per candidate with samples, in candidate order: its most told
// storylines, the replies it most often met, and the whole-playout means. The
// raw material for the overlay's template proof.
_Static_assert(CD_ORC_ROUNDS == OG_ANSWER_PATH_ROUNDS, "a storyline is CD_ORC_ROUNDS symbols");

static void og_orc_fill_why(OgAnswer *a, int ncand, const int *nsim) {
    int nc = 0;
    for (int k = 0; k < ncand && k < OG_MAX_CANDS; k++) {
        if (!nsim || nsim[k] <= 0 || og_orc_n[k] == 0) continue;
        OgAnswerWhy *e = &a->why[nc++];
        e->row = (uint8_t)k;
        e->n = og_orc_n[k];
        double on = (double)og_orc_n[k];
        e->mepk  = (float)(og_orc_mepk[k] / on);
        e->oppk  = (float)(og_orc_oppk[k] / on);
        e->metr  = (float)(og_orc_metr[k] / on);
        e->opptr = (float)(og_orc_opptr[k] / on);
        e->rnds  = (float)(og_orc_rounds[k] / on);
        // top replies by count (insertion top-3 over the 6x53 table)
        {
            int bt[OG_ANSWER_TOP_REPLIES], bc[OG_ANSWER_TOP_REPLIES];
            uint32_t bn[OG_ANSWER_TOP_REPLIES];
            int nr = 0;
            for (int t = 0; t < OG_ORC_RTYPES; t++)
                for (int c = 0; c < OG_ORC_RCARDS; c++) {
                    uint32_t v = og_orc_reply[k][t][c];
                    if (!v) continue;
                    int ins = nr;
                    while (ins > 0 && bn[ins - 1] < v) ins--;
                    if (ins >= OG_ANSWER_TOP_REPLIES) continue;
                    if (nr < OG_ANSWER_TOP_REPLIES) nr++;
                    for (int j = nr - 1; j > ins; j--) {
                        bt[j] = bt[j-1]; bc[j] = bc[j-1]; bn[j] = bn[j-1];
                    }
                    bt[ins] = t; bc[ins] = c; bn[ins] = v;
                }
            e->n_replies = (uint8_t)nr;
            for (int r = 0; r < nr; r++) {
                e->replies[r].type = (uint8_t)bt[r];
                e->replies[r].card = (uint8_t)bc[r];
                e->replies[r].n = bn[r];
            }
        }
        // top path clusters by count (insertion top-12 over 625 slots)
        {
            int top[OG_ANSWER_TOP_PATHS];
            int nt = 0;
            for (int pid = 0; pid < OG_ORC_PATHS; pid++) {
                uint32_t v = og_orc_cnt[k][pid];
                if (!v) continue;
                int ins = nt;
                while (ins > 0 && og_orc_cnt[k][top[ins - 1]] < v) ins--;
                if (ins >= OG_ANSWER_TOP_PATHS) continue;
                if (nt < OG_ANSWER_TOP_PATHS) nt++;
                for (int j = nt - 1; j > ins; j--) top[j] = top[j-1];
                top[ins] = pid;
            }
            e->n_paths = (uint8_t)nt;
            for (int ti = 0; ti < nt; ti++) {
                OgAnswerPath *pe = &e->paths[ti];
                int x = top[ti], len = 0;
                while (x > 0 && len < CD_ORC_ROUNDS) {
                    pe->sym[len] = (uint8_t)(x % 5);
                    x /= 5; len++;
                }
                pe->len = (uint8_t)len;
                pe->n = og_orc_cnt[k][top[ti]];
                pe->fin = (float)(og_orc_fin[k][top[ti]] / (double)og_orc_cnt[k][top[ti]]);
            }
        }
    }
    a->n_why = (uint8_t)nc;
}
#endif  // CD_ORC_TRACE

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

// TWO COVERS THAT SPEND THE SAME CARDS ON THE SAME ATTACKS ARE ONE MOVE, and
// legal move generation emits them separately. Give a defender two trumps and
// two uncovered attacks and it enumerates JD*->9C with QD*->9H and JD*->9H with
// QD*->9C: different LegalMoves, identical commitment, and both end the bout
// with the same four cards in the discard. The panel already knows they are one
// - the accumulator keys on the sorted cards and the sorted targets, so it
// folds them - but by then the damage is done, because each of them took a
// candidate place. One board ranked 26 candidates that were 16 distinct moves.
//
// EVERY BUILD DROPS THEM, the shipped bot included. The worlds below are
// SHARED, so a repeat does not halve anyone's sample - what it costs is a
// rollout per world, a place in the 26-wide table, which is the only place a
// move can be searched from at all, and a seat in the survivor cuts (stage 0
// keeps C.n/3, stage 1 keeps two, so a pair of twins can reach the final duel
// and spend W3 worlds deciding which of two identical moves is better).
// Surveyed over 3,231 shipped-bot decisions, 240 games at 2/4/6/8 players:
// 5.0% of all candidate places were repeats and 13.7% of decisions carried
// one; 6.0% still carried one into stage 2 and 0.4% ran the final duel as a
// move against its own twin. The worst board spent all 26 places on 16 moves.
// After, 0 repeats.
static bool og_same_cards(const Card *x, const Card *y, int n) {
    bool used[MAX_MOVE_CARDS] = { false };
    if (n > MAX_MOVE_CARDS) return false;
    for (int i = 0; i < n; i++) {
        int j = 0;
        while (j < n && (used[j] || x[i].suit != y[j].suit || x[i].value != y[j].value)) j++;
        if (j == n) return false;
        used[j] = true;
    }
    return true;
}

/** Same move by the canonical key the panel uses: same type, same cards as a
 *  multiset, and for a cover the same targets as a multiset. */
static bool og_same_key(const LegalMove *a, const LegalMove *b) {
    if (a->type != b->type || a->n_cards != b->n_cards) return false;
    if (!og_same_cards(a->cards, b->cards, a->n_cards)) return false;
    return a->type != MOVE_COVER
        || og_same_cards(a->attack_cards, b->attack_cards, a->n_cards);
}

static void og_pick_candidates(const Game *g, const LegalMoves *moves,
                               const bool *excluded, Candidates *out) {
    int power = g->power_suit;

    // MOVES ARE RANKED WITHIN THEIR WIDTH, NOT AGAINST IT. A cover's key is a
    // PRODUCT of card scores, so every extra card multiplies it: with scores in
    // the 6-14 range a single cover keys around 10, a double around 100 and a
    // triple in the hundreds or thousands. Ranked into one list that keeps the
    // smallest, a wide cover could never place - a defender facing three
    // attacks was offered singles and doubles and never the move that takes all
    // three, even when that is the move it went on to play. The Oracle showed
    // it plainly: the recorded triple cover was not on the panel at all.
    //
    // Bucketing by width keeps the intent (spend the cheapest cards) without
    // letting width decide the comparison, which a product cannot help doing.
    // A sum or a mean would only move that arbitrariness around: covering
    // three attacks and covering one are different commitments, not cheaper
    // and dearer versions of one, so they are ranked apart and each width gets
    // its own places.
    // One ranked list PER WIDTH, flat: width b keeps OG_W_PER_SIZE at
    // wid[b * OG_W_PER_SIZE ...]. og_ranked_insert works on any slice.
    int wid[OG_W_SIZES * OG_W_PER_SIZE];
    double wid_k[OG_W_SIZES * OG_W_PER_SIZE];
    int n_wid[OG_W_SIZES];
    for (int i = 0; i < OG_W_SIZES; i++) n_wid[i] = 0;
#ifndef FOOLISH_ORACLE_BUILD
    // ATTACKS KEEP THEIR FLAT, WIDEST-FIRST LIST IN A SHIPPED BUILD, and that
    // is a measurement rather than an oversight. Ranking them per width like
    // the covers is unarguably better COVERAGE - it is what lets the Oracle
    // score a single-card attack from a hand holding four 3s, four 9s, four
    // 10s and four Ks, a move the flat list cannot even offer - but the width
    // term it removes is also octogen's prior that dumping cards is worth
    // something, and the MC is not strong enough to rediscover it. Against
    // handwritten over 500 games a side at one seed range, mean finish went
    // pc3 1.438 -> 1.462, pc4 1.806 -> 1.964, pc5 2.442 -> 2.598, pc6
    // 2.808 -> 2.904, with pc4's win rate 46.2% -> 38.2%; holding the emitted
    // count down to the old twelve did not recover it (pc4 2.002), so what is
    // worth the 0.15 of a finish is the prior and not the width of the search.
    //
    // So the two builds rank attacks differently, on purpose. The oracle is not
    // playing: it has to be able to score the move a human or another bot
    // actually made, and a candidate list is the only place that move can be
    // scored from. The bot is playing, and it plays this better.
    int atk[OG_ATK_KEEP];  double atk_k[OG_ATK_KEEP];  int n_atk = 0;
#endif
    int pas[OG_PAS_KEEP];  double pas_k[OG_PAS_KEEP];  int n_pas = 0;
    int good_idx = -1, pickup_idx = -1;

    for (int i = 0; i < moves->n; i++) {
        if (excluded[i]) continue;
        const LegalMove *m = &moves->moves[i];
        double key;
        switch (m->type) {
            case MOVE_ATTACK: {
                int sum = 0;
                for (int j = 0; j < m->n_cards; j++) sum += og_card_score(m->cards[j], power);
#ifdef FOOLISH_ORACLE_BUILD
                key = (double)sum;
                break;
#else
                og_ranked_insert(atk, atk_k, &n_atk, OG_ATK_KEEP, i,
                                 -(double)m->n_cards * 10000.0 + (double)sum);
                continue;
#endif
            }
            case MOVE_COVER: {
                double prod = 1.0;
                for (int j = 0; j < m->n_cards; j++) prod *= (double)og_card_score(m->cards[j], power);
                // The -(n * 0.5) this key used to carry is gone: inside a
                // bucket n_cards is fixed, so it was the same constant added to
                // every key in the same list and could not order anything.
                key = prod;
                break;
            }
            case MOVE_PASS: {
                // WIDTH FIRST, exactly as the attack above it. A pass was keyed
                // on the SUM of its cards into three slots, and a sum grows with
                // every card, so a pass of three twos always sorted behind every
                // pass of one and never placed: a real 8p game recorded PASS
                // 2D 2S 2H and the panel said "not considered", which means
                // octogen could not have chosen it either. Covers had the same
                // defect through a product; this is the same defect through a
                // sum, in the branch that fix did not touch.
                int sum = 0;
                for (int j = 0; j < m->n_cards; j++) sum += og_card_score(m->cards[j], power);
                og_ranked_insert(pas, pas_k, &n_pas, OG_PAS_KEEP, i,
                                 -(double)m->n_cards * 10000.0 + (double)sum);
                continue;
            }
            case MOVE_GOOD:   good_idx = i;   continue;
            case MOVE_PICKUP: pickup_idx = i; continue;
            default: continue;
        }
        int b = m->n_cards - 1;             // width 1 -> list 0
        if (b < 0) b = 0;                   // never index behind wid[]
        if (b >= OG_W_SIZES) b = OG_W_SIZES - 1;
        const int o = b * OG_W_PER_SIZE;
        bool dup = false;                   // see og_same_key above
        for (int k = 0; k < n_wid[b] && !dup; k++)
            dup = og_same_key(m, &moves->moves[wid[o + k]]);
        if (dup) continue;
        og_ranked_insert(wid + o, wid_k + o, &n_wid[b], OG_W_PER_SIZE, i, key);
    }

    out->n = 0;
#ifndef FOOLISH_ORACLE_BUILD
    for (int i = 0; i < n_atk && out->n < OG_MAX_CANDS; i++) out->idx[out->n++] = atk[i];
#endif
    // ROUND-ROBIN, not width by width. Taking width 1 to its cap before looking
    // at width 2 throttles the commonest board there is - one uncovered attack,
    // where every cover is a single and the cap is the only thing that applies -
    // while a board with three attacks still has to wait its turn. Interleaving
    // gives each width its best first, and a board with only one width in play
    // simply keeps drawing from it until the table is full.
    // AND THE TABLE IS NOT THE WHOLE LIST. pass / good / pickup are emitted
    // after it and there are never more than a handful of them, but a defender
    // facing a wide board ranks up to OG_W_SIZES * OG_W_PER_SIZE covers,
    // which is more than OG_MAX_CANDS on its own. Unreserved, the
    // covers filled it: a recorded PASS at a 4p board came back "not
    // considered" behind 24 covers, and the same board could not have offered
    // PICKUP at all - octogen cannot choose a move that is not a candidate, so
    // that is a play defect and not only a panel one. The tail is tiny and
    // always matters; it is reserved before the covers spend anything.
    // n_pas is at most OG_PAS_KEEP and the two flags at most 1 each, so tail is
    // at most 8 against the shipped bot's OG_MAX_CANDS of 26 and at most 17
    // against an oracle build's 128: the reserve cannot go negative in either.
    const int tail = n_pas + (good_idx >= 0) + (pickup_idx >= 0);
    for (int r = 0; r < OG_W_PER_SIZE; r++)
        for (int b = 0; b < OG_W_SIZES && out->n + tail < OG_MAX_CANDS; b++)
            if (r < n_wid[b]) out->idx[out->n++] = wid[b * OG_W_PER_SIZE + r];
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
// A bounded text cursor: what a row's label is written through. It saturates at
// `cap` and never writes past it, so a label too long for its field is cut
// short, never an overrun.
typedef struct { char *b; int cap, n; } OgText;
static void og_text_put(OgText *t, const char *s) {
    for (; *s; s++) if (t->n < t->cap) t->b[t->n++] = *s;
}
static const char *OG_EX_VAL[14] = {
    "?","2","3","4","5","6","7","8","9","10","J","Q","K","A"
};
// Canonical durak rank convention (wire v13 = A), trump marked with '*'.
static void og_text_card(OgText *t, Card c, int trump) {
    char s[2] = { (c.suit >= 0 && c.suit < 4) ? "SHCD"[(int)c.suit] : '?', 0 };
    og_text_put(t, (c.value >= 1 && c.value <= 13) ? OG_EX_VAL[c.value] : "?");
    og_text_put(t, s);
    if (c.suit == trump) og_text_put(t, "*");
}
static const char *OG_EX_MTYPE[6] = { "attack","cover","pass","pickup","good","wait" };
// The move in the kernel's notation, the text a reader is shown for it:
// "attack 7H 7S*", "cover 10H->7H,QS*->9D", "pickup", "good".
static void og_ans_label(OgAnswerRow *r, const LegalMove *m, int trump) {
    OgText t = { r->label, OG_ANSWER_LABEL - 1, 0 };
    if (m->type == MOVE_COVER && m->n_cards >= 1) {
        og_text_put(&t, "cover");
        for (int k = 0; k < m->n_cards; k++) {
            og_text_put(&t, k ? "," : " ");
            og_text_card(&t, m->cards[k], trump);
            og_text_put(&t, "->");
            og_text_card(&t, m->attack_cards[k], trump);
        }
    } else if (m->type == MOVE_PICKUP) {
        og_text_put(&t, "pickup");
    } else if (m->type == MOVE_GOOD) {
        og_text_put(&t, "good");
    } else {
        og_text_put(&t, (m->type >= 0 && m->type < 6) ? OG_EX_MTYPE[m->type] : "?");
        og_text_put(&t, " ");
        for (int c = 0; c < m->n_cards; c++) {
            if (c) og_text_put(&t, " ");
            og_text_card(&t, m->cards[c], trump);
        }
    }
    r->label_len = (uint8_t)t.n;
    r->label[t.n] = 0;              // for the native printer; a host reads label_len
}
// A row as its move describes it, with nothing sampled and nothing proven yet.
static void og_ans_row(OgAnswerRow *r, const LegalMove *m, int trump) {
    int n = m->n_cards < MAX_MOVE_CARDS ? m->n_cards : MAX_MOVE_CARDS;
    r->type = (uint8_t)m->type;
    r->n_cards = (uint8_t)n;
    r->n_targets = (m->type == MOVE_COVER) ? (uint8_t)n : 0;
    for (int i = 0; i < n; i++) { r->cards[i] = m->cards[i]; r->targets[i] = m->attack_cards[i]; }
    r->scored = false; r->alive = true; r->pruned = false;
    r->forced_loss = false; r->chosen = false;
    r->verdict = OG_VERDICT_NONE; r->has_verdict_val = false; r->verdict_val = 0;
    r->nsim = 0; r->score = 0.0;
    og_ans_label(r, m, trump);
}
// The probe's value for a move, as a verdict: a sentinel says why there is no
// value, anything else IS one and its sign is the result.
static void og_ans_verdict(OgAnswerRow *r, int val) {
    if (val == OG_EX_UNKNOWN_V) r->verdict = OG_VERDICT_UNKNOWN;
    else if (val == OG_EX_ILLEGAL_V) r->verdict = OG_VERDICT_ILLEGAL;
    else if (val == OG_EX_NONE_V) r->verdict = OG_VERDICT_NONE;
    else {
        r->has_verdict_val = true; r->verdict_val = val;
        r->verdict = (val > 0) ? OG_VERDICT_WIN : (val < 0) ? OG_VERDICT_LOSS : OG_VERDICT_DRAW;
    }
}

#ifndef CD_WASM_OVERLAY
// The answer as one line of JSON, for a person running a native research build
// with OG_EXPLAIN set. Printed FROM the struct, so what a reader is shown is
// what a host would have read. Debugging output only: no wasm module has it.
static void og_answer_print_cards(FILE *f, const Card *cards, int n, int trump) {
    fputc('[', f);
    for (int i = 0; i < n; i++) {
        char cb[8]; OgText t = { cb, (int)sizeof cb - 1, 0 };
        og_text_card(&t, cards[i], trump); cb[t.n] = 0;
        fprintf(f, "%s\"%s\"", i ? "," : "", cb);
    }
    fputc(']', f);
}
static void og_answer_print(FILE *f, const OgAnswer *a) {
    static const char *VERDICT[] = { "none","unknown","illegal","win","loss","draw" };
    static const char *SOLVER[]  = { "none","win","eval" };
    int trump = a->trump;
    fprintf(f, "{\"seat\":%d,\"deck\":%d,\"defender\":%d,\"trump\":%d,\"nlogs\":%d",
            a->seat, a->deck, a->defender, trump, a->n_logs);
    fprintf(f, ",\"belief\":{\"pinned\":[");
    for (int p = 0; p < a->n_players; p++) {
        if (p) fputc(',', f);
        og_answer_print_cards(f, a->pinned[p].cards, a->pinned[p].n, trump);
    }
    fprintf(f, "],\"pool_count\":%d,\"voids\":[", a->n_pool);
    for (int p = 0; p < a->n_players; p++) {
        if (p) fputc(',', f);
        og_answer_print_cards(f, a->voids[p].cards, a->voids[p].n, trump);
    }
    fprintf(f, "],\"floor\":[");
    for (int p = 0; p < a->n_players; p++) fprintf(f, "%s%d", p ? "," : "", a->floor[p]);
    fprintf(f, "]},\"hand\":");
    og_answer_print_cards(f, a->hand, a->n_hand, trump);
    fprintf(f, ",\"hand_counts\":[");
    for (int p = 0; p < a->n_players; p++) fprintf(f, "%s%d", p ? "," : "", a->hand_count[p]);
    fprintf(f, "],\"table\":[");
    for (int b = 0; b < a->n_battles; b++) {
        fprintf(f, "%s{\"attack\":", b ? "," : "");
        og_answer_print_cards(f, &a->table[b].attack, 1, trump);
        fprintf(f, ",\"defense\":");
        og_answer_print_cards(f, &a->table[b].defense, a->table[b].covered ? 1 : 0, trump);
        fputc('}', f);
    }
    fprintf(f, "],\"solver\":{\"applied\":%d,\"result\":\"%s\"},\"candidates\":[",
            a->solver_applied ? 1 : 0, SOLVER[a->solver_result]);
    for (int k = 0; k < a->n_rows; k++) {
        const OgAnswerRow *r = &a->rows[k];
        fprintf(f, "%s{\"label\":\"%s\"", k ? "," : "", r->label);
        if (r->scored) fprintf(f, ",\"score\":%.4f", r->score);
        fprintf(f, ",\"nsim\":%d,\"alive\":%d", r->nsim, r->alive ? 1 : 0);
        if (r->pruned) fprintf(f, ",\"pruned\":1");
        fprintf(f, ",\"forced_loss\":%d,\"verdict\":\"%s\"", r->forced_loss ? 1 : 0, VERDICT[r->verdict]);
        if (r->has_verdict_val) fprintf(f, ",\"verdict_val\":%d", r->verdict_val);
        fprintf(f, ",\"chosen\":%d}", r->chosen ? 1 : 0);
    }
    fprintf(f, "]}\n");
    fflush(f);
}
#endif  // !CD_WASM_OVERLAY

// Fill the answer for one decision. score/nsim/alive/forced_loss/Cv may be
// NULL (endgame-win and single-move paths). score[k]/nsim[k]/alive[k] index by
// CANDIDATE position k (matching C->idx[k]); verdicts/forced_loss index by the
// move index into moves->moves[].
static void og_ex_emit(const Game *g, int bot_idx, const LegalMoves *moves,
                       const Belief *B, const void *Cv, const double *score, const int *nsim,
                       const bool *alive, const bool *forced_loss,
                       int chosen_idx, int solver_applied, int solver_win_idx) {
#ifndef CD_WASM_OVERLAY
    if (!og_ex_out) return;
#endif
    const Candidates *C = (const Candidates *)Cv;
    OgAnswer *a = &og_ans;
    int trump = g->power_suit;
    const int vcap = (int)(sizeof(og_ex_verdict) / sizeof(og_ex_verdict[0]));

    a->seat = (int8_t)bot_idx;
    a->defender = (int8_t)g->defender;
    a->trump = (int8_t)trump;
    a->n_players = (uint8_t)g->num_players;
    a->deck = g->deck_count;
    a->n_logs = g->num_logs;
    a->solver_applied = solver_applied != 0;
    a->solver_result = !solver_applied ? OG_SOLVER_NONE
                     : (chosen_idx >= 0 && solver_win_idx >= 0) ? OG_SOLVER_WIN : OG_SOLVER_EVAL;

    { const Player *pl = &g->players[bot_idx];
      a->n_hand = (uint8_t)pl->hand_count;
      for (int i = 0; i < pl->hand_count; i++) a->hand[i] = pl->hand[i]; }
    a->n_battles = (uint8_t)g->num_battles;
    for (int b = 0; b < g->num_battles; b++) {
        a->table[b].attack = g->table_battles[b].attack;
        a->table[b].defense = g->table_battles[b].defense;
        a->table[b].covered = !card_is_none(g->table_battles[b].defense);
    }

    // octogen's actual belief about the hidden cards (public deduction): the
    // cards it has PINNED to each opponent's hand (watched them pick up, not yet
    // replayed) vs the genuinely-unknown pool (deck + un-pinned opponent cards),
    // plus demonstrated suit voids and rank floors. This is what octogen knows,
    // straight from og_build_belief - not reconstructed downstream.
    // voids[p] = attack cards p demonstrably could NOT cover at a pickup, so p
    // holds nothing that beats them (a deduced constraint, not a card).
    for (int p = 0; p < g->num_players; p++) {
        a->hand_count[p] = (uint8_t)g->players[p].hand_count;
        a->pinned[p].n = (uint8_t)B->pinned_n[p];
        for (int i = 0; i < B->pinned_n[p]; i++) a->pinned[p].cards[i] = B->pinned[p][i];
        a->voids[p].n = (uint8_t)B->void_n[p];
        for (int i = 0; i < B->void_n[p]; i++) a->voids[p].cards[i] = B->voids[p][i];
        a->floor[p] = (int8_t)B->floor_v[p];
    }
    a->n_pool = (uint8_t)B->n;

    int ncand = C ? C->n : moves->n;
    if (ncand > OG_ANSWER_MAX_CANDS) ncand = OG_ANSWER_MAX_CANDS;
    for (int k = 0; k < ncand; k++) {
        int mi = C ? C->idx[k] : k;
        OgAnswerRow *r = &a->rows[k];
        og_ans_row(r, &moves->moves[mi], trump);
        r->nsim = nsim ? nsim[k] : 0;
        if (score && nsim && nsim[k] > 0) { r->scored = true; r->score = score[k] / (double)nsim[k]; }
        r->alive = alive ? alive[k] : true;
        r->forced_loss = forced_loss ? forced_loss[mi] : false;
        if (solver_applied && mi < vcap) og_ans_verdict(r, og_ex_verdict[mi]);
        r->chosen = (mi == chosen_idx);
    }
    int nrows = ncand;
#ifdef FOOLISH_ORACLE_BUILD
    // §6.3: in the exact regime, the move the player actually made may be
    // excluded from candidacy as a proven loss - the very move a reviewer most
    // wants judged. The probe already filled og_ex_verdict[mi] by MOVE index;
    // add a scoreless, pruned row for every legal move with a PROVEN verdict
    // (win/loss/draw) that is not already a candidate. C==NULL means the
    // candidate set IS every move, so nothing is pruned; the MC regime
    // (!solver_applied) adds none - mid-game menus can be thousands of moves.
    if (solver_applied && C) {
        for (int mi = 0; mi < moves->n && mi < vcap && nrows < OG_ANSWER_MAX_ROWS; mi++) {
            int is_cand = 0;
            for (int k = 0; k < C->n; k++) if (C->idx[k] == mi) { is_cand = 1; break; }
            if (is_cand) continue;
            int val = og_ex_verdict[mi];
            if (val == OG_EX_NONE_V || val == OG_EX_UNKNOWN_V || val == OG_EX_ILLEGAL_V) continue;
            OgAnswerRow *r = &a->rows[nrows++];
            og_ans_row(r, &moves->moves[mi], trump);
            r->alive = false;
            r->pruned = true;
            r->forced_loss = forced_loss ? forced_loss[mi] : false;
            og_ans_verdict(r, val);
            r->chosen = (mi == chosen_idx);
        }
    }
#endif
    a->n_rows = (uint16_t)nrows;

    // The why table is refreshed in lockstep with the rows: stale storylines
    // must never survive an answer that carries no MC candidates.
    a->n_why = 0;
#ifdef CD_ORC_TRACE
    if (C && score && nsim) og_orc_fill_why(a, ncand, nsim);
#endif
    og_ans_ready = true;
#ifndef CD_WASM_OVERLAY
    og_answer_print(og_ex_out, a);
#endif
}
#endif  // OG_EXPLAIN_BUILD

// ---------- the chooser's parts ----------------------------------------------
// octogen_choose_impl below is the algorithm and nothing else. What used to be
// written out inside it lives here, each part once: the knob latch, what a
// build reads back out of a deliberation (every #ifdef the chooser had), and
// the stages of the search. All of it is static, so in a shipped build it
// inlines back into the chooser and the publishing half is no code at all.

// Latch the OG_* knobs (bot_knobs.h: env over the roster spec over the C
// default). Once per thread, and again after og_reload_flags.
static void og_load_flags(void) {
    og_no_solve  = og_flag("OG_NO_SOLVE");
    og_no_voids  = og_flag("OG_NO_VOIDS");
    og_no_flip   = og_flag("OG_NO_FLIP");
    og_no_floors = og_flag("OG_NO_FLOORS");
    // Exact leaf endgames in rollouts are OFF by default: measured both
    // slower and weaker vs the real (imperfect) opponent pool - modeling
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

// ---------- publishing a decision (analysis builds only) --------------------
//
// THE ONE PLACE a deliberation is read back out. Each of octogen_choose_impl's
// five exits publishes what it decided, and what that means is the build's:
//
//   OG_EXPLAIN_BUILD   fill the answer (og_ex_emit), when it is asked for
//   FOOLISH_ORACLE_MT  agree the candidate table with the other threads
//                      (og_mt_commit_desc) and, if this batch may contribute,
//                      fold it into the shared accumulator
//   neither (shipped)  nothing: every function in this section is empty
//
// The five exits are NOT five copies of one call. What each hands og_publish:
//
//   exit            candidates    scores  chosen    best          win
//   one legal move  move 0 alone  -       0         0             -       (og_publish_only_move)
//   proven win      a fresh pick  -       solved    solved's row  solved  (og_publish_win)
//   no candidate    C, empty      -       0         none          -
//   one candidate   C             -       C.idx[0]  0             -
//   Monte Carlo     C             yes     chosen    best          -
//
// `chosen` indexes the legal moves and is the row the answer marks. `best`
// indexes the candidates and is what Mode B stores (-1 stores nothing). They
// are two arguments because at the Monte Carlo exit they are two moves: the
// information-hiding rule can turn the sampled best into a pickup, and the
// answer marks the pickup while Mode B keeps the sampled best; and when
// nothing was sampled at all, the answer marks move 0 and Mode B stores no
// choice. Only the Monte Carlo exit has scores, so only it folds sums, sample
// counts and forced-loss marks into the accumulator - the other exits publish
// the table and the verdicts and count the batch.
//
// The first two exits need something the chooser does not have at that point
// (a belief, a candidate set), which is built only when somebody is reading.
// They get a wrapper each, so the chooser never builds it in a shipped build.
#if defined(OG_EXPLAIN_BUILD) || defined(FOOLISH_ORACLE_MT)
// Is anybody reading? Mode B always is; an explain build only when asked.
static int og_publishing(void) {
#ifdef FOOLISH_ORACLE_MT
    return 1;
#else
    return og_explain_on();
#endif
}
#endif

// Forget the last decision's exact verdicts, before the root endgame solve
// probes this one's (og_try_endgame_solve).
static void og_verdicts_reset(void) {
#ifdef OG_EXPLAIN_BUILD
    if (og_explain_on()) {
        og_ex_solve_applied = 0;
        for (int i = 0; i < (int)(sizeof(og_ex_verdict)/sizeof(og_ex_verdict[0])); i++)
            og_ex_verdict[i] = OG_EX_NONE_V;
    }
#elif defined(FOOLISH_ORACLE_MT)
    og_ex_solve_applied = 0;
    for (int i = 0; i < (int)(sizeof(og_ex_verdict)/sizeof(og_ex_verdict[0])); i++)
        og_ex_verdict[i] = OG_EX_NONE_V;
#endif
}

// score/nsim/alive index by CANDIDATE position (matching C->idx[]) and are
// NULL at every exit but the Monte Carlo one; forced_loss indexes the legal
// moves and is NULL only when one move was legal.
static void og_publish(const Game *g, int bot_idx, const LegalMoves *moves,
                       const Belief *B, const Candidates *C,
                       const double *score, const int *nsim, const bool *alive,
                       const bool *forced_loss, int chosen, int best, int win_idx) {
    (void)g; (void)bot_idx; (void)moves; (void)B; (void)C; (void)score; (void)nsim;
    (void)alive; (void)forced_loss; (void)chosen; (void)best; (void)win_idx;
#ifdef OG_EXPLAIN_BUILD
    if (og_explain_on())
        og_ex_emit(g, bot_idx, moves, B, C, score, nsim, alive, forced_loss,
                   chosen, og_ex_solve_applied, win_idx);
#endif
#ifdef FOOLISH_ORACLE_MT
    // MT5 (docs/INFINITE_ORACLE_DESIGN.md §8b.5): fold this batch's per-candidate
    // rollout scores into the shared accumulator, once the candidate set has
    // been agreed with whatever thread published first. score[] is a sum of
    // finish positions (ints 1..N), so the u64 atomic add is exact - no float
    // reduction, and no dependence on the order the threads happen to land in.
    if (og_mt_commit_desc(moves, C)) {
        og_mt_capture_verdicts(C);            // NONE unless the endgame probe fired
        if (score) {
            for (int i = 0; i < C->n && i < OG_MT_MAX_CANDS; i++) {
                if (nsim[i] > 0) {
                    __atomic_fetch_add(&g_ogmt.sum_fp[i], (uint64_t)(score[i] + 0.5), __ATOMIC_RELAXED);
                    __atomic_fetch_add(&g_ogmt.nsim[i], (uint32_t)nsim[i], __ATOMIC_RELAXED);
                }
                if (forced_loss[C->idx[i]])
                    __atomic_store_n(&g_ogmt.forced_loss[i], 1u, __ATOMIC_RELAXED);
            }
        }
        if (best >= 0) __atomic_store_n(&g_ogmt.chosen, best, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_ogmt.batches, 1u, __ATOMIC_RELAXED);
    }
#endif
}

// The degenerate exit: one legal move, nothing to sample. It runs BEFORE the
// belief is built and before the verdicts are reset, so it resets the solver
// mark itself, and builds a belief only for an answer that is being read.
// Published so the answer and the overlay have their one row, and the batch
// is counted.
static void og_publish_only_move(const Game *g, int bot_idx, const LegalMoves *moves) {
    (void)g; (void)bot_idx; (void)moves;
#if defined(OG_EXPLAIN_BUILD) || defined(FOOLISH_ORACLE_MT)
    if (!og_publishing()) return;
    og_ex_solve_applied = 0;
    Candidates TC; TC.n = 1; TC.idx[0] = 0;
    const Belief *B = NULL;
#ifdef OG_EXPLAIN_BUILD
    Belief lb;
    if (og_explain_on()) { og_build_belief(g, bot_idx, &lb); B = &lb; }   // for the known-state dump only
#endif
    og_publish(g, bot_idx, moves, B, &TC, NULL, NULL, NULL, NULL, 0, 0, -1);
#endif
}

// Proven win: octogen returns the winning move directly (no MC), so there is
// no candidate set yet. Pick one - what Monte Carlo would have been handed -
// so the answer and the controller show the exact panel with its verdicts.
static void og_publish_win(const Game *g, int bot_idx, const LegalMoves *moves,
                           const Belief *B, const bool *forced_loss, int solved) {
    (void)g; (void)bot_idx; (void)moves; (void)B; (void)forced_loss; (void)solved;
#if defined(OG_EXPLAIN_BUILD) || defined(FOOLISH_ORACLE_MT)
    if (!og_publishing()) return;
    Candidates EC; og_pick_candidates(g, moves, forced_loss, &EC);
    int row = -1;
    for (int i = 0; i < EC.n; i++)
        if (EC.idx[i] == solved) { row = i; break; }
    og_publish(g, bot_idx, moves, B, &EC, NULL, NULL, NULL, forced_loss, solved, row, solved);
#endif
}

// The Mode A path trace (CD_ORC_TRACE, the answer's why table), around one
// deliberation and around each root-move playout of it. Empty elsewhere.
static inline void og_trace_reset(void) {
#ifdef CD_ORC_TRACE
    og_orc_reset();   // fresh path aggregation per deliberation
#endif
}
static inline void og_trace_begin(int bot_idx) {
    (void)bot_idx;
#ifdef CD_ORC_TRACE
    cd_orc_begin(bot_idx);   // trace root apply + playout
#endif
}
static inline void og_trace_end(int ci, int fp) {
    (void)ci; (void)fp;
#ifdef CD_ORC_TRACE
    cd_orc_end();
    og_orc_fold(ci, fp);
#endif
}

// ---------- the Monte Carlo stages -------------------------------------------

// Trump-conservation tie-break tax for one candidate (added to its mean-finish
// score ONLY at selection; the raw MC scores in score[]/nsim[] are left
// untouched so the OG_EXPLAIN dump still shows true rollout values). Attacks
// only, deck alive only - leading/throwing a trump is discretionary spend;
// covering with one is forced defense and endgame trumps must be used.
static double og_keep_penalty(const Game *g, const LegalMove *m) {
    double pen = 0.0;
    if (og_trump_keep > 0.0 && (g->deck_count > 0 || g->has_flipped) && m->type == MOVE_ATTACK)
        for (int k = 0; k < m->n_cards; k++)
            if (m->cards[k].suit == g->power_suit)
                pen += og_trump_keep;
    return pen;
}

// After stage 0 and stage 1: drop the worst candidates until `keep` are alive.
static void og_cull(int stage, int n, const double *keep_pen,
                    const double *score, const int *nsim, bool *alive) {
    int n_alive = 0;
    for (int i = 0; i < n; i++) if (alive[i]) n_alive++;
    int keep;
    if (stage == 0) {
        keep = (og_keep1 > 0) ? og_keep1 : (og_oracle ? (n + 1) / 2 : n / 3);
        if (keep < (og_oracle ? 4 : 3)) keep = og_oracle ? 4 : 3;
    } else {
        keep = (og_keep2 > 0) ? og_keep2 : (og_oracle ? 3 : 2);
    }
    if (keep >= n_alive) return;
    for (int dropped = n_alive - keep; dropped > 0; dropped--) {
        int worst = -1;
        double worst_v = -1e30;
        for (int i = 0; i < n; i++) {
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

// The three-stage race over shared sampled worlds. On return score[]/nsim[]
// hold each candidate's summed finish positions and sample count, and alive[]
// the survivors of the two culls.
//
// The two rollout paths stay written out in the world loop on purpose. Lifted
// into a function per path they inline back, and bots.wasm still came out
// 182 B larger for it (measured at the shipped flags); this is the hot loop,
// so it keeps the shape it was profiled in.
static void og_run_stages(const Game *g, int bot_idx, const LegalMoves *moves,
                          const Belief *B, const Candidates *C, const double *keep_pen,
                          double *score, int *nsim, bool *alive) {
    int W1, W2, W3;
    og_params(g->num_players, &W1, &W2, &W3);

    // World-sampling seed. Depends ONLY on the SERVER-ONLY secret - the strategy
    // LCG, reseeded per decision from state_fnv(g_rng_base) (see wasm_api.c) -
    // and the deciding seat. It deliberately folds in NO other game state.
    // Rationale: anything that seeds the RNG but is NOT recoverable from a shared
    // replay makes a recorded game impossible to reproduce bit-exactly. num_logs
    // (records the codec may drop), the ordered hands and the face-down deck are
    // all either non-reproducible or hidden from the bot; a player could even
    // perturb them (hand rearrange). Unpredictability still rests entirely on the
    // secret deal seed - a source holder can't recompute it - and the W diverse
    // wseeds within a decision come from (w+1) below, so MC coverage is intact.
    // bot_idx only decorrelates co-seated bots in multi-bot games.
    uint32_t base = og_mix((uint32_t)bot_idx * 2654435761u, random_strategy_rng_get());

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
            og_sample_world(world, g, bot_idx, B, wseed, use_voids, use_floors);
            uint32_t sim_rng = og_mix(wseed, 0x51AB1E5u);

            if (fast_path) {
                cd_sim_from_game(world_sim, world);     // convert world ONCE
                bool reply_stage = og_reply && stage >= og_reply_stage;
                for (int ci = 0; ci < C->n; ci++) {
                    if (!alive[ci]) continue;
                    *trial_sim = *world_sim;            // cheap struct copy
                    game_rng_set(sim_rng);              // identical stream
                    og_trace_begin(bot_idx);
                    int fp;
                    if (!cd_sim_apply_root_move(trial_sim, bot_idx,
                                                &moves->moves[C->idx[ci]])) {
                        fp = g->num_players;
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
                    og_trace_end(ci, fp);
                }
                continue;
            }

            for (int ci = 0; ci < C->n; ci++) {
                if (!alive[ci]) continue;
                og_lite_clone(trial, world);
                game_rng_set(sim_rng);   // identical stream for every move
                if (!legal_move_apply(trial, bot_idx, &moves->moves[C->idx[ci]])) {
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
        if (stage < 2) og_cull(stage, C->n, keep_pen, score, nsim, alive);
    }
}

// Information-hiding rule (docs/OCTOGEN_HIDE_UNCOVERABLE.md). If octogen is
// about to play a COVER while it cannot cover EVERY uncovered card on the
// table (no full cover exists in the legal set), pick up immediately. A
// doomed partial cover only leaks its cover cards - returned to hand and
// logged by the pickup that is coming anyway - to the memory-keeping MC
// opponents, and covering opens throw-in windows that grow the pickup.
// Weakly dominant: never costs a card or tempo. ON for every seat.
// Returns the move to play: `chosen`, or the pickup in its place.
static int og_hide_uncoverable(const Game *g, const LegalMoves *moves, int chosen) {
    if (chosen >= 0 && chosen < moves->n && moves->moves[chosen].type == MOVE_COVER) {
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
        if (!full_cover && pickup_idx >= 0) chosen = pickup_idx;
    }
    return chosen;
}

static int octogen_choose_impl(const Game *g, int bot_idx,
                            const LegalMoves *moves, void *ctx) {
    (void)ctx;
    if (moves->n == 0) return -1;
    if (moves->n == 1) {
        og_publish_only_move(g, bot_idx, moves);
        return 0;
    }

    if (!og_flags_loaded) og_load_flags();

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
    og_verdicts_reset();
    int solved = og_no_solve ? -1
               : og_try_endgame_solve(g, bot_idx, moves, &B, forced_loss, &n_safe);
    if (solved >= 0) {
        og_publish_win(g, bot_idx, moves, &B, forced_loss, solved);
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
        og_publish(g, bot_idx, moves, &B, &C, NULL, NULL, NULL, forced_loss, 0, -1, -1);
        game_rng_set(saved_rng); return 0;
    }
    if (C.n == 1) {
        og_publish(g, bot_idx, moves, &B, &C, NULL, NULL, NULL, forced_loss, C.idx[0], 0, -1);
        game_rng_set(saved_rng); return C.idx[0];
    }

    double score[OG_MAX_CANDS] = {0};
    int    nsim [OG_MAX_CANDS] = {0};
    bool   alive[OG_MAX_CANDS];
    og_trace_reset();
    for (int i = 0; i < C.n; i++) alive[i] = true;

    double keep_pen[OG_MAX_CANDS];
    for (int i = 0; i < C.n; i++) keep_pen[i] = og_keep_penalty(g, &moves->moves[C.idx[i]]);

    og_run_stages(g, bot_idx, moves, &B, &C, keep_pen, score, nsim, alive);

    int best = -1;
    double best_v = 1e30;
    for (int i = 0; i < C.n; i++) {
        if (!alive[i] || nsim[i] == 0) continue;
        double v = score[i] / (double)nsim[i] + keep_pen[i];
        if (v < best_v) { best_v = v; best = i; }
    }

    int chosen = og_hide_uncoverable(g, moves, best >= 0 ? C.idx[best] : 0);
    og_publish(g, bot_idx, moves, &B, &C, score, nsim, alive, forced_loss, chosen, best, -1);
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
