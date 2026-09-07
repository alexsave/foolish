// CL-20 — octogen's successor (HNIW is the rung above HMX on the
// explosives ladder). Octogen's engine forked verbatim (the research-only
// CL_EXPLAIN / oracle-MT scaffolding stripped), with cl_/CL_ prefixes so the
// two bots latch independent knobs; the levers that make it cl20 are
// documented in CL20.md and gated by CL_* knobs below. With every lever off
// it is decision-identical to octogen (paired 0/0/N).
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

#include "cl20_strategy.h"
#include "strategy.h"
#include "bot_knobs.h"
#include "card.h"
#include "game.h"
#include "cordite_sim.h"
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

// ---------- small utils ------------------------------------------------

static inline int cl_card_score(Card c, int power) {
    return c.value + (c.suit == power ? 1000 : 0);
}

static bool cl_set_contains(const Card *arr, int n, Card c) {
    for (int i = 0; i < n; i++) if (card_eq(arr[i], c)) return true;
    return false;
}

static uint32_t cl_xorshift(uint32_t s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s ? s : 0xB1A570u;
}

static uint32_t cl_mix(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u);
    h ^= h >> 16; h *= 0x85EBCA77u; h ^= h >> 13;
    return h ? h : 1;
}

// Ablation switches (read once): CL_NO_SOLVE / CL_NO_VOIDS / CL_NO_FLIP /
// CL_NO_FLOORS / CL_NO_LEAF / CL_NO_AVOID / CL_VERIFY, plus CL_W1/CL_W2
// world-count overrides for tuning.
// Values come from the bot roster's knob spec, with env still overriding for
// research/ablation sweeps (bot_knobs.h).
static int cl_flag(const char *name) {
    return bot_knob_flag(name);
}
static int cl_env_int(const char *name, int def) {
    return bot_knob_int(name, def);
}
static _Thread_local int cl_flags_loaded = 0;
// Drop the latched CL_* values so the next choose re-reads them. Two callers:
// bot_roster_choose, which installs a different knob spec per named bot; and
// the infinite-oracle bridge (client replay analysis,
// docs/INFINITE_ORACLE_DESIGN.md §6.2), which re-reads CL_* between
// deliberation batches so the per-batch world budget can adapt to the measured
// device speed. Mirrors cordite_reload_flags (cordite_strategy.c). Only the
// oracle's *wasm export* is oracle-build-only (wasm_bots_api.c).
void cl_reload_flags(void) { cl_flags_loaded = 0; }
static _Thread_local int cl_no_solve = 0, cl_no_voids = 0, cl_no_flip = 0;
static _Thread_local int cl_no_floors = 0, cl_no_leaf = 0, cl_no_avoid = 0;
static _Thread_local int cl_no_earlyexit = 0;
static _Thread_local int cl_verify = 0;
static _Thread_local int cl_no_fastroll = 0;   // CL_NO_FASTROLL=1: struct rollout
// Bitboard exact-leaf endgames inside rollouts (semtex's own lever): resolve
// small 2-player deck-empty rollout endgames with the fast bitboard solver
// instead of handwritten policy play. Against opponents that themselves play
// endgames exactly (cordite), the exact model is the realistic one.
// CL_BBLEAF: 2 (default) = pc-aware exact leaf endgames in rollouts —
// small (CL_BBLEAF_CARDS2, 8) leaves heads-up only; CL_BBLEAF_CARDS
// (default 0 = off) at 3+ players. Loss analysis showed 12-card leaves at
// 3+ inject "the endgame will be played perfectly" into mid-game values —
// individually terrible calls (trump-burning covers, passive pickups) that
// exactly cancel the good calls (paired mirror delta ~0) while costing 3x
// wall-clock; the replicated win is the heads-up leaf. 1 = CL_BBLEAF_CARDS
// everywhere; 0 = off.
static _Thread_local int cl_bbleaf = 2;
static _Thread_local int cl_bbleaf_cards2 = 8;
// CL_ADAPT: void-contradiction => per-seat distrust of floors+voids (on by
// default — pure evidence, no downside). CL_PROFILE: weak-seat detection +
// LOOSE rollout model for profiled seats.
static _Thread_local int cl_adapt = 1;
// Reply tournament (cl20's lever): in late-stage worlds the first
// opponent reply is chosen by search over their legal replies instead of
// assumed from the rollout policy. CL_REPLY: 0 off (== semtex), 1 on
// (default). CL_REPLY_CAP: replies searched per world (cheap-first ranked;
// good/pickup always ranked last so they stay in range). CL_REPLY_STAGE:
// first MC stage (0-2) that uses the tournament (default 2 = final duel).
static _Thread_local int cl_reply = 0;   // flat vs semtex; research knob
static _Thread_local int cl_reply_cap = 6;
static _Thread_local int cl_reply_stage = 2;
// CL_MCDEF (default 0 — measured flat-to-harmful vs semtex, pc3
// +0.055±0.039; the 50% pickup rate over-models strategic pickups):
// seats with a proven mc_tell roll out with CD_POL_MCDEF. Research knob.
static _Thread_local int cl_mcdef = 1;
// Void world-mixture: voids applied in (mod-1)/mod of sampled worlds
// (cordite: 3 of 4). A softer mixture hedges between heuristic-family
// opponents (voids true) and MC/human strategic pickups (voids misleading).
static _Thread_local int cl_void_mod = 4;
static _Thread_local int cl_profile = 0;
static _Thread_local int cl_profile_viol0 = 2;        // CL_PROFILE_V0: violations tolerated for free
static _Thread_local double cl_profile_viol_w = 0.25; // CL_PROFILE_VW/100 per extra violation
static _Thread_local long cl_prof_stat[64][2];        // [strategy_key][0=seat-decisions,1=loose]
static _Thread_local int cl_prof_stats_on = 0;
// Root endgame-solve card ceiling (cordite: 20). The bitboard solver + TT can
// resolve bigger endgames within budget; a higher ceiling opens a window where
// semtex plays exactly while cordite still guesses with MC.
static _Thread_local int cl_solve_cards = 20;
// Loss-avoidance ceiling (CL_AVOID_CARDS, default 24 = semtex's window).
// The extended 25-28-card window is WIN-HUNT ONLY: taking a proven win is
// strictly safe (the win re-proves at every subsequent in-window ply), but
// avoiding "proven losing" moves out there measured HARMFUL against a
// near-peer (semtex tables pc2: 0 better/5 worse/195) — under optimal play
// they lose, but against an equally imperfect opponent they carry the best
// swindle equity, which the safe move forfeits. CORDITE.md's
// adverse-selection guard, one level up.
static _Thread_local int cl_avoid_cards = 24;
static _Thread_local int cl_bbleaf_cards = 12;
static _Thread_local long cl_bbleaf_budget = 3000;
// Per-seat rollout policy map for the current decision (NULL = all
// handwritten). Set by cl20_strategy_choose when profiling flags seats.
static _Thread_local uint8_t cl_polmap_buf[MAX_PLAYERS];
static _Thread_local const uint8_t *cl_polmap = NULL;
static _Thread_local int cl_bbleaf_on = 0;   // effective flag for this decision
static _Thread_local int cl_bbleaf_cards_eff = 12;
static _Thread_local int cl_difftest = 0;      // CL_DIFFTEST=1: assert fast==slow
static _Thread_local int cl_w1_override = 0, cl_w2_override = 0;
static _Thread_local int cl_w3_override = -1;
static _Thread_local int cl_old_budget = 0;    // cordite_old variant: pre-2x worlds
static _Thread_local int cl_keep1 = 0, cl_keep2 = 0;  // CL_KEEP1/2: candidates kept past stage 0/1 (0=default n/3, 2)
// Oracle mode (research): multiply the world budget and widen candidate
// survival for one call. Used by cl20_oracle_strategy_choose to audit
// loss games — a decision the oracle changes was compute-limited.
static _Thread_local int cl_oracle = 0;
static _Thread_local int cl_rollout_policy = 0;       // CL_ROLLOUT: 0=default, 1=espresso, 2=handwritten (struct path)
// Trump-conservation tie-break: the weak handwritten rollout policy undervalues
// keeping trumps while the deck is alive, so leading a low trump scores ~equal
// to a junk card and MC noise picks it ~half the time (measured: 52.5% -> 36.7%
// under a stronger rollout). A small tax per trump LED (attack only; covers are
// forced defense) added to the mean-finish score at selection tips near-ties
// toward the cheap non-trump WITHOUT overriding a genuine gap. In milli-units of
// mean-finish-position via CL_TRUMP_KEEP (default 40 = 0.040; MC noise between
// near-tied candidates ~0.05). 0 disables. Endgame (deck dead) is never taxed.
static _Thread_local double cl_trump_keep = 0.040;
// Bitboard endgame-solver node budgets (per shared pass). The bitboard solver
// (transposition table + O(1) clone) resolves far more per node than the
// struct solver, so it needs a much smaller node budget to do equivalent work
// in less wall-clock. Tunable via CL_BB_WIN / CL_BB_AVOID for sweeps.
static _Thread_local int cl_bb_win_budget = 20000;
static _Thread_local int cl_bb_avoid_budget = 15000;


static int cl_in_count(const Game *g) {
    int n = 0;
    for (int i = 0; i < g->num_players; i++)
        if (g->players[i].status == PLAYER_STATUS_IN) n++;
    return n;
}

