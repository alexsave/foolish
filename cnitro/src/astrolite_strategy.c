// Astrolite — cordite + explicit defender card-management heuristics.
//
// Cordite's engine verbatim (notes below), with three "levers" bolted onto the
// DEFENDER's cover decision — the exact spot where cordite's terminal-only
// Monte Carlo is indifferent (covering ≡ pickup in terminal ownership, info
// leakage costs nothing, trump waste is invisible). Each lever is gated by an
// env flag (AS_NO_LEVER1/2/3) so its contribution can be measured:
//
//   Lever 1 (hard): if you cannot cover EVERY uncovered attack AND no more
//     cards can land (uncovered >= your hand size), drop all cover moves —
//     covering only leaks cards. Pick up (a legal pass still competes).
//   Lever 2 (prune): if you CAN cover everything, forbid any cover that
//     strands a remaining attack (e.g. trumping the off-suit king so the
//     trump king can no longer be covered). Full covers always survive.
//   Lever 3 (bias): if you can't cover all but more could still land, a
//     "baiting" partial cover must beat just-picking-up by a margin in the MC
//     (else pick up). Pickup is protected through the tournament.
//
// ---- cordite's original design notes ----
// Belief-constrained determinized Monte Carlo, v2.
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

#include "astrolite_strategy.h"
#include "strategy.h"
#include "card.h"
#include "game.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

// ---------- small utils ------------------------------------------------

static inline int cd_card_score(Card c, int power) {
    return c.value + (c.suit == power ? 1000 : 0);
}

// ---------- astrolite: cover-feasibility helpers ------------------------
// Bipartite matching (Kuhn) between uncovered attacks and defender hand cards:
// can EVERY attack be assigned a distinct covering card? This is the engine
// behind all three levers — "can I clear the whole table?" and "does this
// cover leave the rest coverable?".

static bool as_augment(int u, const Card *att, const Card *hand, int nh,
                       int power, int *matchR, bool *seen) {
    for (int v = 0; v < nh; v++) {
        if (seen[v] || !can_cover(att[u], hand[v], power)) continue;
        seen[v] = true;
        if (matchR[v] < 0 || as_augment(matchR[v], att, hand, nh, power, matchR, seen)) {
            matchR[v] = u;
            return true;
        }
    }
    return false;
}

static bool as_can_cover_all(const Card *att, int na, const Card *hand, int nh,
                             int power) {
    if (na == 0) return true;
    if (nh < na) return false;
    int matchR[MAX_HAND_SIZE];
    for (int v = 0; v < nh; v++) matchR[v] = -1;
    int matched = 0;
    for (int u = 0; u < na; u++) {
        bool seen[MAX_HAND_SIZE] = { false };
        if (as_augment(u, att, hand, nh, power, matchR, seen)) matched++;
    }
    return matched == na;
}

// Would playing cover move `m` still leave every remaining uncovered attack
// coverable by the remaining hand? (Lever 2's stranding test.)
static bool as_cover_keeps_feasible(const Game *g, int bot_idx,
                                    const LegalMove *m,
                                    const Card *unc, int U) {
    const Player *me = &g->players[bot_idx];
    Card ratt[MAX_BATTLES]; int rna = 0;
    for (int i = 0; i < U; i++) {
        bool covered = false;
        for (int j = 0; j < m->n_cards; j++)
            if (card_eq(unc[i], m->attack_cards[j])) { covered = true; break; }
        if (!covered) ratt[rna++] = unc[i];
    }
    if (rna == 0) return true;   // full cover strands nothing
    Card rhand[MAX_HAND_SIZE]; int rnh = 0;
    bool used_cover[MAX_MOVE_CARDS] = { false };
    for (int j = 0; j < me->hand_count; j++) {
        bool is_cover = false;
        for (int c = 0; c < m->n_cards; c++) {
            if (!used_cover[c] && card_eq(me->hand[j], m->cards[c])) {
                used_cover[c] = true; is_cover = true; break;
            }
        }
        if (!is_cover) rhand[rnh++] = me->hand[j];
    }
    return as_can_cover_all(ratt, rna, rhand, rnh, g->power_suit);
}