static bool cl_apply(Game *g, int p_idx, const LegalMove *m) {
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
static void cl_lite_clone(Game *dst, const Game *src) {
    size_t base = offsetof(Game, logs);
    memcpy(dst, src, base + (size_t)src->num_logs * sizeof(GameLog));
}

// ---------- belief state ------------------------------------------------

#define CL_MAX_VOIDS 6

// ---------- policy-consistency constraints (CL_HWC) ------------------------

#define CL_MAX_CONS   48
#define CL_CON_FORBID 0   // seat could not hold any card in `forbid` at time t
#define CL_CON_ATTACK 1   // first attack of k non-trump cards of value v, defender cap
#define CL_CON_PICKUP 2   // picked up facing unc[]: greedy full cover must FAIL

typedef struct {
    uint8_t  seat, kind;
    uint8_t  k, v, cap;
    uint8_t  unc_n;
    uint8_t  unc[8];          // PICKUP: uncovered attack ids, table order
    uint64_t forbid;          // FORBID
    uint64_t held;            // cards KNOWN to have been in the hand at t
    uint64_t gained_after;    // cards publicly gained after t (pickups)
    // Draw persistence: a constraint on the hand at t survives later draws as
    // a COUNT constraint — at most `drawn` of the current unknown cards may
    // violate it (only drawn cards can). `used` = forbidden cards the seat has
    // played since its first draw after t (they must have been drawn, so they
    // consume the slack); used > drawn is a contradiction.
    int8_t   drawn, used;
    uint8_t  src;             // originating rule (audit): see CL_SRC_*
} HwCon;
#define CL_SRC_ATK_TRUMP 1
#define CL_SRC_ATK_VALUE 2
#define CL_SRC_ATK_COUNT 3
#define CL_SRC_THROWIN   4
#define CL_SRC_COVER_PASS 5
#define CL_SRC_COVER_GREEDY 6
#define CL_SRC_GOOD      7
#define CL_SRC_PICKUP_PASS 8
#define CL_SRC_PICKUP_COVER 9
#define CL_SRC_PICKUP_MULTI 10


typedef struct {
    Card pool[80];                  // unseen pool (deck ∪ opp unknowns)
    int  n;
    Card pinned[MAX_PLAYERS][MAX_HAND_SIZE];  // publicly located in p's hand
    int  pinned_n[MAX_PLAYERS];
    // Hard-ish void constraints (see blackpowder): attack cards a defender
    // demonstrably couldn't cover at pickup time. Cleared on their draw.
    Card voids[MAX_PLAYERS][CL_MAX_VOIDS];
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
    // CL-20 policy-consistency constraints (CL_HWC, see CL20.md): every
    // decision by a handwritten-family seat is a deterministic function of its
    // hand, so each observed move is an exact constraint on that hand until the
    // seat's next unknown draw. Seats proven non-handwritten (a play that a live
    // constraint forbids, a partial cover, a mixed trump/non-trump attack, an
    // infeasible constraint set) drop them for the rest of the game.
    HwCon    cons[CL_MAX_CONS];
    int      ncons;
    bool     soft_void[MAX_PLAYERS];  // CL_MCVOID: strategic seat, voids mean "no cheap cover"
    bool     not_hw[MAX_PLAYERS];     // sticky: proven not handwritten-family
    int      hw_viol[MAX_PLAYERS];    // proven handwritten-consistency violations
    bool     hwc_on[MAX_PLAYERS];     // constraints apply to this seat now
    uint64_t forbid_all[MAX_PLAYERS]; // OR of the seat's live FORBID masks
} Belief;


static inline int cl_cid(Card c) { return c.suit * 13 + (c.value - 1); }
static inline uint64_t cl_bit(Card c) { return 1ull << cl_cid(c); }
static inline uint64_t cl_trump_mask(int power) { return 0x1FFFull << (power * 13); }
static inline uint64_t cl_value_mask(int v) {
    uint64_t m = 0;
    for (int s = 0; s < 4; s++) m |= 1ull << (s * 13 + v - 1);
    return m;
}
static inline int cl_id_score(int id, int power) {
    return (id % 13 + 1) + ((id / 13) == power ? 1000 : 0);
}
// Every card id that can cover attack `a`.
static uint64_t cl_covers_mask(Card a, int power) {
    uint64_t m = 0;
    for (int v = a.value + 1; v <= ACE_VALUE; v++) m |= 1ull << (a.suit * 13 + v - 1);
    if (a.suit != power) m |= cl_trump_mask(power);
    return m;
}
// Every card id whose handwritten score is strictly below `score`.
static uint64_t cl_score_below_mask(int score, int power) {
    uint64_t m = 0;
    for (int id = 0; id < 52; id++) if (cl_id_score(id, power) < score) m |= 1ull << id;
    return m;
}
static _Thread_local int cl_hwc = 1;          // CL_HWC: 0 off, 1 on
static _Thread_local int cl_selfpol = 0;      // CL_SELFPOL: rollout policy for OUR seat (CD_POL_*)
static _Thread_local int cl_mcvoid = 0;       // CL_MCVOID: soft voids for proven-strategic seats
static _Thread_local int cl_mcpol = 0;        // CL_MCPOL: CD_POL_MC model for proven-strategic seats
// Live-deck exact evaluation (CL_XDECK, CL20.md): heads-up with at most
// CL_XDECK_CARDS cards left in the deck (flipped trump included) and at most
// CL_XDECK_TOTAL cards in play, every (world x candidate) is valued by an exact
// sign-only solve of the rest of the game in that world (the deck order is
// fixed inside a determinized world) instead of a handwritten rollout; an
// aborted solve (CL_XDECK_BUDGET nodes) falls back to the rollout.
// CL_WMUL6: world-budget multiplier at 6+ players (the one regime where the
// compute probe still pays, CL20.md). 1 = octogen's budget.
static _Thread_local int cl_wmul6 = 1;
static _Thread_local int cl_xdeck = 0;
static _Thread_local int cl_xdeck_cards = 4;
static _Thread_local int cl_xdeck_total = 20;
static _Thread_local long cl_xdeck_budget = 40000;
static _Thread_local int cl_xdeck_worlds = 0;      // 0 = octogen's W1/W2/W3; else this many worlds, one stage
static _Thread_local long cl_xd_stat_solves = 0, cl_xd_stat_abort = 0;
// Adaptive deliberation (CL20.md): after a stage's fixed budget, if the two
// leading candidates are not statistically separated on their paired
// same-world finishes, the stage is extended by another block of shared
// worlds, up to CL_ADAPT_K extra blocks. CL_ADAPT_STAGE = first stage it
// applies to (2 = the final duel only). CL_ADAPT_C = separation threshold in
// standard errors of the paired difference.
// Self-search (CL_SELF): in worlds of stage >= CL_SELF_STAGE, my first
// post-root decision inside the rollout is searched (cd_sim_playout_self)
// over up to CL_SELF_CAP moves instead of played by the handwritten policy.
static _Thread_local int cl_self = 0;
static _Thread_local int cl_self_cap = 6;
static _Thread_local int cl_self_stage = 2;
// Race deliberation (CL_RACE, CL20.md): replaces the fixed 3-stage schedule.
// Worlds are sampled in blocks for every surviving candidate; after each block
// a candidate whose paired same-world finish is worse than the leader's by
// more than CL_RACE_C standard errors is dropped, and the race ends when one
// candidate remains, the last two are separated by CL_RACE_CF SEs, or the
// world cap (CL_RACE_MULT x octogen's total budget) is reached. Landslides
// finish in a fraction of octogen's budget; close calls get several times it.
static _Thread_local int cl_race = 0;
static _Thread_local int cl_race_mult = 3;
static _Thread_local double cl_race_c = 2.0, cl_race_cf = 2.0;
static _Thread_local int cl_race_block = 0;
static _Thread_local int cl_race_min_blocks = 2;
static _Thread_local int cl_adapt_k = 5;
static _Thread_local int cl_adapt_stage = 2;
static _Thread_local double cl_adapt_c = 2.0;
#define CL_MAX_W 8192
static _Thread_local int8_t cl_fpw[26][CL_MAX_W];   // per-world finish per candidate
static _Thread_local int cl_hwc_mod = 4;      // constraints applied in (mod-1)/mod worlds
static _Thread_local int cl_hwc_tries = 12;   // rejection-sampling retries per world
static _Thread_local long cl_hwc_stat_worlds = 0, cl_hwc_stat_rejects = 0, cl_hwc_stat_fail = 0;

void cl_hwc_stats_report(void) {
    fprintf(stderr, "[cl20 hwc] constrained worlds=%ld rejects=%ld unrepaired=%ld\n",
            cl_hwc_stat_worlds, cl_hwc_stat_rejects, cl_hwc_stat_fail);
}


void cl_prof_stats_report(void) {
    for (int k = 0; k < 64; k++)
        if (cl_prof_stat[k][0])
            fprintf(stderr, "[cl20 profile] strat=%d seat-decisions=%ld loose=%ld (%.2f%%)\n",
                    k, cl_prof_stat[k][0], cl_prof_stat[k][1], 100.0 * cl_prof_stat[k][1] / cl_prof_stat[k][0]);
}


extern _Thread_local long cd_sim_abort_depth, cd_sim_abort_budget;
void cl_xdeck_stats_report(void) {
    fprintf(stderr, "[cl20 xdeck] solves=%ld aborted=%ld (%.1f%%)  depth-aborts=%ld budget-aborts=%ld\n",
            cl_xd_stat_solves, cl_xd_stat_abort,
            cl_xd_stat_solves ? 100.0 * cl_xd_stat_abort / cl_xd_stat_solves : 0.0,
            cd_sim_abort_depth, cd_sim_abort_budget);
}

static HwCon *cl_con_new(Belief *B, int seat, int kind, uint64_t held) {
    if (B->ncons >= CL_MAX_CONS) {
        // Drop the OLDEST constraint of this seat (or the oldest overall).
        int drop = -1;
        for (int i = 0; i < B->ncons; i++) if (B->cons[i].seat == seat) { drop = i; break; }
        if (drop < 0) drop = 0;
        for (int i = drop; i + 1 < B->ncons; i++) B->cons[i] = B->cons[i + 1];
        B->ncons--;
    }
    HwCon *c = &B->cons[B->ncons++];
    memset(c, 0, sizeof(*c));
    c->seat = (uint8_t)seat; c->kind = (uint8_t)kind; c->held = held;
    return c;
}
static uint64_t cl_pinned_mask(const Belief *B, int p) {
    uint64_t m = 0;
    for (int q = 0; q < B->pinned_n[p]; q++) m |= cl_bit(B->pinned[p][q]);
    return m;
}
// Seat `p` played card `c` (attack/pass/cover): it was in the hand at every
// live constraint's time t unless publicly gained after t. A play that a
// FORBID constraint excludes proves the seat is not handwritten-family.
static void cl_con_note_play(Belief *B, const Game *g, int p, Card c) {
    uint64_t b = cl_bit(c);
    bool viol = false;
    for (int i = 0; i < B->ncons; i++) {
        HwCon *k = &B->cons[i];
        if (k->seat != p) continue;
        if (k->gained_after & b) continue;
        bool v = false;
        if (k->drawn == 0) {
            k->held |= b;
            if (k->kind == CL_CON_FORBID && (k->forbid & b)) v = true;
        } else if (k->kind == CL_CON_FORBID && (k->forbid & b)) {
            if (++k->used > k->drawn) v = true;
        }
        if (v) {
            viol = true;
            if (cl_verify)
                fprintf(stderr, "CL_VERIFY: play-violation seat=%d strat=%d src=%d card v%d s%d drawn=%d used=%d\n",
                        p, g->players[p].strategy_key, k->src, c.value, c.suit, k->drawn, k->used);
        }
    }
    if (viol) { B->not_hw[p] = true; B->hw_viol[p]++; }   // one strike per play
}
// Seat p drew n unknown cards: constraints persist as count constraints;
// PICKUP (greedy-cover-fails) has no sound count form and is dropped.
static void cl_con_note_draw(Belief *B, int p, int n) {
    int w = 0;
    for (int i = 0; i < B->ncons; i++) {
        HwCon *k = &B->cons[i];
        if (k->seat != p) { B->cons[w++] = *k; continue; }
        if (k->kind == CL_CON_PICKUP) continue;
        int d = k->drawn + n;
        k->drawn = (int8_t)(d > 100 ? 100 : d);
        B->cons[w++] = *k;
    }
    B->ncons = w;
}
static void cl_con_note_gain(Belief *B, int p, uint64_t gained) {
    for (int i = 0; i < B->ncons; i++)
        if (B->cons[i].seat == p) B->cons[i].gained_after |= gained;
}
// Non-trump card count of value v in hand mask h.
static inline int cl_nt_count(uint64_t h, int v, uint64_t trump) {
    return __builtin_popcountll(h & cl_value_mask(v) & ~trump);
}
// ATTACK: handwritten's first attack is the value with the most non-trump
// cards (capped by the defender's hand), ties to the lowest value.
static bool cl_con_attack_ok(const HwCon *k, uint64_t h, int power, int min_v) {
    uint64_t trump = cl_trump_mask(power);
    int excess = 0;   // cards that must have been drawn since t for h to be consistent
    for (int u = min_v; u <= ACE_VALUE; u++) {
        if (u == k->v) continue;
        int n = cl_nt_count(h, u, trump);
        if (u < k->v) { if (n >= k->k) excess += n - (k->k - 1); }
        else if (k->k < k->cap) { if (n > k->k) excess += n - k->k; }
    }
    return excess <= k->drawn;
}
// PICKUP: handwritten covers whenever its greedy (table-order, lowest-score
// cover per attack) full cover succeeds; a pickup means it failed.
static bool cl_con_pickup_ok(const HwCon *k, uint64_t h, int power) {
    uint64_t avail = h;
    for (int i = 0; i < k->unc_n; i++) {
        uint64_t cov = cl_covers_mask(card_of_id(k->unc[i]), power) & avail;
        if (!cov) return true;      // greedy fails here: consistent with a pickup
        int best = -1, best_s = 1 << 20;
        while (cov) {
            int id = __builtin_ctzll(cov); cov &= cov - 1;
            int s = cl_id_score(id, power);
            if (s < best_s) { best_s = s; best = id; }
        }
        avail &= ~(1ull << best);
    }
    return false;                    // a full greedy cover existed: contradiction
}

static bool cl_void_forbidden(const Belief *B, const Game *g, int p, Card c) {
    for (int k = 0; k < B->void_n[p]; k++) {
        Card v = B->voids[p][k];
        if (B->soft_void[p]) {
            // A strategic (MC) seat picks up to protect trumps, not to keep a
            // cheap same-suit cover: the void excludes only same-suit higher
            // non-trump cards; trumps stay possible.
            if (c.suit == v.suit && c.suit != g->power_suit && c.value > v.value) return true;
            continue;
        }
        if (can_cover(v, c, g->power_suit)) return true;
    }
    return false;
}

static bool cl_floor_forbidden(const Belief *B, const Game *g, int p, Card c) {
    return B->floor_v[p] > 0 && c.suit != g->power_suit && c.value < B->floor_v[p];
}

static void cl_pinned_remove(Belief *B, int p, Card c) {
    for (int q = 0; q < B->pinned_n[p]; q++) {
        if (card_eq(B->pinned[p][q], c)) {
            B->pinned[p][q] = B->pinned[p][B->pinned_n[p] - 1];
            B->pinned_n[p]--;
            return;
        }
    }
}

static void cl_pinned_add(Belief *B, int p, Card c) {
    if (B->pinned_n[p] >= MAX_HAND_SIZE) return;
    if (cl_set_contains(B->pinned[p], B->pinned_n[p], c)) return;
    B->pinned[p][B->pinned_n[p]++] = c;
}

// A floor contradiction (p plays a non-trump card below their inferred
// floor) means p is not a lowest-first attacker: distrust their floors.
static void cl_floor_check(Belief *B, const Game *g, int p, Card c) {
    if (p < 0 || B->floor_v[p] <= 0) return;
    if (c.suit != g->power_suit && c.value < B->floor_v[p]) {
        B->floor_v[p] = 0;
        B->distrust_floor[p] = true;
    }
}

// Chronological scan over logs: pinned cards, flipped-trump holder, void
// constraints, rank floors and trust flags, all in one pass.
static void cl_build_belief(const Game *g, int bot_idx, Belief *B) {
    memset(B, 0, sizeof(*B));

    // Last draw event: holds the flipped trump if the deck is exhausted, and
    // marks the moment the deck died (for "deck alive at log i" tests).
    int last_draw_idx = -1;
    for (int i = 0; i < g->num_logs; i++) {
        if (g->logs[i].log_type == LOG_DRAW) last_draw_idx = i;
    }
    bool deck_alive_now = (g->deck_count > 0 || g->has_flipped);
    int flip_log_idx = (!deck_alive_now && !cl_no_flip) ? last_draw_idx : -1;

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
    uint32_t out_mask = 0;         // seats that have left, as of the scan position
    const uint64_t TRUMP = cl_trump_mask(g->power_suit);

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
                if (cl_hwc && p >= 0 && p != bot_idx && L->log_type == LOG_ATTACK
                    && (!B->not_hw[p] || cl_profile >= 2) && L->num_pairs > 0) {
                    uint64_t thrown = 0; int n_tr = 0;
                    for (int k = 0; k < L->num_pairs; k++) {
                        thrown |= cl_bit(L->pairs[k].primary);
                        if (L->pairs[k].primary.suit == g->power_suit) n_tr++;
                    }
                    uint64_t held0 = cl_pinned_mask(B, p) | thrown;
                    if (n_tr > 0 && n_tr < L->num_pairs) {
                        B->not_hw[p] = true; B->hw_viol[p]++;   // mixed trump/non-trump attack: never handwritten
                    } else if (first_attack) {
                        int defcap = (cur_def >= 0) ? hand_n[cur_def] : CARDS_PER_PLAYER;
                        if (n_tr > 0) {
                            // Trump first attack: handwritten only leads trump with
                            // NO non-trump card in hand.
                            HwCon *c = cl_con_new(B, p, CL_CON_FORBID, held0);
                            c->src = CL_SRC_ATK_TRUMP;
                            c->forbid = ~TRUMP & ((1ull << 52) - 1);
                            if (c->held & c->forbid) B->not_hw[p] = true;
                        } else {
                            int kk = L->num_pairs, v = L->pairs[0].primary.value;
                            uint64_t fb = 0;
                            if (kk < defcap) fb |= cl_value_mask(v) & ~TRUMP & ~thrown;   // all its v's
                            if (kk == 1) {
                                for (int u = min_value_for(g->num_players); u < v; u++)
                                    fb |= cl_value_mask(u) & ~TRUMP;                    // floor
                            }
                            if (fb) {
                                HwCon *c = cl_con_new(B, p, CL_CON_FORBID, held0);
                                c->src = CL_SRC_ATK_VALUE;
                                c->forbid = fb;
                                if (c->held & c->forbid) B->not_hw[p] = true;
                            }
                            if (!(kk == 1 && kk >= defcap)) {
                                HwCon *c = cl_con_new(B, p, CL_CON_ATTACK, held0);
                                c->src = CL_SRC_ATK_COUNT;
                                c->k = (uint8_t)kk; c->v = (uint8_t)v;
                                c->cap = (uint8_t)(defcap > 60 ? 60 : defcap);
                            }
                        }
                    } else if (cur_def >= 0) {
                        // Throw-in: handwritten adds EVERY non-trump card whose value
                        // is on the table (capped by defender capacity), lowest first.
                        int cap = hand_n[cur_def] - unc_n;
                        uint64_t tv = 0;
                        for (int q = 0; q < tbl_n; q++) tv |= cl_value_mask(tbl[q].value);
                        uint64_t fb = 0;
                        if (n_tr > 0) fb = tv & ~TRUMP;
                        else if (L->num_pairs < cap) fb = tv & ~TRUMP & ~thrown;
                        if (fb) {
                            HwCon *c = cl_con_new(B, p, CL_CON_FORBID, held0);
                            c->src = CL_SRC_THROWIN;
                            c->forbid = fb;
                            if (c->held & c->forbid) B->not_hw[p] = true;
                        }
                    }
                }
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
                        if (!cl_set_contains(B->pinned[p], B->pinned_n[p], c)
                            && cl_void_forbidden(B, g, p, c)) B->mc_tell[p] = true;
                        if (c.suit != g->power_suit
                            && (decl_vals[p] & (1u << c.value))) B->mc_tell[p] = true;
                        cl_floor_check(B, g, p, c);
                        cl_con_note_play(B, g, p, c);
                        cl_pinned_remove(B, p, c);
                    }
                }
                if (p >= 0 && p != bot_idx && L->log_type == LOG_ATTACK) {
                    if (any_trump && deck_alive_at) trump_viol[p]++;
                    if (first_attack && L->num_pairs == 1 && !any_trump
                        && in_now > 2 && !cl_no_floors) {
                        B->floor_v[p] = L->pairs[0].primary.value;
                    }
                }
                break;
            }
            case LOG_COVER:
                if (p >= 0) hand_n[p] -= L->num_pairs;
                if (cl_hwc && p >= 0 && p != bot_idx && (!B->not_hw[p] || cl_profile >= 2) && L->num_pairs > 0) {
                    uint64_t covers = 0;
                    for (int k = 0; k < L->num_pairs; k++) covers |= cl_bit(L->pairs[k].primary);
                    uint64_t held0 = cl_pinned_mask(B, p) | covers;
                    if (L->num_pairs < unc_n) {
                        B->not_hw[p] = true; B->hw_viol[p]++;   // partial cover: handwritten covers all or picks up
                    } else {
                        uint64_t fb = 0;
                        // Pass not taken: handwritten transfers whenever it legally can.
                        if (unc_n == tbl_n && tbl_n > 0 && game_pass_allowed(g)) {
                            int v0 = tbl[0].value; bool same = true;
                            for (int q = 1; q < tbl_n; q++) if (tbl[q].value != v0) { same = false; break; }
                            if (same) {
                                int nxt = p;
                                for (int step = 1; step < g->num_players; step++) {
                                    int cand = (p + step) % g->num_players;
                                    if (!(out_mask & (1u << cand))
                                        && g->players[cand].status != PLAYER_STATUS_IDLE) { nxt = cand; break; }
                                }
                                if (nxt != p && hand_n[nxt] >= 1 + tbl_n) fb |= cl_value_mask(v0);
                            }
                        }
                        // Greedy lowest-score covers: nothing cheaper that covers the
                        // same attack was in the hand (order-free form, CL20.md).
                        uint64_t fbg = 0;
                        for (int k = 0; k < L->num_pairs; k++) {
                            Card cc = L->pairs[k].primary, tg = L->pairs[k].target;
                            if (card_is_none(tg)) continue;
                            int sc = cc.value + (cc.suit == g->power_suit ? 1000 : 0);
                            fbg |= cl_covers_mask(tg, g->power_suit)
                                & cl_score_below_mask(sc, g->power_suit) & ~covers;
                        }
                        uint64_t fbp = fb;
                        fb |= fbg;
                        if (fb) {
                            HwCon *c = cl_con_new(B, p, CL_CON_FORBID, held0);
                            c->src = fbp ? CL_SRC_COVER_PASS : CL_SRC_COVER_GREEDY;
                            c->forbid = fb;
                            if (c->held & c->forbid) B->not_hw[p] = true;
                        }
                    }
                }
                for (int k = 0; k < L->num_pairs; k++) {
                    Card c = L->pairs[k].primary;
                    if (tbl_n < (int)(sizeof(tbl) / sizeof(tbl[0]))) tbl[tbl_n++] = c;
                    if (p >= 0 && p != bot_idx) {
                        if (deck_alive_at) {
                            B->cards_played[p]++;
                            if (c.suit == g->power_suit) B->trumps_played[p]++;
                        }
                        if (!cl_set_contains(B->pinned[p], B->pinned_n[p], c)
                            && cl_void_forbidden(B, g, p, c)) B->mc_tell[p] = true;
                        if (c.suit != g->power_suit
                            && (decl_vals[p] & (1u << c.value))) B->mc_tell[p] = true;
                        cl_floor_check(B, g, p, c);
                        cl_con_note_play(B, g, p, c);
                        cl_pinned_remove(B, p, c);
                    }
                    if (!card_is_none(L->pairs[k].target)) {
                        for (int q = 0; q < unc_n; q++) {
                            if (card_eq(unc[q], L->pairs[k].target)) {
                                // Stable removal: unc[] stays in table (battle) order,
                                // which the PICKUP greedy-cover constraint replays.
                                for (int r = q; r + 1 < unc_n; r++) unc[r] = unc[r + 1];
                                unc_n--;
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
                // Declined with capacity left: handwritten never declines a legal
                // non-trump throw-in, so it held no non-trump card of a table value.
                if (cl_hwc && p >= 0 && p != bot_idx && (!B->not_hw[p] || cl_profile >= 2) && cur_def >= 0
                    && p != cur_def && hand_n[cur_def] - unc_n >= 1 && tbl_n > 0) {
                    uint64_t tv = 0;
                    for (int q = 0; q < tbl_n; q++) tv |= cl_value_mask(tbl[q].value);
                    HwCon *c = cl_con_new(B, p, CL_CON_FORBID, cl_pinned_mask(B, p));
                    c->src = CL_SRC_GOOD;
                    c->forbid = tv & ~TRUMP;
                    if (c->held & c->forbid) B->not_hw[p] = true;
                }
                break;
            case LOG_DEFENDER_CHANGE:
                cur_def = L->defender_index;
                break;
            case LOG_PICKUP:
                if (cl_hwc && p >= 0 && p != bot_idx && (!B->not_hw[p] || cl_profile >= 2) && unc_n > 0) {
                    uint64_t held0 = cl_pinned_mask(B, p);
                    uint64_t fb = 0;
                    if (unc_n == tbl_n && game_pass_allowed(g)) {   // pass not taken
                        int v0 = tbl[0].value; bool same = true;
                        for (int q = 1; q < tbl_n; q++) if (tbl[q].value != v0) { same = false; break; }
                        if (same) {
                            int nxt = p;
                            for (int step = 1; step < g->num_players; step++) {
                                int cand = (p + step) % g->num_players;
                                if (!(out_mask & (1u << cand))
                                    && g->players[cand].status != PLAYER_STATUS_IDLE) { nxt = cand; break; }
                            }
                            if (nxt != p && hand_n[nxt] >= 1 + tbl_n) fb |= cl_value_mask(v0);
                        }
                    }
                    uint64_t fbp = fb;
                    if (unc_n == 1) fb |= cl_covers_mask(unc[0], g->power_suit);
                    if (fb) {
                        HwCon *c = cl_con_new(B, p, CL_CON_FORBID, held0);
                        c->src = fbp ? CL_SRC_PICKUP_PASS : CL_SRC_PICKUP_COVER;
                        c->forbid = fb;
                        if (c->held & c->forbid) B->not_hw[p] = true;
                    }
                    if (unc_n >= 2) {
                        HwCon *c = cl_con_new(B, p, CL_CON_PICKUP, held0);
                        c->src = CL_SRC_PICKUP_MULTI;
                        c->unc_n = (uint8_t)(unc_n > 8 ? 8 : unc_n);
                        for (int q = 0; q < c->unc_n; q++) c->unc[q] = (uint8_t)cl_cid(unc[q]);
                    }
                }
                if (p >= 0) { hand_n[p] += tbl_n; decl_vals[p] = 0; }
                if (p >= 0 && p != bot_idx) {
                    // Exactly one uncovered attack => defender held no cover.
                    if (unc_n == 1 && B->void_n[p] < CL_MAX_VOIDS) {
                        B->voids[p][B->void_n[p]++] = unc[0];
                    }
                    uint64_t gained = 0;
                    for (int k = 0; k < tbl_n; k++) { cl_pinned_add(B, p, tbl[k]); gained |= cl_bit(tbl[k]); }
                    cl_con_note_gain(B, p, gained);
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
                    cl_con_note_draw(B, p, L->num_pairs);
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
                            if (fc.value >= 0 && fc.suit >= 0) cl_pinned_add(B, p, fc);
                        }
                    }
                }
                break;
            case LOG_PLAYER_OUT:
                in_now--;
                if (p >= 0) out_mask |= 1u << p;
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
    if (cl_adapt) {
        for (int p = 0; p < g->num_players; p++) {
            if (!B->mc_tell[p]) continue;
            if (cl_mcvoid) {
                // Keep the voids, soften them (cl_void_forbidden). Voids of a
                // trump attack say nothing cheap was possible: drop those.
                int w = 0;
                for (int k = 0; k < B->void_n[p]; k++)
                    if (B->voids[p][k].suit != g->power_suit) B->voids[p][w++] = B->voids[p][k];
                B->void_n[p] = w;
                B->soft_void[p] = true;
            } else {
                B->void_n[p] = 0;
                B->distrust_void[p] = true;
            }
            B->floor_v[p] = 0;
            B->distrust_floor[p] = true;
        }
    }

    // Semtex weak-seat profile (fulminate's lever, conservatively gated): a
    // seat that burns trumps while the deck is alive at a rate no strong or
    // heuristic bot exhibits is rolled out with the LOOSE model instead of
    // handwritten. Evidence: deck-alive trump share ramped over [0.40, 0.60]
    // (needs >= 14 observed cards), plus repeated deck-alive trump attacks.
    if (cl_profile) {
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
            // CL_PROFILE=2: proven handwritten-consistency violations (dominated
            // covers, trump leads holding non-trumps, partial covers...). Strong
            // MC seats commit a few per game; random seats commit them constantly.
            if (cl_profile >= 2) {
                int v = B->hw_viol[p] - cl_profile_viol0;
                if (v > 0) conf += cl_profile_viol_w * v;
            }
            if (conf >= 0.70) {
                B->loose[p] = true;
                B->void_n[p] = 0;    // weak seats don't obey cover-if-you-can
                B->floor_v[p] = 0;   // ...or lowest-first attacks
            }
            if (cl_prof_stats_on && g->players[p].status == PLAYER_STATUS_IN) {
                int sk = g->players[p].strategy_key;   // audit only: which bot sits there
                if (sk >= 0 && sk < 64) { cl_prof_stat[sk][0]++; if (B->loose[p]) cl_prof_stat[sk][1]++; }
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
            if (!cl_set_contains(known, kn, c)) B->pool[B->n++] = c;
        }
    }

    // CL_HWC seat gating + feasibility. A seat with a proven strategic tell
    // (mc_tell), a proven non-handwritten play (not_hw) or a weak-seat profile
    // is not modeled as handwritten. If a seat's FORBID masks leave fewer
    // allowed pool cards than its unknown count, the constraint set is
    // contradictory under the handwritten hypothesis: drop it (a tell).
    for (int p = 0; p < g->num_players; p++) {
        B->hwc_on[p] = false;
        B->forbid_all[p] = 0;
        if (!cl_hwc || p == bot_idx || B->not_hw[p] || B->mc_tell[p]
            || (cl_profile && B->loose[p])
            || g->players[p].status != PLAYER_STATUS_IN) continue;
        bool any = false;
        int unknown = g->players[p].hand_count - B->pinned_n[p];
        for (int i = 0; i < B->ncons; i++) {
            HwCon *k = &B->cons[i];
            if (k->seat != p) continue;
            int slack = k->drawn - k->used;
            if (slack < 0) { B->not_hw[p] = true; break; }
            if (slack >= unknown) continue;                 // vacuous
            any = true;
            if (k->kind == CL_CON_FORBID && slack == 0) B->forbid_all[p] |= k->forbid;
        }
        if (B->not_hw[p] || !any) continue;
        int allowed = 0;
        for (int i = 0; i < B->n; i++)
            if (!(B->forbid_all[p] & cl_bit(B->pool[i]))) allowed++;
        if (allowed < unknown) { B->not_hw[p] = true; B->forbid_all[p] = 0; continue; }
        B->hwc_on[p] = true;
    }

    // Feasibility: if constraints leave fewer allowed pool cards than the
    // player's unknown count, relax floors first, then voids.
    for (int p = 0; p < g->num_players; p++) {
        if (B->void_n[p] == 0 && B->floor_v[p] == 0) continue;
        int unknown = g->players[p].hand_count - B->pinned_n[p];
        if (unknown <= 0) continue;
        int allowed = 0;
        for (int i = 0; i < B->n; i++) {
            if (!cl_void_forbidden(B, g, p, B->pool[i])
                && !cl_floor_forbidden(B, g, p, B->pool[i])) allowed++;
        }
        if (allowed < unknown && B->floor_v[p] > 0) {
            B->floor_v[p] = 0;
            allowed = 0;
            for (int i = 0; i < B->n; i++) {
                if (!cl_void_forbidden(B, g, p, B->pool[i])) allowed++;
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
static _Thread_local int cl_full_logs = 0;   // CL_FULL_LOGS=1: bp-style worlds

static int cl_sample_world(Game *g_out, const Game *g_in, int my_idx,
                           const Belief *B, uint32_t seed,
                           bool apply_voids, bool apply_floors, bool apply_hwc) {
    // CL_FULL_LOGS (research, bp-style worlds) needs the whole log array —
    // only possible when the world slot is full-size (WORLD_LOG_CAP == 0,
    // every native build); under short slots it degrades to the filter.
    if (cl_full_logs && WORLD_LOG_CAP == 0) {
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
    if (hn == 0) return 1;

    uint32_t s = seed ? seed : 0xCAFEu;
    for (int i = hn - 1; i > 0; i--) {
        s = cl_xorshift(s);
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

    if (apply_voids || apply_floors) {
        for (int si = 0; si < ns; si++) {
            int p = slots[si].player;
            bool use_v = apply_voids && B->void_n[p] > 0;
            bool use_f = apply_floors && B->floor_v[p] > 0;
            if (!use_v && !use_f) continue;
            Card c = g_out->players[p].hand[slots[si].slot];
            bool bad = (use_v && cl_void_forbidden(B, g_in, p, c))
                    || (use_f && cl_floor_forbidden(B, g_in, p, c));
            if (!bad) continue;
            for (int d = 0; d < deck_n; d++) {
                Card dc = g_out->deck[d];
                bool dc_bad = (use_v && cl_void_forbidden(B, g_in, p, dc))
                           || (use_f && cl_floor_forbidden(B, g_in, p, dc));
                if (!dc_bad) {
                    g_out->deck[d] = c;
                    g_out->players[p].hand[slots[si].slot] = dc;
                    break;
                }
            }
        }
    }
    if (!apply_hwc) return 1;

    // CL_HWC repair: a slot holding a card its seat's FORBID masks exclude is
    // swapped with a deck card the seat may hold, else with another seat's
    // unknown slot when the swap is legal for both.
    bool any_hwc = false;
    for (int p = 0; p < g_in->num_players; p++) if (B->hwc_on[p]) any_hwc = true;
    if (!any_hwc) return 1;
    for (int si = 0; si < ns; si++) {
        int p = slots[si].player;
        if (!B->hwc_on[p] || !B->forbid_all[p]) continue;
        Card c = g_out->players[p].hand[slots[si].slot];
        if (!(B->forbid_all[p] & cl_bit(c))) continue;
        bool fixed = false;
        for (int d = 0; d < deck_n && !fixed; d++) {
            Card dc = g_out->deck[d];
            if (B->forbid_all[p] & cl_bit(dc)) continue;
            g_out->deck[d] = c;
            g_out->players[p].hand[slots[si].slot] = dc;
            fixed = true;
        }
        for (int sj = 0; sj < ns && !fixed; sj++) {
            int q = slots[sj].player;
            if (q == p) continue;
            Card qc = g_out->players[q].hand[slots[sj].slot];
            if (B->forbid_all[p] & cl_bit(qc)) continue;
            if (B->hwc_on[q] && (B->forbid_all[q] & cl_bit(c))) continue;
            g_out->players[q].hand[slots[sj].slot] = c;
            g_out->players[p].hand[slots[si].slot] = qc;
            fixed = true;
        }
    }
    // Count constraints (a FORBID that survived draws): at most `slack` of the
    // seat's unknown cards may be forbidden. Repair the excess by swapping
    // forbidden slots for deck cards (or another seat's slot) that satisfy
    // every hard mask involved.
    for (int i = 0; i < B->ncons; i++) {
        const HwCon *k = &B->cons[i];
        if (k->kind != CL_CON_FORBID || !B->hwc_on[k->seat]) continue;
        int slack = k->drawn - k->used;
        if (slack <= 0) continue;                 // hard: handled above
        int p = k->seat;
        int unknown = g_in->players[p].hand_count - B->pinned_n[p];
        if (slack >= unknown) continue;
        int cnt = 0;
        for (int si = 0; si < ns; si++)
            if (slots[si].player == p && (k->forbid & cl_bit(g_out->players[p].hand[slots[si].slot]))) cnt++;
        for (int si = 0; si < ns && cnt > slack; si++) {
            if (slots[si].player != p) continue;
            Card c = g_out->players[p].hand[slots[si].slot];
            if (!(k->forbid & cl_bit(c))) continue;
            uint64_t bad = k->forbid | B->forbid_all[p];
            bool fixed = false;
            for (int d = 0; d < deck_n && !fixed; d++) {
                Card dc = g_out->deck[d];
                if (bad & cl_bit(dc)) continue;
                g_out->deck[d] = c; g_out->players[p].hand[slots[si].slot] = dc; fixed = true;
            }
            for (int sj = 0; sj < ns && !fixed; sj++) {
                int q = slots[sj].player;
                if (q == p) continue;
                Card qc = g_out->players[q].hand[slots[sj].slot];
                if (bad & cl_bit(qc)) continue;
                if (B->hwc_on[q] && (B->forbid_all[q] & cl_bit(c))) continue;
                g_out->players[q].hand[slots[sj].slot] = c;
                g_out->players[p].hand[slots[si].slot] = qc; fixed = true;
            }
            if (fixed) cnt--;
        }
        if (cnt > slack) return 0;
    }
    // Set-level constraints (attack choice, multi-card pickup) are checked on
    // the repaired hand; the caller resamples on failure.
    for (int i = 0; i < B->ncons; i++) {
        const HwCon *k = &B->cons[i];
        if (k->kind == CL_CON_FORBID || !B->hwc_on[k->seat]) continue;
        int p = k->seat;
        uint64_t h = k->held;
        for (int si = 0; si < ns; si++)
            if (slots[si].player == p) h |= cl_bit(g_out->players[p].hand[slots[si].slot]);
        bool ok = (k->kind == CL_CON_ATTACK)
                ? cl_con_attack_ok(k, h, g_in->power_suit, min_value_for(g_in->num_players))
                : cl_con_pickup_ok(k, h, g_in->power_suit);
        if (!ok) return 0;
    }
    return 1;
}

// ---------- exact solver (shared by root + rollout leaves) -----------------

#define CL_SOLVE_MAX_DEPTH   48
#define CL_SOLVE_MAX_MOVES   96
#define CL_SOLVE_BUDGET      200000L
#define CL_AVOID_BUDGET      150000L
#define CL_SOLVE_MAX_CARDS   20
#define CL_LEAF_BUDGET       1500L

typedef struct {
    long budget;
    bool aborted;
    int  me;
    SolveMoves *mv;      // [CL_SOLVE_MAX_DEPTH]
} Solver;

static _Thread_local SolveMoves *cl_solver_mv = NULL;

_Static_assert(CL_SOLVE_MAX_DEPTH <= SOLVE_SCRATCH_DEPTH,
               "shared solver scratch shallower than this family's depth");
static bool cl_solver_ready(void) {
    if (!cl_solver_mv) {
        cl_solver_mv = solve_scratch_mv();
    }
    return cl_solver_mv != NULL;
}

// Value in [-1000, 1000] from `me`'s perspective: positive = me escaping,
// negative = me as durak. Magnitude prefers faster wins / slower losses.
static int cl_solve(Solver *S, const Game *g, int alpha, int beta, int depth) {
    int loser = game_done(g);
    if (loser >= 0) return (loser == S->me) ? -(1000 - depth) : (1000 - depth);
    if (cl_in_count(g) == 0) return 0;   // defensive: simultaneous out
    if (depth >= CL_SOLVE_MAX_DEPTH) { S->aborted = true; return 0; }
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
    if (mv->n > CL_SOLVE_MAX_MOVES) { S->aborted = true; return 0; }

    bool maximizing = (actor == S->me);
    int best = maximizing ? -2000 : 2000;
    for (int i = 0; i < mv->n; i++) {
        Game *child = solve_scratch_child(depth);
        solve_clone_prefix(child, g);
        if (!cl_apply(child, actor, &mv->moves[i])) continue;
        int v = cl_solve(S, child, alpha, beta, depth + 1);
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
static _Thread_local long cl_leaf_budget = CL_LEAF_BUDGET;
static _Thread_local int  cl_leaf_max_cards = 0;   // set from env at init
static _Thread_local int  cl_floor_mod = 2;        // floors in 1/mod worlds

static int cl_leaf_solve(const Game *g) {
    if (!cl_solver_ready()) return -1;
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
    S.budget  = cl_leaf_budget;
    S.aborted = false;
    S.me      = me;
    S.mv      = cl_solver_mv;

    // Null window around 0: only the sign matters (true values are ±(1000-d)
    // for decided games), and the narrow window maximizes pruning.
    int v = cl_solve(&S, root, -1, 1, 0);
    if (S.aborted || v == 0) return -1;
    return (v > 0) ? opp : me;
}

// ---------- simulation ---------------------------------------------------

// Stage-aware rollout policy (gunpowder's rule): handwritten while the deck
// is alive or the game is heads-up, espresso for multi-player endgames.
static StrategyFn cl_rollout_for(const Game *g) {
    // CL_ROLLOUT (struct path only): 1 = espresso everywhere, 2 = handwritten
    // everywhere. Research knob for the "rollout-policy bias" hypothesis — vs a
    // strong opponent, a weak (handwritten) rollout policy biases value
    // estimates, so more worlds saturates. A stronger rollout policy may reduce
    // that bias. Run with CL_NO_FASTROLL=1.
    if (cl_rollout_policy == 1) return espresso_strategy_choose;
    if (cl_rollout_policy == 2) return handwritten_strategy_choose;
    bool deck_active = (g->deck_count > 0 || g->has_flipped);
    if (deck_active || cl_in_count(g) == 2) return handwritten_strategy_choose;
    return espresso_strategy_choose;
}

// Roll a sampled world forward; returns my finish position (1..N), or 0 if
// the simulation didn't terminate. Exits early once my position is known,
// and resolves small 2-player deck-empty endgames exactly (one attempt per
// rollout — a failed solve falls back to policy play for good).
static int cl_simulate(Game *g, int my_idx, int max_turns) {
    int turns = 0;
    bool leaf_tried = false;
    while (game_done(g) < 0 && turns++ < max_turns) {
        // My fate is sealed as soon as I'm out: position = elimination slot.
        if (!cl_no_earlyexit
            && g->players[my_idx].status != PLAYER_STATUS_IN) {
            for (int i = 0; i < g->num_eliminated; i++) {
                if (g->elimination_order[i] == my_idx) return i + 1;
            }
            break;   // not IN and not eliminated: corrupt state, bail
        }

        if (!cl_no_leaf && !leaf_tried && g->deck_count == 0 && !g->has_flipped
            && cl_in_count(g) == 2) {
            int total = 0;
            for (int i = 0; i < g->num_players; i++) {
                if (g->players[i].status == PLAYER_STATUS_IN)
                    total += g->players[i].hand_count;
            }
            if (total <= cl_leaf_max_cards) {
                leaf_tried = true;
                int loser = cl_leaf_solve(g);
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
            StrategyFn fn = cl_rollout_for(g);
            int idx = fn(g, pi, moves, NULL);
            if (idx < 0 || idx >= moves->n) continue;
            if (cl_apply(g, pi, &moves->moves[idx])) { acted = true; break; }
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
// and play it out on bitmasks. ~10x faster per ply than cl_simulate. The
// effective rollout policy is always handwritten (see cordite_sim.c), and the
// exact leaf solver (CL_LEAF, off by default) is not used in the fast path.
static int cl_simulate_fast(const Game *g, int my_idx, int max_turns) {
    SimState s;
    cd_sim_from_game(&s, g);
    if (cl_bbleaf_on || cl_polmap)
        return cd_sim_playout_pol(&s, my_idx, max_turns, !cl_no_earlyexit,
                                  cl_bbleaf_on ? cl_bbleaf_cards_eff : 0,
                                  cl_bbleaf_budget, cl_polmap);
    return cd_sim_playout(&s, my_idx, max_turns, !cl_no_earlyexit);
}

// Dispatcher: bitboard rollout by default; struct rollout under CL_NO_FASTROLL
// or CL_LEAF (the leaf solver lives on the struct path). CL_DIFFTEST runs both
// and tallies divergences (printed at process exit by the eval harness if it
// hooks cl_difftest_report, else just counted).
static _Thread_local long cl_diff_total = 0, cl_diff_mismatch = 0;
static int cl_rollout(Game *g, int my_idx, int max_turns) {
    if (cl_no_fastroll || !cl_no_leaf) return cl_simulate(g, my_idx, max_turns);
    if (cl_difftest) {
        uint32_t rng0 = game_rng_get();
        int fast = cl_simulate_fast(g, my_idx, max_turns);
        game_rng_set(rng0);
        Game *slow_g = rollout_scratch_diff();
        cl_lite_clone(slow_g, g);
        int slow = cl_simulate(slow_g, my_idx, max_turns);
        cl_diff_total++;
        if (fast != slow) {
            cl_diff_mismatch++;
            if (cl_diff_mismatch <= 20) {
                fprintf(stderr, "CL_DIFFTEST mismatch #%ld: fast=%d slow=%d "
                        "(np=%d deck=%d logs=%d)\n", cl_diff_mismatch, fast, slow,
                        g->num_players, g->deck_count, g->num_logs);
            }
        }
        return slow;  // keep slow behavior while difftesting
    }
    return cl_simulate_fast(g, my_idx, max_turns);
}

void cl_difftest_report(void) {
    if (cl_diff_total > 0) {
        fprintf(stderr, "CL_DIFFTEST: %ld/%ld rollouts diverged (%.3f%%)\n",
                cl_diff_mismatch, cl_diff_total,
                100.0 * (double)cl_diff_mismatch / (double)cl_diff_total);
    }
}

// ---------- CL_EXPLAIN: deliberation dump (analysis build only) -------------
//
// COMPILED OUT of every normal build: the entire deliberation-dump machinery
// (this block, the solver-verdict probe, cl_ex_emit, and the emit hooks in
// cl20_choose_impl) lives behind CL_EXPLAIN_BUILD, so the shipped native +
// wasm bots carry ZERO of it — no code-size cost. It is enabled only by the
// `make cl_explain` analysis tool (c/tools/cl_explain), which defines
// -DOG_EXPLAIN_BUILD.
//
// When built, set CL_EXPLAIN=1 (dump to stderr) or CL_EXPLAIN=/path/to/file to
// emit one JSON-lines record per deciding-seat decision: the seat's hand, the
// table, the candidate moves with their MC average-finish scores
// (score[i]/nsim[i]) OR the exact endgame-solver verdict per move (win/loss/
// draw/unknown), and the chosen move. Nothing runs when the var is unset.


// ---------- root endgame solve (win take + loss avoid) ---------------------

// Solve every root move with a full window when 2 players remain and the
// deck is empty (the unseen pool IS the opponent's hand — public deduction).
// Returns the fastest forced-win index, or -1. When no win exists, sets
// forced_loss[i] for root moves that lose under optimal play; the MC stage
// avoids them whenever at least one non-losing move exists.
static int cl_try_endgame_solve(const Game *g, int bot_idx,
                                const LegalMoves *moves, const Belief *B,
                                bool *forced_loss, int *n_safe) {
    *n_safe = moves->n;
    if (g->deck_count > 0 || g->has_flipped) return -1;
    if (cl_in_count(g) != 2) return -1;
    if (g->players[bot_idx].status != PLAYER_STATUS_IN) return -1;

    int opp = -1;
    for (int i = 0; i < g->num_players; i++) {
        if (i != bot_idx && g->players[i].status == PLAYER_STATUS_IN) opp = i;
    }
    if (opp < 0) return -1;

    int unknown = g->players[opp].hand_count - B->pinned_n[opp];
    if (unknown < 0 || unknown != B->n) return -1;  // deduction failed; bail

    int total = g->players[bot_idx].hand_count + g->players[opp].hand_count;
    if (total > cl_solve_cards) return -1;

    if (!cl_solver_ready()) return -1;

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
    // CL_NO_BBSOLVE=1 falls back to the struct solver for A/B.
    bool bbsolve = !cl_flag("CL_NO_BBSOLVE");
    SimState root_sim;
    if (bbsolve) cd_sim_from_game(&root_sim, root);
    cd_sim_solve_reset();

    // CL_EXPLAIN: record an EXACT full-window verdict for every root move
    // (win/loss/draw/unknown), for the deliberation dump. This is a pure probe
    // — it runs its own budget, then resets the transposition table so the real
    // pruned win-hunt below starts cold exactly as it would with the var unset.
    // Nothing here executes when CL_EXPLAIN is not set.

    Solver S;
    S.budget  = CL_SOLVE_BUDGET;
    S.aborted = false;
    S.me      = bot_idx;
    S.mv      = cl_solver_mv;
    long win_budget   = bbsolve ? (long)cl_bb_win_budget   : CL_SOLVE_BUDGET;
    long avoid_budget = bbsolve ? (long)cl_bb_avoid_budget : CL_AVOID_BUDGET;
    long budget = win_budget;

    // Pass 1 — win hunt (blackpowder's loop): fail-soft with an accumulating
    // alpha floor at 0, so losing subtrees prune immediately.
    // CL_BP_SOLVE=1 reverts to blackpowder's exact semantics (alpha starts
    // wide open, any abort bails the whole solve) for A/B testing.
    int best_idx = -1;
    int best_v = 0;       // only accept strictly winning lines
    int alpha = cl_flag("CL_BP_SOLVE") ? -2000 : 0;
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
            // Slot 0 is free here: cl_solve starts the recursion at depth 1.
            Game *child = solve_scratch_child(0);
            solve_clone_prefix(child, root);
            if (!cl_apply(child, bot_idx, &moves->moves[i])) continue;
            S.aborted = false;
            v = cl_solve(&S, child, alpha, 2000, 1);
            aborted_i = S.aborted;
            if (S.budget <= 0) return -1;
        }
        if (aborted_i) { if (bail_on_abort) return -1; any_abort = true; continue; }
        if (v > best_v) { best_v = v; best_idx = i; }
        if (v > alpha) alpha = v;
    }
    if (best_idx >= 0) return best_idx;
    if (cl_no_avoid || any_abort) return -1;
    if (total > cl_avoid_cards) return -1;   // extended window: win hunt only

    // Pass 2 — loss avoidance: no win exists, so classify each move with a
    // null window around 0 (sign only, maximal pruning).
    S.budget = CL_AVOID_BUDGET;
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
            if (!cl_apply(child, bot_idx, &moves->moves[i])) continue;
            S.aborted = false;
            v = cl_solve(&S, child, -1, 0, 1);
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

#define CL_MAX_CANDS 26

typedef struct {
    int idx[CL_MAX_CANDS];
    int n;
} Candidates;


static void cl_ranked_insert(int *idxs, double *keys, int *n, int cap,
                             int idx, double key) {
    int pos = *n;
    while (pos > 0 && keys[pos - 1] > key) pos--;
    if (pos >= cap) return;
    int last = (*n < cap) ? *n : cap - 1;
    for (int i = last; i > pos; i--) { idxs[i] = idxs[i - 1]; keys[i] = keys[i - 1]; }
    idxs[pos] = idx; keys[pos] = key;
    if (*n < cap) (*n)++;
}

static void cl_pick_candidates(const Game *g, const LegalMoves *moves,
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
                for (int j = 0; j < m->n_cards; j++) sum += cl_card_score(m->cards[j], power);
                cl_ranked_insert(atk, atk_k, &n_atk, 12, i,
                                 -(double)m->n_cards * 10000.0 + (double)sum);
                break;
            }
            case MOVE_COVER: {
                double prod = 1.0;
                for (int j = 0; j < m->n_cards; j++) prod *= (double)cl_card_score(m->cards[j], power);
                cl_ranked_insert(cov, cov_k, &n_cov, 10, i,
                                 prod - (double)m->n_cards * 0.5);
                break;
            }
            case MOVE_PASS: {
                int sum = 0;
                for (int j = 0; j < m->n_cards; j++) sum += cl_card_score(m->cards[j], power);
                cl_ranked_insert(pas, pas_k, &n_pas, 3, i, (double)sum);
                break;
            }
            case MOVE_GOOD:   good_idx = i;   break;
            case MOVE_PICKUP: pickup_idx = i; break;
            default: break;
        }
    }

    out->n = 0;
    for (int i = 0; i < n_atk && out->n < CL_MAX_CANDS; i++) out->idx[out->n++] = atk[i];
    for (int i = 0; i < n_cov && out->n < CL_MAX_CANDS; i++) out->idx[out->n++] = cov[i];
    for (int i = 0; i < n_pas && out->n < CL_MAX_CANDS; i++) out->idx[out->n++] = pas[i];
    if (good_idx >= 0 && out->n < CL_MAX_CANDS)   out->idx[out->n++] = good_idx;
    if (pickup_idx >= 0 && out->n < CL_MAX_CANDS) out->idx[out->n++] = pickup_idx;
}

// ---------- main MC ---------------------------------------------------------

// Worlds per decision (W1: all candidates, W2: surviving third, W3: final
// top-2 duel). Compact worlds + early rollout exits buy ~2-3x blackpowder's
// sampling budget at comparable wall-clock.
static void cl_params(int num_players, int *W1, int *W2, int *W3) {
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
    if (cl_old_budget) {
        if (num_players <= 2)      { *W1 = 16; *W2 = 28; *W3 = 28; }
        else if (num_players <= 4) { *W1 = 14; *W2 = 28; *W3 = 28; }
        else if (num_players <= 6) { *W1 = 20; *W2 = 40; *W3 = 28; }
        else                       { *W1 = 20; *W2 = 40; *W3 = 24; }
    }
    if (cl_oracle) { *W1 *= 6; *W2 *= 6; *W3 *= 6; }
    else if (num_players >= 6 && cl_wmul6 > 1) { *W1 *= cl_wmul6; *W2 *= cl_wmul6; *W3 *= cl_wmul6; }
    if (cl_w1_override > 0) *W1 = cl_w1_override;
    if (cl_w2_override > 0) *W2 = cl_w2_override;
    if (cl_w3_override >= 0) *W3 = cl_w3_override;
}

// CL_VERIFY=1: oracle self-check (test-only — reads real hands to validate
// the public-info belief, never to play).
static _Thread_local long cl_verify_cons = 0, cl_verify_viol = 0, cl_verify_seats = 0;
void cl_verify_report(void) {
    fprintf(stderr, "[cl20 verify] constrained seat-decisions=%ld constraints=%ld violations=%ld\n",
            cl_verify_seats, cl_verify_cons, cl_verify_viol);
}
static void cl_verify_belief(const Game *g, int bot_idx, const Belief *B) {
    // CL_HWC audit: the TRUE hand (read here only, for verification) must
    // satisfy every live constraint of a handwritten-family seat.
    for (int p = 0; p < g->num_players; p++) {
        if (p == bot_idx || !B->hwc_on[p]) continue;
        const Player *pl = &g->players[p];
        uint64_t hand = 0;
        for (int j = 0; j < pl->hand_count; j++) hand |= cl_bit(pl->hand[j]);
        uint64_t unknown = hand & ~cl_pinned_mask(B, p);
        cl_verify_seats++;
        for (int i = 0; i < B->ncons; i++) {
            const HwCon *k = &B->cons[i];
            if (k->seat != p) continue;
            cl_verify_cons++;
            uint64_t ht = unknown | k->held;
            bool ok = true;
            if (k->kind == CL_CON_FORBID) ok = __builtin_popcountll(unknown & k->forbid) <= k->drawn - k->used;
            else if (k->kind == CL_CON_ATTACK) ok = cl_con_attack_ok(k, ht, g->power_suit, min_value_for(g->num_players));
            else ok = cl_con_pickup_ok(k, ht, g->power_suit);
            if (!ok) {
                cl_verify_viol++;
                fprintf(stderr, "CL_VERIFY: hwc kind=%d violated by p%d (strat %d) logs=%d k=%d v=%d cap=%d unc_n=%d\n",
                        k->kind, p, g->players[p].strategy_key, g->num_logs, k->k, k->v, k->cap, k->unc_n);
            }
        }
    }
    for (int p = 0; p < g->num_players; p++) {
        if (p == bot_idx) continue;
        const Player *pl = &g->players[p];
        for (int k = 0; k < B->pinned_n[p]; k++) {
            bool found = false;
            for (int j = 0; j < pl->hand_count; j++) {
                if (card_eq(pl->hand[j], B->pinned[p][k])) { found = true; break; }
            }
            if (!found) {
                fprintf(stderr, "CL_VERIFY: pinned card v%d s%d NOT in p%d hand (logs=%d)\n",
                        B->pinned[p][k].value, B->pinned[p][k].suit, p, g->num_logs);
            }
        }
        for (int j = 0; j < pl->hand_count; j++) {
            if (cl_set_contains(B->pinned[p], B->pinned_n[p], pl->hand[j])) continue;
            if (B->void_n[p] > 0 && cl_void_forbidden(B, g, p, pl->hand[j])) {
                fprintf(stderr, "CL_VERIFY: void violated: p%d holds v%d s%d\n",
                        p, pl->hand[j].value, pl->hand[j].suit);
            }
            if (cl_floor_forbidden(B, g, p, pl->hand[j])) {
                fprintf(stderr, "CL_VERIFY: floor violated: p%d holds v%d s%d floor=%d\n",
                        p, pl->hand[j].value, pl->hand[j].suit, B->floor_v[p]);
            }
            if (!cl_set_contains(B->pool, B->n, pl->hand[j])) {
                fprintf(stderr, "CL_VERIFY: p%d unknown card v%d s%d missing from pool (logs=%d)\n",
                        p, pl->hand[j].value, pl->hand[j].suit, g->num_logs);
            }
        }
    }
}

// ---------- CL_EXPLAIN emit (defined here: needs Candidates + MOVE_*) -------

// Exact sign-only solve of a determinized world with a live deck (CL_XDECK).
// Returns 1 with *fp = my finish (1 = I escape, 2 = I am the durak) when the
// solve resolved; 0 (fall back to the rollout) on abort or a null value.
static int cl_xdeck_eval(SimState *trial, int me, uint32_t wseed, int *fp) {
    cd_sim_set_livedeck(1, (uint64_t)wseed);
    long budget = cl_xdeck_budget;
    int aborted = 0;
    int v = cd_sim_solve_d(trial, me, -1, 1, &budget, 1, &aborted);
    cd_sim_set_livedeck(0, 0);
    cl_xd_stat_solves++;
    if (aborted || v == 0) { cl_xd_stat_abort++; return 0; }
    *fp = (v > 0) ? 1 : 2;
    return 1;
}

static int cl20_choose_impl(const Game *g, int bot_idx,
                            const LegalMoves *moves, void *ctx) {
    (void)ctx;
    if (moves->n == 0) return -1;
    if (moves->n == 1) {
        return 0;
    }

    if (!cl_flags_loaded) {
        cl_no_solve  = cl_flag("CL_NO_SOLVE");
        cl_no_voids  = cl_flag("CL_NO_VOIDS");
        cl_no_flip   = cl_flag("CL_NO_FLIP");
        cl_no_floors = cl_flag("CL_NO_FLOORS");
        // Exact leaf endgames in rollouts are OFF by default: measured both
        // slower and weaker vs the real (imperfect) opponent pool — modeling
        // actual opponents beats assuming perfect play. CL_LEAF=1 re-enables.
        cl_no_leaf   = !cl_flag("CL_LEAF");
        cl_no_avoid  = cl_flag("CL_NO_AVOID");
        cl_verify    = cl_flag("CL_VERIFY");
        if (cl_verify) { void cl_verify_report(void); atexit(cl_verify_report); }
        cl_w1_override = cl_env_int("CL_W1", 0);
        cl_w2_override = cl_env_int("CL_W2", 0);
        cl_w3_override = cl_env_int("CL_W3", -1);
        cl_keep1 = cl_env_int("CL_KEEP1", 0);
        cl_keep2 = cl_env_int("CL_KEEP2", 0);
        cl_rollout_policy = cl_env_int("CL_ROLLOUT", 0);
        cl_trump_keep = cl_env_int("CL_TRUMP_KEEP", 40) / 1000.0;
        cl_bb_win_budget = cl_env_int("CL_BB_WIN", 400000);
        cl_solve_cards = cl_env_int("CL_SOLVE_CARDS", 28);
        cl_avoid_cards = cl_env_int("CL_AVOID_CARDS", 24);
        cl_bb_avoid_budget = cl_env_int("CL_BB_AVOID", 250000);
        cl_leaf_budget = cl_env_int("CL_LEAF_BUDGET", 1500);
        cl_leaf_max_cards = cl_env_int("CL_LEAF_CARDS", 10);
        cl_floor_mod = cl_env_int("CL_FLOOR_MOD", 2);
        if (cl_floor_mod < 1) cl_floor_mod = 1;
        cl_full_logs = cl_flag("CL_FULL_LOGS");
        cl_no_earlyexit = cl_flag("CL_NO_EARLYEXIT");
        cl_no_fastroll = cl_flag("CL_NO_FASTROLL");
        cl_bbleaf = cl_env_int("CL_BBLEAF", 2);
        cl_adapt = cl_env_int("CL_ADAPT", 1);
        cl_reply = cl_env_int("CL_REPLY", 0);
        cl_mcdef = cl_env_int("CL_MCDEF", 0);
        cl_reply_cap = cl_env_int("CL_REPLY_CAP", 6);
        cl_reply_stage = cl_env_int("CL_REPLY_STAGE", 2);
        cl_void_mod = cl_env_int("CL_VOID_MOD", 4);
        if (cl_void_mod < 2) cl_void_mod = 2;
        cl_profile = cl_env_int("CL_PROFILE", 0);
        cl_profile_viol0 = cl_env_int("CL_PROFILE_V0", 2);
        cl_profile_viol_w = cl_env_int("CL_PROFILE_VW", 25) / 100.0;
        cl_prof_stats_on = cl_flag("CL_PROFILE_STATS");
        if (cl_prof_stats_on) { void cl_prof_stats_report(void); atexit(cl_prof_stats_report); }
        cl_hwc = cl_env_int("CL_HWC", 1);
        cl_selfpol = cl_env_int("CL_SELFPOL", 0);
        cl_mcvoid = cl_env_int("CL_MCVOID", 0);
        cl_mcpol = cl_env_int("CL_MCPOL", 0);
        cl_wmul6 = cl_env_int("CL_WMUL6", 1);
        if (cl_wmul6 < 1) cl_wmul6 = 1;
        cl_xdeck = cl_env_int("CL_XDECK", 0);
        cl_xdeck_cards = cl_env_int("CL_XDECK_CARDS", 4);
        cl_xdeck_total = cl_env_int("CL_XDECK_TOTAL", 20);
        cl_xdeck_budget = cl_env_int("CL_XDECK_BUDGET", 40000);
        cl_xdeck_worlds = cl_env_int("CL_XDECK_WORLDS", 0);
        if (cl_flag("CL_XDECK_STATS")) { void cl_xdeck_stats_report(void); atexit(cl_xdeck_stats_report); }
        cd_sim_set_mc_model(cl_env_int("CL_MC_PICKT", 30), cl_env_int("CL_MC_PICKN", 15),
                            cl_env_int("CL_MC_NOPASS", 11), cl_env_int("CL_MC_FIRST", 50),
                            cl_env_int("CL_MC_GOOD", 7));
        cl_adapt_k = cl_env_int("CL_ADAPT_K", 5);
        cl_race = cl_env_int("CL_RACE", 0);
        cl_race_mult = cl_env_int("CL_RACE_MULT", 3);
        cl_race_c = cl_env_int("CL_RACE_C", 200) / 100.0;
        cl_race_cf = cl_env_int("CL_RACE_CF", 200) / 100.0;
        cl_race_block = cl_env_int("CL_RACE_BLOCK", 0);
        cl_race_min_blocks = cl_env_int("CL_RACE_MINB", 2);
        cl_self = cl_env_int("CL_SELF", 0);
        cl_self_cap = cl_env_int("CL_SELF_CAP", 6);
        cl_self_stage = cl_env_int("CL_SELF_STAGE", 2);
        cl_adapt_stage = cl_env_int("CL_ADAPT_STAGE", 2);
        cl_adapt_c = cl_env_int("CL_ADAPT_C", 200) / 100.0;
        cl_hwc_mod = cl_env_int("CL_HWC_MOD", 4);
        if (cl_hwc_mod < 2) cl_hwc_mod = 2;
        cl_hwc_tries = cl_env_int("CL_HWC_TRIES", 12);
        if (cl_flag("CL_HWC_STATS")) { void cl_hwc_stats_report(void); atexit(cl_hwc_stats_report); }
        cl_bbleaf_cards = cl_env_int("CL_BBLEAF_CARDS", 0);
        cl_bbleaf_cards2 = cl_env_int("CL_BBLEAF_CARDS2", 8);
        cl_bbleaf_budget = cl_env_int("CL_BBLEAF_BUDGET", 3000);
        cl_difftest = cl_flag("CL_DIFFTEST");
        if (cl_difftest) { void cl_difftest_report(void); atexit(cl_difftest_report); }
        cl_flags_loaded = 1;
    }

    uint32_t saved_rng = game_rng_get();

    Belief B;
    cl_build_belief(g, bot_idx, &B);
    if (cl_no_voids) for (int p = 0; p < MAX_PLAYERS; p++) B.void_n[p] = 0;
    if (cl_verify) cl_verify_belief(g, bot_idx, &B);

    // Player-count gate for the leaf lever (see cl_bbleaf comment). In the
    // pc-aware default mode heads-up uses the small-leaf threshold.
    cl_bbleaf_on = (cl_bbleaf != 0);
    cl_bbleaf_cards_eff = (cl_bbleaf == 2 && g->num_players == 2)
                        ? cl_bbleaf_cards2 : cl_bbleaf_cards;

    // Per-seat rollout policies: profiled-weak seats get the LOOSE model;
    // proven-strategic seats (mc_tell) get the MC-defender model.
    cl_polmap = NULL;
    {
        bool any = false;
        for (int p = 0; p < g->num_players; p++) {
            cl_polmap_buf[p] = CD_POL_HW;
            if (p == bot_idx && cl_selfpol) { cl_polmap_buf[p] = (uint8_t)cl_selfpol; any = true; continue; }
            if (cl_profile && B.loose[p]) { cl_polmap_buf[p] = CD_POL_LOOSE; any = true; }
            else if (cl_mcpol && B.mc_tell[p] && p != bot_idx) {
                cl_polmap_buf[p] = CD_POL_MC; any = true;
            }
            else if (cl_mcdef && B.mc_tell[p] && p != bot_idx) {
                cl_polmap_buf[p] = CD_POL_MCDEF; any = true;
            }
        }
        if (any) cl_polmap = cl_polmap_buf;
    }

    // Exact endgame: take a proven win; mark proven losses for exclusion.
    bool *forced_loss = forced_loss_scratch();
    memset(forced_loss, 0, (size_t)moves->n * sizeof(bool));
    int n_safe = moves->n;
    int solved = cl_no_solve ? -1
               : cl_try_endgame_solve(g, bot_idx, moves, &B, forced_loss, &n_safe);
    if (solved >= 0) {
        game_rng_set(saved_rng);
        return solved;
    }

    Candidates C;
    cl_pick_candidates(g, moves, forced_loss, &C);
    if (C.n == 0) {
        // Everything we'd consider is a proven loss; fall back to all moves.
        memset(forced_loss, 0, (size_t)moves->n * sizeof(bool));
        cl_pick_candidates(g, moves, forced_loss, &C);
    }
    if (C.n == 0) {
        game_rng_set(saved_rng); return 0;
    }
    if (C.n == 1) {
        game_rng_set(saved_rng); return C.idx[0];
    }

    int W1, W2, W3;
    cl_params(g->num_players, &W1, &W2, &W3);
    bool xdeck_on = false;
    if (cl_xdeck && cl_in_count(g) == 2 && (g->deck_count > 0 || g->has_flipped)
        && g->deck_count + (g->has_flipped ? 1 : 0) <= cl_xdeck_cards) {
        int total = g->deck_count + (g->has_flipped ? 1 : 0);
        for (int p = 0; p < g->num_players; p++)
            if (g->players[p].status == PLAYER_STATUS_IN) total += g->players[p].hand_count;
        for (int i = 0; i < g->num_battles; i++)
            total += 1 + (card_is_none(g->table_battles[i].defense) ? 0 : 1);
        if (total <= cl_xdeck_total) xdeck_on = true;
    }
    if (xdeck_on && cl_xdeck_worlds > 0) { W1 = cl_xdeck_worlds; W2 = 0; W3 = 0; }

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
    uint32_t base = cl_mix((uint32_t)bot_idx * 2654435761u, random_strategy_rng_get());

    double score[CL_MAX_CANDS] = {0};
    int    nsim [CL_MAX_CANDS] = {0};
    bool   alive[CL_MAX_CANDS];
    for (int i = 0; i < C.n; i++) alive[i] = true;

    // Trump-conservation tie-break tax (added to a candidate's mean-finish score
    // ONLY at selection; the raw MC scores in score[]/nsim[] are left untouched so
    // the CL_EXPLAIN dump still shows true rollout values). Attacks only, deck
    // alive only — leading/throwing a trump is discretionary spend; covering with
    // one is forced defense and endgame trumps must be used.
    double keep_pen[CL_MAX_CANDS];
    {
        bool deck_alive = (g->deck_count > 0 || g->has_flipped);
        for (int i = 0; i < C.n; i++) {
            keep_pen[i] = 0.0;
            if (cl_trump_keep > 0.0 && deck_alive) {
                const LegalMove *m = &moves->moves[C.idx[i]];
                if (m->type == MOVE_ATTACK)
                    for (int k = 0; k < m->n_cards; k++)
                        if (m->cards[k].suit == g->power_suit)
                            keep_pen[i] += cl_trump_keep;
            }
        }
    }

    Game *world = world_scratch_game(), *trial = trial_scratch_game();
    SimState *world_sim = world_scratch_sim(), *trial_sim = trial_scratch_sim();

    // The fast bitboard path: convert each sampled WORLD to a compact SimState
    // ONCE, then each candidate just clones the SimState, applies its move on
    // bitboards, and plays out. The struct path (CL_NO_FASTROLL / CL_LEAF /
    // CL_DIFFTEST) keeps the per-candidate Game clone for the leaf solver and
    // the exact-equivalence difftest.
    bool fast_path = !cl_no_fastroll && cl_no_leaf && !cl_difftest && !cl_flag("CL_NO_WORLDSIM");

    // Stage 1: all candidates on W1 shared worlds.
    // Stage 2: surviving third on W2 more shared worlds.
    // Stage 3: top 2 duel on W3 final shared worlds.
    bool raced = false;
    if (cl_race && fast_path) {
        raced = true;
        int wtot = W1 + W2 + W3;
        int block = cl_race_block > 0 ? cl_race_block : W1 / 4;
        if (block < 16) block = 16;
        int wmax = wtot * cl_race_mult;
        if (wmax > CL_MAX_W) wmax = CL_MAX_W;
        int w = 0, blocks = 0;
        for (;;) {
            int n_alive = 0;
            for (int i = 0; i < C.n; i++) if (alive[i]) n_alive++;
            bool final2 = (n_alive <= 2);
            for (int e = 0; e < block && w < wmax; e++, w++) {
                uint32_t wseed = cl_mix(base, (uint32_t)(w + 1) * 0x85EBCA77u);
                bool use_voids  = (w % cl_void_mod) != cl_void_mod - 1;
                bool use_floors = !cl_no_floors && (w % cl_floor_mod) == 0;
                bool use_hwc = cl_hwc && (w % cl_hwc_mod) != cl_hwc_mod - 1;
                int ok = cl_sample_world(world, g, bot_idx, &B, wseed, use_voids, use_floors, use_hwc);
                int tries = 0;
                while (!ok && tries < cl_hwc_tries) {
                    tries++; cl_hwc_stat_rejects++;
                    uint32_t rs = cl_mix(wseed, 0xC0FFEEu + (uint32_t)tries);
                    ok = cl_sample_world(world, g, bot_idx, &B, rs, use_voids, use_floors, use_hwc);
                }
                if (!ok) cl_hwc_stat_fail++;
                if (use_hwc) cl_hwc_stat_worlds++;
                uint32_t sim_rng = cl_mix(wseed, 0x51AB1E5u);
                cd_sim_from_game(world_sim, world);
                bool self_w = cl_self && (final2 || cl_self_stage == 0);
                bool reply_w = cl_reply && (final2 || cl_reply_stage == 0);
                for (int ci = 0; ci < C.n; ci++) {
                    if (!alive[ci]) continue;
                    *trial_sim = *world_sim;
                    game_rng_set(sim_rng);
                    int fp;
                    if (!cd_sim_apply_root_move(trial_sim, bot_idx, &moves->moves[C.idx[ci]])) {
                        fp = g->num_players;
                    } else if (self_w) {
                        fp = cd_sim_playout_self(trial_sim, bot_idx, 600,
                                                 cl_bbleaf_on ? cl_bbleaf_cards_eff : 0,
                                                 cl_bbleaf_budget, cl_polmap, cl_self_cap);
                    } else if (reply_w) {
                        fp = cd_sim_playout_reply(trial_sim, bot_idx, 600,
                                                  cl_bbleaf_on ? cl_bbleaf_cards_eff : 0,
                                                  cl_bbleaf_budget, cl_polmap, cl_reply_cap);
                    } else {
                        fp = (cl_bbleaf_on || cl_polmap)
                           ? cd_sim_playout_pol(trial_sim, bot_idx, 600, !cl_no_earlyexit,
                                                cl_bbleaf_on ? cl_bbleaf_cards_eff : 0,
                                                cl_bbleaf_budget, cl_polmap)
                           : cd_sim_playout(trial_sim, bot_idx, 600, !cl_no_earlyexit);
                    }
                    if (fp == 0) fp = g->num_players;
                    score[ci] += (double)fp;
                    nsim[ci]++;
                    cl_fpw[ci][w] = (int8_t)fp;
                }
            }
            blocks++;
            n_alive = 0;
            for (int i = 0; i < C.n; i++) if (alive[i]) n_alive++;
            if (n_alive <= 1 || w >= wmax) break;
            if (blocks < cl_race_min_blocks) continue;
            // Leader by mean (+ trump-keep tax), ties to the cheapest-ranked.
            int a = -1; double va = 1e30;
            for (int i = 0; i < C.n; i++) {
                if (!alive[i] || nsim[i] == 0) continue;
                double v = score[i] / (double)nsim[i] + keep_pen[i];
                if (v < va) { va = v; a = i; }
            }
            if (a < 0) break;
            // Drop every candidate whose paired deficit to the leader is proven.
            double best_other_t = 1e30;
            for (int i = 0; i < C.n; i++) {
                if (!alive[i] || i == a) continue;
                double ds = 0, ds2 = 0; int n = 0;
                for (int q = 0; q < w; q++) {
                    double d = (double)cl_fpw[i][q] - (double)cl_fpw[a][q];
                    ds += d; ds2 += d * d; n++;
                }
                if (n < 2) continue;
                double md = ds / n + keep_pen[i] - keep_pen[a];
                double var = (ds2 - ds * ds / n) / (n - 1);
                double se = sqrt(var > 0 ? var / n : 0);
                double tt = se > 0 ? md / se : (md > 0 ? 1e9 : 0);
                if (tt > cl_race_c) alive[i] = false;
                else if (tt < best_other_t) best_other_t = tt;
            }
            n_alive = 0;
            for (int i = 0; i < C.n; i++) if (alive[i]) n_alive++;
            if (n_alive <= 1) break;
            if (n_alive == 2 && best_other_t > cl_race_cf) break;   // final pair separated
        }
    }

    int w_next = 0;   // first unused world index (stages extend contiguously)
    for (int stage = 0; stage < 3 && !raced; stage++) {
        int block = (stage == 0) ? W1 : (stage == 1) ? W2 : W3;
        int w_lo = w_next;
        int w_hi = w_lo + block;
        int extra = 0;
      for (;;) {
        for (int w = w_lo; w < w_hi; w++) {
            uint32_t wseed = cl_mix(base, (uint32_t)(w + 1) * 0x85EBCA77u);
            // Belief mixture: voids assume cover-if-you-can pickups (3 of 4
            // worlds), floors assume lowest-first attackers (every other
            // world). Per-player distrust already cleared bogus constraints.
            bool use_voids  = (w % cl_void_mod) != cl_void_mod - 1;
            bool use_floors = !cl_no_floors && (w % cl_floor_mod) == 0;
            bool use_hwc = cl_hwc && (w % cl_hwc_mod) != cl_hwc_mod - 1;
            {
                int ok = cl_sample_world(world, g, bot_idx, &B, wseed, use_voids, use_floors, use_hwc);
                int tries = 0;
                while (!ok && tries < cl_hwc_tries) {
                    tries++;
                    cl_hwc_stat_rejects++;
                    uint32_t rs = cl_mix(wseed, 0xC0FFEEu + (uint32_t)tries);
                    ok = cl_sample_world(world, g, bot_idx, &B, rs, use_voids, use_floors, use_hwc);
                }
                if (!ok) cl_hwc_stat_fail++;
                if (use_hwc) cl_hwc_stat_worlds++;
            }
            uint32_t sim_rng = cl_mix(wseed, 0x51AB1E5u);

            if (fast_path) {
                cd_sim_from_game(world_sim, world);     // convert world ONCE
                bool reply_stage = cl_reply && stage >= cl_reply_stage;
                bool self_stage = cl_self && stage >= cl_self_stage;
                for (int ci = 0; ci < C.n; ci++) {
                    if (!alive[ci]) continue;
                    *trial_sim = *world_sim;            // cheap struct copy
                    game_rng_set(sim_rng);              // identical stream
                    int fp;
                    if (!cd_sim_apply_root_move(trial_sim, bot_idx,
                                                &moves->moves[C.idx[ci]])) {
                        fp = g->num_players;
                    } else if (xdeck_on && cl_xdeck_eval(trial_sim, bot_idx, wseed, &fp)) {
                        /* exact live-deck value */
                    } else if (self_stage) {
                        fp = cd_sim_playout_self(trial_sim, bot_idx, 600,
                                                 cl_bbleaf_on ? cl_bbleaf_cards_eff : 0,
                                                 cl_bbleaf_budget, cl_polmap, cl_self_cap);
                        if (fp == 0) fp = g->num_players;
                    } else if (reply_stage) {
                        fp = cd_sim_playout_reply(trial_sim, bot_idx, 600,
                                                  cl_bbleaf_on ? cl_bbleaf_cards_eff : 0,
                                                  cl_bbleaf_budget, cl_polmap,
                                                  cl_reply_cap);
                        if (fp == 0) fp = g->num_players;
                    } else {
                        fp = (cl_bbleaf_on || cl_polmap)
                           ? cd_sim_playout_pol(trial_sim, bot_idx, 600,
                                                !cl_no_earlyexit,
                                                cl_bbleaf_on ? cl_bbleaf_cards_eff : 0,
                                                cl_bbleaf_budget, cl_polmap)
                           : cd_sim_playout(trial_sim, bot_idx, 600, !cl_no_earlyexit);
                        if (fp == 0) fp = g->num_players;
                    }
                    score[ci] += (double)fp;
                    nsim[ci]++;
                    if (w < CL_MAX_W) cl_fpw[ci][w] = (int8_t)fp;
                }
                continue;
            }

            for (int ci = 0; ci < C.n; ci++) {
                if (!alive[ci]) continue;
                cl_lite_clone(trial, world);
                game_rng_set(sim_rng);   // identical stream for every move
                if (!cl_apply(trial, bot_idx, &moves->moves[C.idx[ci]])) {
                    score[ci] += (double)g->num_players;
                    nsim[ci]++;
                    if (w < CL_MAX_W) cl_fpw[ci][w] = (int8_t)g->num_players;
                    continue;
                }
                int fp = cl_rollout(trial, bot_idx, 600);
                if (fp == 0) fp = g->num_players;
                score[ci] += (double)fp;
                nsim[ci]++;
                if (w < CL_MAX_W) cl_fpw[ci][w] = (int8_t)fp;
            }
        }
        w_next = w_hi;
        // Adaptive extension: are the two leaders separated? Every alive
        // candidate has been simulated on every world so far (pruning only
        // happens between stages), so their finishes pair world-for-world.
        if (stage < cl_adapt_stage || extra >= cl_adapt_k || w_hi + block > CL_MAX_W) break;
        {
            int a = -1, b2 = -1; double va = 1e30, vb = 1e30;
            for (int i = 0; i < C.n; i++) {
                if (!alive[i] || nsim[i] == 0) continue;
                double v = score[i] / (double)nsim[i] + keep_pen[i];
                if (v < va) { b2 = a; vb = va; a = i; va = v; }
                else if (v < vb) { b2 = i; vb = v; }
            }
            if (a < 0 || b2 < 0) break;
            double ds = 0, ds2 = 0; int n = 0;
            for (int w = 0; w < w_hi; w++) {
                double d = (double)cl_fpw[a][w] - (double)cl_fpw[b2][w];
                ds += d; ds2 += d * d; n++;
            }
            if (n < 2) break;
            double md = ds / n + keep_pen[a] - keep_pen[b2];
            double var = (ds2 - ds * ds / n) / (n - 1);
            double se = sqrt(var > 0 ? var / n : 0);
            if (se <= 0 || fabs(md) >= cl_adapt_c * se) break;   // separated: done
        }
        w_lo = w_hi; w_hi += block; extra++;
      }
        if (stage < 2) {
            int n_alive = 0;
            for (int i = 0; i < C.n; i++) if (alive[i]) n_alive++;
            int keep;
            if (stage == 0) {
                keep = (cl_keep1 > 0) ? cl_keep1 : (cl_oracle ? (C.n + 1) / 2 : C.n / 3);
                if (keep < (cl_oracle ? 4 : 3)) keep = cl_oracle ? 4 : 3;
            } else {
                keep = (cl_keep2 > 0) ? cl_keep2 : (cl_oracle ? 3 : 2);
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
    // Information-hiding rule (docs/CL20_HIDE_UNCOVERABLE.md). If cl20 is
    // about to play a COVER while it cannot cover EVERY uncovered card on the
    // table (no full cover exists in the legal set), pick up immediately. A
    // doomed partial cover only leaks its cover cards — returned to hand and
    // logged by the pickup that is coming anyway — to the memory-keeping MC
    // opponents, and covering opens throw-in windows that grow the pickup.
    // Weakly dominant: never costs a card or tempo. ON for every seat.
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
    game_rng_set(saved_rng);
    return chosen;
}

// Oracle semtex: the same brain with 6x the sampled-world budget and wider
// candidate survival. Research-only — used to audit losses: where the oracle
// picks a different move, the default budget was the binding constraint.
// cl20's public entry: cl20's brain PLUS the LEAFBOOK endgame oracle
// (c/LEAFBOOK.md, docs/L1_SPEND_PLAN.md §4). A book hit terminates a
// round-boundary ≤K-card subtree with a proven-exact value, so cl20 resolves
// the same lines with fewer solver nodes — ~15% faster e2e p50 (measured in the
// wasm bot bench), strength-neutral (the book only shifts play through the
// shared node-budget channel, like a bigger TT; 4,000-game paired vs espresso:
// wash). No-op unless built -DCD_LEAFBOOK.
int cl20_strategy_choose(const Game *g, int bot_idx,
                            const LegalMoves *moves, void *ctx) {
    cd_sim_set_leafbook(1);
    int r = cl20_choose_impl(g, bot_idx, moves, ctx);
    cd_sim_set_leafbook(0);
    return r;
}

int cl20_oracle_strategy_choose(const Game *g, int bot_idx,
                                  const LegalMoves *moves, void *ctx) {
    cl_oracle = 1;
    int r = cl20_strategy_choose(g, bot_idx, moves, ctx);
    cl_oracle = 0;
    return r;
}