// Per-card "cost" for the lead-low (L6) / cover-low (L7) biases: rank value,
// with trumps pushed far above any non-trump so they're spent last.
static inline int as_card_cost(Card c, int power) {
    return c.value + (c.suit == power ? 50 : 0);
}
static int as_move_cost(const LegalMove *m, int power) {
    int s = 0;
    for (int i = 0; i < m->n_cards; i++) s += as_card_cost(m->cards[i], power);
    return s;
}

// Find the MOVE_ATTACK in `moves` whose card multiset equals bundle[0..bn) —
// used by L5 to map a greedily-grown attack back to a single legal move index.
static int as_find_attack(const LegalMoves *moves, const Card *bundle, int bn) {
    for (int i = 0; i < moves->n; i++) {
        const LegalMove *m = &moves->moves[i];
        if (m->type != MOVE_ATTACK || m->n_cards != bn) continue;
        bool used[MAX_MOVE_CARDS] = { false };
        int matched = 0;
        for (int a = 0; a < bn; a++)
            for (int b = 0; b < m->n_cards; b++)
                if (!used[b] && card_eq(bundle[a], m->cards[b])) { used[b] = true; matched++; break; }
        if (matched == bn) return i;
    }
    return -1;
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
static _Thread_local int cd_no_solve = 0, cd_no_voids = 0, cd_no_flip = 0;
static _Thread_local int cd_no_floors = 0, cd_no_leaf = 0, cd_no_avoid = 0;
static _Thread_local int cd_no_earlyexit = 0;
static _Thread_local int cd_verify = 0;
static _Thread_local int cd_w1_override = 0, cd_w2_override = 0;
static _Thread_local int cd_w3_override = -1;

// Astrolite lever switches + selection margins (expected-finish units).
// L3 "bait" margin: a partial cover must beat pickup by this much.
// L4 "grab" margin: pickup must beat the clean full cover by this much (i.e.
// grabbing those cards — e.g. a trump in the attacked set — has to pay).
static _Thread_local int as_flags_loaded = 0;
static _Thread_local int as_no_l1 = 0, as_no_l2 = 0, as_no_l3 = 0, as_no_l4 = 0;
static _Thread_local int as_no_l5 = 0, as_no_l6 = 0, as_no_l7 = 0;
static _Thread_local double as_bait_margin = 0.10;
static _Thread_local double as_grab_margin = 0.10;
static _Thread_local double as_leadlow_margin  = 0.01;  // L6: lead-low bias / rank
static _Thread_local double as_coverlow_margin = 0.01;  // L7: cover-low bias / rank
static _Thread_local int as_in_l5_expand = 0;           // L5 recursion guard
// (b) Raised exact-endgame reach vs cordite (cap 20, budget 200k). Bigger
// 2-player endgames (huge hands from repeated pickups) become solvable; the
// node budget still aborts to MC on the genuinely intractable ones.
static _Thread_local int  as_solve_cards  = 28;
static _Thread_local long as_solve_budget = 400000;

// Lever-firing stats (AS_STATS=1): how often each lever actually triggers, so
// "it should help" can be checked against "it almost never fires".
static long as_stat_dec = 0, as_stat_l1 = 0, as_stat_l2 = 0;
static long as_stat_l3 = 0, as_stat_l4 = 0;
static long as_stat_l5 = 0, as_stat_l5_extra = 0, as_stat_l6 = 0, as_stat_l7 = 0;
static int  as_stats_on = 0;
static void as_print_stats(void) {
    fprintf(stderr,
        "[AS_STATS] defender-uncovered=%ld | L1 pickup-forced=%ld "
        "L2 strand-pruned=%ld L3 bait-gate=%ld L4 fullcover-pref=%ld | "
        "L5 attacks-expanded=%ld (+%ld cards) L6 lead-low=%ld L7 cover-low=%ld\n",
        as_stat_dec, as_stat_l1, as_stat_l2, as_stat_l3, as_stat_l4,
        as_stat_l5, as_stat_l5_extra, as_stat_l6, as_stat_l7);
}

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
    Game       *child;   // [CD_SOLVE_MAX_DEPTH]
    LegalMoves *mv;      // [CD_SOLVE_MAX_DEPTH]
} Solver;

static _Thread_local Game       *cd_solver_child = NULL;
static _Thread_local LegalMoves *cd_solver_mv = NULL;

static bool cd_solver_ready(void) {
    if (!cd_solver_child) {
        cd_solver_child = malloc(sizeof(Game) * CD_SOLVE_MAX_DEPTH);
        cd_solver_mv    = malloc(sizeof(LegalMoves) * CD_SOLVE_MAX_DEPTH);
    }
    return cd_solver_child && cd_solver_mv;
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

    LegalMoves *mv = &S->mv[depth];
    calculate_legal_moves(g, actor, mv);
    if (mv->n == 0) return 0;
    if (mv->n > CD_SOLVE_MAX_MOVES) { S->aborted = true; return 0; }

    bool maximizing = (actor == S->me);
    int best = maximizing ? -2000 : 2000;
    for (int i = 0; i < mv->n; i++) {
        Game *child = &S->child[depth];
        cd_lite_clone(child, g);
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
    S.child   = cd_solver_child;
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
    bool deck_active = (g->deck_count > 0 || g->has_flipped);
    if (deck_active || cd_in_count(g) == 2) return handwritten_strategy_choose;
    return espresso_strategy_choose;
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
            LegalMoves moves;
            calculate_legal_moves_lite(g, pi, &moves);
            if (moves.n == 0) continue;
            StrategyFn fn = cd_rollout_for(g);
            int idx = fn(g, pi, &moves, NULL);
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
    if (total > as_solve_cards) return -1;

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

    Solver S;
    S.budget  = as_solve_budget;
    S.aborted = false;
    S.me      = bot_idx;
    S.child   = cd_solver_child;
    S.mv      = cd_solver_mv;

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
        Game child;
        cd_lite_clone(&child, &root);
        if (!cd_apply(&child, bot_idx, &moves->moves[i])) continue;
        S.aborted = false;
        int v = cd_solve(&S, &child, alpha, 2000, 1);
        if (S.budget <= 0) return -1;   // out of budget: no claims at all
        if (S.aborted) { if (bail_on_abort) return -1; any_abort = true; continue; }
        if (v > best_v) { best_v = v; best_idx = i; }
        if (v > alpha) alpha = v;
    }
    if (best_idx >= 0) return best_idx;
    if (cd_no_avoid || any_abort) return -1;

    // Pass 2 — loss avoidance: no win exists, so classify each move with a
    // null window around 0 (sign only, maximal pruning).
    S.budget = as_solve_budget * 3 / 4;
    int n_loss = 0, n_nonloss = 0;
    for (int i = 0; i < moves->n; i++) {
        Game child;
        cd_lite_clone(&child, &root);
        if (!cd_apply(&child, bot_idx, &moves->moves[i])) continue;
        S.aborted = false;
        int v = cd_solve(&S, &child, -1, 0, 1);
        if (S.budget <= 0 || S.aborted) continue;   // unknown
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
    if (num_players <= 2)      { *W1 = 16; *W2 = 28; *W3 = 28; }
    else if (num_players <= 4) { *W1 = 14; *W2 = 28; *W3 = 28; }
    else if (num_players <= 6) { *W1 = 20; *W2 = 40; *W3 = 28; }
    else                       { *W1 = 20; *W2 = 40; *W3 = 24; }
    if (cd_w1_override > 0) *W1 = cd_w1_override;
    if (cd_w2_override > 0) *W2 = cd_w2_override;
    if (cd_w3_override >= 0) *W3 = cd_w3_override;
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

int astrolite_strategy_choose(const Game *g, int bot_idx,
                              const LegalMoves *moves, void *ctx) {
    (void)ctx;
    if (moves->n == 0) return -1;
    if (moves->n == 1) return 0;

    if (!as_flags_loaded) {
        as_no_l1 = cd_flag("AS_NO_LEVER1");
        as_no_l2 = cd_flag("AS_NO_LEVER2");
        as_no_l3 = cd_flag("AS_NO_LEVER3");
        // Lever 4 is OPT-IN: it regressed pc4 (it overrides the MC's legitimate
        // pickup choices on the real cover-vs-pickup tradeoff). AS_LEVER4=1 to try.
        as_no_l4 = !cd_flag("AS_LEVER4");
        // L5 (attack chaining), L6 (lead low), L7 (cover low) are OPT-IN.
        as_no_l5 = !cd_flag("AS_LEVER5");
        as_no_l6 = !cd_flag("AS_LEVER6");
        as_no_l7 = !cd_flag("AS_LEVER7");
        as_bait_margin = cd_env_int("AS_BAIT_MARGIN_MILLI", 100) / 1000.0;
        as_grab_margin = cd_env_int("AS_GRAB_MARGIN_MILLI", 100) / 1000.0;
        as_leadlow_margin  = cd_env_int("AS_LEADLOW_MARGIN_MILLI", 10) / 1000.0;
        as_coverlow_margin = cd_env_int("AS_COVERLOW_MARGIN_MILLI", 10) / 1000.0;
        as_solve_cards  = cd_env_int("AS_SOLVE_CARDS", 28);
        as_solve_budget = cd_env_int("AS_SOLVE_BUDGET", 400000);
        as_stats_on = cd_flag("AS_STATS");
        if (as_stats_on) atexit(as_print_stats);
        as_flags_loaded = 1;
    }

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
        cd_leaf_budget = cd_env_int("CD_LEAF_BUDGET", 1500);
        cd_leaf_max_cards = cd_env_int("CD_LEAF_CARDS", 10);
        cd_floor_mod = cd_env_int("CD_FLOOR_MOD", 2);
        if (cd_floor_mod < 1) cd_floor_mod = 1;
        cd_full_logs = cd_flag("CD_FULL_LOGS");
        cd_no_earlyexit = cd_flag("CD_NO_EARLYEXIT");
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

    // ---- astrolite levers: defender cover decision -----------------------
    // forced_loss[] doubles as the candidate-exclusion mask, so levers 1 & 2
    // just mark cover moves they want suppressed. Lever 3 is applied later at
    // the tournament/selection stage.
    bool lever3_active = false, lever4_active = false;
    int  lever_U = 0;
    // L6: lead-low bias on a first attack (leading an empty table).
    bool l6_active = !as_no_l6 && g->num_battles == 0 && bot_idx == g->first_attacker;
    if (l6_active) as_stat_l6++;
    // L7: cover-low bias when defending (set below if there are covers to make).
    bool l7_active = false;
    if (bot_idx == g->defender && g->num_battles > 0) {
        Card unc[MAX_BATTLES]; int U = 0;
        for (int i = 0; i < g->num_battles; i++)
            if (!!card_is_none(g->table_battles[i].defense)) unc[U++] = g->table_battles[i].attack;
        if (U > 0) {
            if (!as_no_l7) { l7_active = true; as_stat_l7++; }
            const Player *me = &g->players[bot_idx];
            int H = me->hand_count;
            bool coverable = as_can_cover_all(unc, U, me->hand, H, g->power_suit);
            bool no_more   = (U >= H);   // attackers can add iff H > U
            int  pickup_idx = -1;
            for (int i = 0; i < moves->n; i++)
                if (moves->moves[i].type == MOVE_PICKUP) { pickup_idx = i; break; }
            lever_U = U;
            as_stat_dec++;

            if (coverable) {
                // LEVER 2: forbid covers that strand a still-coverable attack.
                if (!as_no_l2) {
                    int pruned = 0;
                    for (int i = 0; i < moves->n; i++) {
                        const LegalMove *m = &moves->moves[i];
                        if (m->type == MOVE_COVER
                            && !as_cover_keeps_feasible(g, bot_idx, m, unc, U)) {
                            forced_loss[i] = true; pruned++;
                        }
                    }
                    if (pruned) as_stat_l2++;
                }
                // LEVER 4: prefer the clean full cover; pickup / partial cover
                // must out-score it by a margin (e.g. grabbing a trump in the
                // attacked set has to actually pay). Selection-stage bias.
                if (!as_no_l4) { lever4_active = true; as_stat_l4++; }
            } else if (no_more) {
                // LEVER 1: can't clear, nothing more can land -> covering only
                // leaks cards. Drop all covers; pickup (and any legal pass) MC.
                if (!as_no_l1 && pickup_idx >= 0) {
                    for (int i = 0; i < moves->n; i++)
                        if (moves->moves[i].type == MOVE_COVER) forced_loss[i] = true;
                    as_stat_l1++;
                }
            } else {
                // LEVER 3: can't clear but more could still land -> a "bait"
                // partial cover must out-score pickup by a margin.
                if (!as_no_l3 && pickup_idx >= 0) { lever3_active = true; as_stat_l3++; }
            }
        }
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

    // Lever 3/4 selection bias: one "preferred" safe move is protected through
    // the tournament and unpenalized; "discretionary" moves must beat it by a
    // margin to win. L3: prefer pickup, penalize partial covers. L4: prefer the
    // clean full cover, penalize everything else (pickup + partial covers).
    bool   lever_sel = lever3_active || lever4_active;
    int    sel_pref_ci = -1;
    double sel_margin = 0.0;
    bool   sel_penalize[CD_MAX_CANDS] = { false };
    if (lever_sel) {
        if (lever3_active) {
            sel_margin = as_bait_margin;
            for (int ci = 0; ci < C.n; ci++) {
                const LegalMove *m = &moves->moves[C.idx[ci]];
                if (m->type == MOVE_PICKUP) sel_pref_ci = ci;
                else if (m->type == MOVE_COVER && m->n_cards < lever_U) sel_penalize[ci] = true;
            }
        } else { // lever4
            sel_margin = as_grab_margin;
            for (int ci = 0; ci < C.n; ci++) {
                const LegalMove *m = &moves->moves[C.idx[ci]];
                if (m->type == MOVE_COVER && m->n_cards == lever_U && sel_pref_ci < 0)
                    sel_pref_ci = ci;
            }
            if (sel_pref_ci >= 0)
                for (int ci = 0; ci < C.n; ci++)
                    if (ci != sel_pref_ci) sel_penalize[ci] = true;
        }
        if (sel_pref_ci < 0) lever_sel = false;
    }

    // L6 (lead low) / L7 (cover low): a small per-candidate finish-position
    // penalty proportional to card cost (trumps weighted heavy), applied in BOTH
    // the tournament and final selection so cheap leads/covers aren't dropped.
    // Off by default, so this is all zero unless AS_LEVER6/7 is set.
    double cand_bias[CD_MAX_CANDS] = { 0 };
    if (l6_active || l7_active) {
        for (int ci = 0; ci < C.n; ci++) {
            const LegalMove *m = &moves->moves[C.idx[ci]];
            if (l6_active && m->type == MOVE_ATTACK)
                cand_bias[ci] += as_leadlow_margin * (double)as_move_cost(m, g->power_suit);
            if (l7_active && m->type == MOVE_COVER)
                cand_bias[ci] += as_coverlow_margin * (double)as_move_cost(m, g->power_suit);
        }
    }

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
            uint32_t sim_rng = cd_mix(wseed, 0x51AB1E5u);
            for (int ci = 0; ci < C.n; ci++) {
                if (!alive[ci]) continue;
                cd_lite_clone(&trial, &world);
                game_rng_set(sim_rng);   // identical stream for every move
                if (!cd_apply(&trial, bot_idx, &moves->moves[C.idx[ci]])) {
                    score[ci] += (double)g->num_players;
                    nsim[ci]++;
                    continue;
                }
                int fp = cd_simulate(&trial, bot_idx, 600);
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
                keep = C.n / 3;
                if (keep < 3) keep = 3;
            } else {
                keep = 2;
            }
            if (keep >= n_alive) continue;
            for (int dropped = n_alive - keep; dropped > 0; dropped--) {
                int worst = -1;
                double worst_v = -1e30;
                for (int i = 0; i < C.n; i++) {
                    if (!alive[i]) continue;
                    if (lever_sel && i == sel_pref_ci) continue; // protect preferred move
                    double v = score[i] / (double)(nsim[i] ? nsim[i] : 1) + cand_bias[i];
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
        double v = score[i] / (double)nsim[i] + cand_bias[i];  // L6/L7 bias
        // LEVER 3/4: discretionary moves must clear the preferred move's margin.
        if (lever_sel && sel_penalize[i]) v += sel_margin;
        if (v < best_v) { best_v = v; best = i; }
    }

    int chosen = (best >= 0) ? C.idx[best] : 0;

    // LEVER 5: attack chaining. Once we've committed to an attack, re-run the
    // decision from the post-attack state to see if we'd immediately throw in
    // more (matching the now-on-table ranks); if so, fold those cards into one
    // multi-card attack instead of dribbling them out over later turns. The
    // grown bundle's ranks are always a subset of what's already legal, so it
    // maps back to a single legal move in `moves`.
    if (!as_no_l5 && !as_in_l5_expand && moves->moves[chosen].type == MOVE_ATTACK) {
        const LegalMove *cm = &moves->moves[chosen];
        Card bundle[MAX_MOVE_CARDS]; int bn = 0;
        for (int k = 0; k < cm->n_cards && bn < MAX_MOVE_CARDS; k++) bundle[bn++] = cm->cards[k];
        int base_n = bn;
        Game sim; game_clone(&sim, g);
        if (handle_attack(&sim, bot_idx, cm->cards, cm->n_cards)) {
            as_in_l5_expand = 1;
            for (int guard = 0; guard < 6 && bn < MAX_MOVE_CARDS; guard++) {
                LegalMoves sm;
                calculate_legal_moves(&sim, bot_idx, &sm);
                int any = 0;
                for (int i = 0; i < sm.n; i++)
                    if (sm.moves[i].type == MOVE_ATTACK) { any = 1; break; }
                if (!any) break;
                int sidx = astrolite_strategy_choose(&sim, bot_idx, &sm, NULL);
                if (sidx < 0 || sidx >= sm.n || sm.moves[sidx].type != MOVE_ATTACK) break;
                const LegalMove *am = &sm.moves[sidx];
                int before = bn;
                for (int k = 0; k < am->n_cards && bn < MAX_MOVE_CARDS; k++) bundle[bn++] = am->cards[k];
                if (bn == before) break;
                if (!handle_attack(&sim, bot_idx, am->cards, am->n_cards)) break;
            }
            as_in_l5_expand = 0;
            if (bn > base_n) {
                int fi = as_find_attack(moves, bundle, bn);
                if (fi >= 0) { chosen = fi; as_stat_l5++; as_stat_l5_extra += (bn - base_n); }
            }
        }
    }

    game_rng_set(saved_rng);
    return chosen;
}
