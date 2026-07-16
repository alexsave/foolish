// Fulminate — cordite + in-game opponent modeling. Port of
// supabase/functions/_shared/common/strategies/fulminate_strategy.ts and the
// fulminate-only profiling half of cordite_core.ts (profileSeats 1667-1897,
// seatWeightsFromProfiles 1983-2057) back into the C engine the TS was itself
// ported from.
//
// Same belief-constrained determinized Monte-Carlo brain as cordite, but
// BEFORE each decision it reads the public move log to (1) profile each
// opponent seat against a set of archetypes {handwritten, espresso, random,
// simple, greedy, passive, human} and (2) SKEW the rollout policy of that
// seat toward its best-fit archetype, so value estimates reflect how each
// opponent actually plays. No cross-game state: every profile is rebuilt from
// THIS game's logs. Early game (few/no logs) the posterior stays on the
// strong policies and no override is installed, so it behaves exactly like
// cordite until evidence accumulates.
//
// Legitimacy contract (same as cordite/blackpowder): reads ONLY public
// information — the move log (opponent draws are masked {-1,-1}), hand
// counts, table, statuses, deck count, flipped card. Opponents' actual hand
// contents are never read; the "known-held" negative inference below tracks
// only cards a seat PUBLICLY took from the table and hasn't publicly played
// since.
//
// Implementation: a thin wrapper around cordite_strategy_choose. The ONLY
// engine change is the optional per-seat rollout-policy override
// (cordite_set_seat_weights in cordite_strategy.c) that is cleared for plain
// cordite — when cleared the engine is bit-for-bit identical to cordite.

#include "strategy.h"
#include "cordite_strategy.h"
#include "card.h"
#include "game.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ---------- exp ----------------------------------------------------------
// seatWeightsFromProfiles' posterior is a softmax (TS Math.exp, cordite_core
// line 2042). Native builds use libm's exp. The wasm build is freestanding
// (no libm), so it uses a local range-reduced Taylor evaluation instead —
// the ONE knowingly-inexact spot of this port: TS Math.exp is a correctly-
// rounded fdlibm implementation we cannot reproduce bit-for-bit without
// libm. The local version is accurate to well under 1 ulp on the domain the
// softmax uses (x <= 0), so the downstream branch outcomes (the 0.50+ commit
// threshold, the per-world sampling walk) could only differ from TS on
// razor-edge float ties the thresholds are nowhere near.
#if defined(__wasm__) || defined(__wasm32__)
static double fm_exp(double x) {
    if (x <= -708.0) return 0.0;   // softmax weight ~1e-308: negligible mass
    if (x == 0.0) return 1.0;
    // Reduce: x = n*ln2 + r, |r| <= ln2/2, with a two-part ln2 so the
    // reduction itself loses almost no bits.
    static const double LN2_HI  = 6.93147180369123816490e-01;
    static const double LN2_LO  = 1.90821492927058770002e-10;
    static const double INV_LN2 = 1.44269504088896338700e+00;
    double fn = x * INV_LN2;
    int n = (int)(fn >= 0.0 ? fn + 0.5 : fn - 0.5);
    double r = (x - (double)n * LN2_HI) - (double)n * LN2_LO;
    // Taylor to r^13 (nested Horner): with |r| <= 0.3466 the first dropped
    // term r^14/14! is < 4e-18 relative — below double ulp.
    double s = 1.0;
    for (int k = 13; k >= 1; k--) s = 1.0 + r * s / (double)k;
    // Scale by 2^n through the exponent bits. n is in [-1021, 1] here (the
    // -708 clamp above keeps the result normal).
    union { double d; uint64_t u; } sc;
    sc.u = (uint64_t)(1023 + n) << 52;
    return s * sc.d;
}
#else
#include <math.h>
static double fm_exp(double x) { return exp(x); }
#endif

// ---------- seat profile (cordite_core.ts SeatProfile, 1636-1649) ----------

typedef struct {
    int    policy;          // best-fit PolicyId (CORDITE_POL_*)
    double confidence;      // weakness score (0..1) backing the label
    int    decisions;       // # of this seat's decisions scored
    double weak;            // weighted count of discrete weakness signals
    double rnd;             // signal mass attributable to random-like play
    double grd;             // signal mass attributable to greedy-like play
    double smp;             // signal mass attributable to simple/give-up play
    int    trumps_played;   // trumps played while the deck was alive
    int    cards_played;    // cards played while the deck was alive
    int    pickups;         // # times this seat took the table
    int    defends;         // # times this seat covered
    int    declined_cover;  // # takes while KNOWN-holding a cheap legal cover
} FmSeatProfile;

// A log-pair card is "NONE" when masked/absent (hidden draws are {-1,-1});
// mirrors the TS adapter's toInt() -> NONE mapping (fulminate_strategy.ts:32).
static inline bool fm_none(Card c) { return c.suit < 0 || c.value < 0; }

// tallyCards (TS 1682-1689): trumps-vs-cards a seat voluntarily played while
// the deck was alive. Skips NONE entries itself (cover lists may hold them).
static void fm_tally(FmSeatProfile *prof, const Card *cards, int n,
                     bool deck_alive, int power) {
    if (!deck_alive) return;
    for (int i = 0; i < n; i++) {
        if (fm_none(cards[i])) continue;
        prof->cards_played++;
        if (cards[i].suit == power) prof->trumps_played++;
    }
}

// removeKnown (TS 1707-1711): drop one instance of each played card from the
// seat's known-held list, preserving order (indexOf + splice semantics).
#define FM_MAX_KNOWN 80
static void fm_known_remove(Card *kh, int *kn, const Card *cards, int n) {
    for (int c = 0; c < n; c++) {
        for (int i = 0; i < *kn; i++) {
            if (card_eq(kh[i], cards[c])) {
                for (int j = i + 1; j < *kn; j++) kh[j - 1] = kh[j];
                (*kn)--;
                break;
            }
        }
    }
}

// profileSeats (cordite_core.ts 1667-1897): replay the public table state
// across the log and accumulate hand-free weakness features per opponent
// seat. Only RARE-FOR-STRONG actions count as weakness evidence (see the
// design note at TS 1651-1663); everything else is neutral, so a strong seat
// scores ~0 and stays on the handwritten default.
static void fm_profile_seats(const Game *g, int bot_idx, FmSeatProfile *profiles) {
    int n = g->num_players;
    int power = g->power_suit;
    for (int p = 0; p < n; p++) {
        memset(&profiles[p], 0, sizeof(FmSeatProfile));
        profiles[p].policy = CORDITE_POL_HANDWRITTEN;
    }

    // Replayed table (attack cards + covers, {-1,-1} = uncovered).
    Card b_atk[FM_MAX_KNOWN];
    Card b_def[FM_MAX_KNOWN];
    int  bn = 0;
    bool first_attack = true;

    int last_draw_idx = -1;
    for (int i = 0; i < g->num_logs; i++) {
        if (g->logs[i].log_type == LOG_DRAW) last_draw_idx = i;
    }
    bool deck_alive_now = (g->deck_count > 0 || g->has_flipped);

    // NEGATIVE INFERENCE (TS 1701-1711): cards each seat is KNOWN to still
    // hold — taken via pickup, minus any it later played. Certain knowledge
    // from public events only, never the hidden hand.
    static _Thread_local Card kh[MAX_PLAYERS][FM_MAX_KNOWN];
    static _Thread_local int  khn[MAX_PLAYERS];
    for (int p = 0; p < n; p++) khn[p] = 0;

    for (int i = 0; i < g->num_logs; i++) {
        const GameLog *L = &g->logs[i];
        int p = L->player_idx;
        bool deck_alive_at = deck_alive_now || i <= last_draw_idx;
        bool is_opp = p >= 0 && p != bot_idx
                   && g->players[p].status == PLAYER_STATUS_IN;
        FmSeatProfile *prof = is_opp ? &profiles[p] : NULL;

        switch (L->log_type) {
            case LOG_ATTACK: {
                Card cards[MAX_LOG_PAIRS];
                int cn = 0;
                bool any_trump = false;
                for (int k = 0; k < L->num_pairs; k++) {
                    Card c = L->pairs[k].primary;
                    if (fm_none(c)) continue;
                    cards[cn++] = c;
                    if (c.suit == power) any_trump = true;
                }
                if (p >= 0) fm_known_remove(kh[p], &khn[p], cards, cn);
                if (prof) {
                    prof->decisions++;
                    // (a) Trump LEAD (first attack) while the deck is alive —
                    // strong bots essentially never do this (TS 1731-1742).
                    // Kept to the FIRST attack only: cordite's MC does add
                    // trumps to an existing round deliberately, so additional-
                    // attack trumps are not a clean discriminator.
                    if (any_trump && deck_alive_at && first_attack) {
                        prof->weak += 1.0;
                        prof->rnd += 0.5;
                        prof->grd += 0.5;
                    }
                    fm_tally(prof, cards, cn, deck_alive_at, power);
                }
                for (int k = 0; k < cn && bn < FM_MAX_KNOWN; k++) {
                    b_atk[bn] = cards[k];
                    b_def[bn].suit = -1; b_def[bn].value = -1;
                    bn++;
                }
                first_attack = false;
                break;
            }
            case LOG_PASS: {
                Card cards[MAX_LOG_PAIRS];
                int cn = 0;
                for (int k = 0; k < L->num_pairs; k++) {
                    Card c = L->pairs[k].primary;
                    if (!fm_none(c)) cards[cn++] = c;
                }
                if (p >= 0) fm_known_remove(kh[p], &khn[p], cards, cn);
                if (prof) {
                    prof->decisions++;
                    fm_tally(prof, cards, cn, deck_alive_at, power);
                }
                for (int k = 0; k < cn && bn < FM_MAX_KNOWN; k++) {
                    b_atk[bn] = cards[k];
                    b_def[bn].suit = -1; b_def[bn].value = -1;
                    bn++;
                }
                // NB: the TS pass case does NOT reset firstAttack — kept
                // verbatim (the flag only matters for the first-lead signal).
                break;
            }
            case LOG_COVER: {
                Card played[MAX_LOG_PAIRS];
                int pn = 0;
                for (int k = 0; k < L->num_pairs; k++) {
                    Card c = L->pairs[k].primary;
                    if (!fm_none(c)) played[pn++] = c;
                }
                if (p >= 0) fm_known_remove(kh[p], &khn[p], played, pn);
                if (prof) {
                    prof->decisions++;
                    prof->defends++;
                    // tallyCards over ALL pair primaries (TS 1768-1769; the
                    // NONE skip lives inside the tally).
                    for (int k = 0; k < L->num_pairs; k++) {
                        fm_tally(prof, &L->pairs[k].primary, 1, deck_alive_at, power);
                    }
                    for (int k = 0; k < L->num_pairs; k++) {
                        Card cov = L->pairs[k].primary;
                        Card atk = L->pairs[k].target;
                        if (fm_none(cov) || !!card_is_none(L->pairs[k].target) || fm_none(atk)) continue;
                        bool cov_trump = (cov.suit == power);
                        bool atk_trump = (atk.suit == power);
                        // (b) Wasteful trump cover of a VERY LOW (<=7)
                        // non-trump attack (TS 1774-1785): a random/greedy
                        // tell, weighted modestly because the cover MAY be
                        // forced; gated downstream by the per-game rate bar.
                        if (cov_trump && !atk_trump && atk.value <= 7) {
                            prof->weak += 0.6;
                            prof->rnd += 0.3;
                            prof->grd += 0.3;
                        }
                    }
                }
                for (int k = 0; k < L->num_pairs; k++) {
                    if (!!card_is_none(L->pairs[k].target) || fm_none(L->pairs[k].target)) continue;
                    for (int q = 0; q < bn; q++) {
                        if (card_eq(b_atk[q], L->pairs[k].target)) {
                            // indexOf: only the FIRST matching attack, and only
                            // if still uncovered (TS 1793-1797).
                            if (fm_none(b_def[q])) b_def[q] = L->pairs[k].primary;
                            break;
                        }
                    }
                }
                break;
            }
            case LOG_PICKUP: {
                if (prof) {
                    prof->decisions++;
                    prof->pickups++;
                    // Negative inference (TS 1802-1821): took while KNOWN-
                    // holding a CHEAP legal cover (low non-trump only —
                    // declining a trump/high cover is strategic, not weak).
                    if (khn[p] > 0) {
                        for (int q = 0; q < bn; q++) {
                            if (!fm_none(b_def[q])) continue;
                            bool can_cov = false;
                            for (int c = 0; c < khn[p]; c++) {
                                if (kh[p][c].suit == power || kh[p][c].value > 9) continue;
                                if (can_cover(b_atk[q], kh[p][c], power)) {
                                    can_cov = true;
                                    break;
                                }
                            }
                            if (can_cov) { prof->declined_cover++; break; }
                        }
                    }
                }
                // The taker gains all table cards into hand — now known-held.
                if (p >= 0) {
                    for (int q = 0; q < bn && khn[p] < FM_MAX_KNOWN; q++) {
                        if (!fm_none(b_atk[q])) kh[p][khn[p]++] = b_atk[q];
                    }
                    for (int q = 0; q < bn && khn[p] < FM_MAX_KNOWN; q++) {
                        if (!fm_none(b_def[q])) kh[p][khn[p]++] = b_def[q];
                    }
                }
                bn = 0;
                first_attack = true;
                break;
            }
            case LOG_GOOD:
                if (prof) prof->decisions++;   // neutral
                break;
            case LOG_DISCARD:
            case LOG_DEFENDER_CHANGE:
                bn = 0;
                first_attack = true;
                break;
            default:
                break;   // draw / player_out / game_start: no profiling signal
        }
    }

    // Resolve (TS 1843-1896). The dominant signal is the trump-conservation
    // RATE mapped through a [0.40, 0.60] ramp calibrated so strong bots
    // (~0.265 measured) score ~0; the small discrete-signal rate blends in.
    for (int p = 0; p < n; p++) {
        FmSeatProfile *prof = &profiles[p];
        if (p == bot_idx || g->players[p].status != PLAYER_STATUS_IN
            || prof->decisions == 0) {
            prof->policy = CORDITE_POL_HANDWRITTEN;
            prof->confidence = 0.0;
            continue;
        }
        double trump_score = 0.0;
        if (prof->cards_played >= 8) {
            double rate = (double)prof->trumps_played / (double)prof->cards_played;
            trump_score = (rate - 0.40) / (0.60 - 0.40);
            if (trump_score < 0.0) trump_score = 0.0;
            if (trump_score > 1.0) trump_score = 1.0;
        }
        double sig_rate = prof->weak / (double)prof->decisions;
        double sig = sig_rate < 1.0 ? sig_rate : 1.0;
        prof->confidence = trump_score > sig ? trump_score : sig;
        // Pick WHICH weak rollout best matches the observed style (TS
        // 1876-1894); the downstream gate decides WHETHER to use it at all.
        int pol = CORDITE_POL_RANDOM;
        int defend_opps = prof->pickups + prof->defends;
        if (defend_opps >= 3) {
            double pickup_rate = (double)prof->pickups / (double)defend_opps;
            if (pickup_rate >= 0.45) pol = CORDITE_POL_PASSIVE;
            else if (prof->pickups == 0 && defend_opps >= 4) pol = CORDITE_POL_HUMAN;
        }
        if (pol == CORDITE_POL_RANDOM && prof->grd > prof->rnd) pol = CORDITE_POL_GREEDY;
        prof->policy = pol;
    }
}

// seatWeightsFromProfiles (cordite_core.ts 1983-2057): per-seat POSTERIOR
// over the policy basis — softmax(logPrior + evidence-scaled signed votes).
// The prior is heavy on the strong policies, so with little evidence a seat
// stays ~all-strong; only a MAJORITY-weak posterior (the pc-scaled commit
// threshold) lets a seat deviate, and then it keeps its full blend. Returns
// 1 and fills w when at least one seat deviates; returns 0 (leave the
// override off — cordite's exact fast path) otherwise.
static int fm_seat_weights_from_profiles(const Game *g, int bot_idx,
        const FmSeatProfile *profiles,
        double w[MAX_PLAYERS][CORDITE_NUM_POLICIES]) {
    int n = g->num_players;
    // Offline control knob (TS FUL_OFF via globalThis): force NO deviation so
    // fulminate runs cordite's exact path. Read per decision — one getenv per
    // choose call is noise next to the MC loop.
    const char *off = getenv("FUL_OFF");
    if (off && off[0] && off[0] != '0') return 0;

    // log-prior over [hw, esp, rnd, smp, grd, pas, hum]: strongly favour strong.
    static const double log_prior[CORDITE_NUM_POLICIES] =
        { 6.0, 4.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    int extra = n - 2 > 0 ? n - 2 : 0;
    int min_dec = 8 + 4 * extra;   // pc2:8 pc4:16 pc6:24 pc8:32
    // Commit bar (TS 1992-1999): pc2/pc4 at 0.50; pc6+ scales up because each
    // seat reveals fewer decisions there and a borderline-strong seat could
    // cross a low bar by chance.
    double commit_thresh;
    if (n <= 4) commit_thresh = 0.50;
    else {
        commit_thresh = 0.50 + 0.075 * (double)(n - 4);
        if (commit_thresh > 0.85) commit_thresh = 0.85;
    }

    int any_deviate = 0;
    for (int p = 0; p < n; p++) {
        const FmSeatProfile *prof = &profiles[p];
        // oneHotHw default for the hero / OUT / silent seats (TS 2000, 2005).
        for (int k = 0; k < CORDITE_NUM_POLICIES; k++) w[p][k] = 0.0;
        w[p][CORDITE_POL_HANDWRITTEN] = 1.0;
        if (p == bot_idx || g->players[p].status != PLAYER_STATUS_IN
            || prof->decisions == 0) continue;

        double L[CORDITE_NUM_POLICIES];
        for (int k = 0; k < CORDITE_NUM_POLICIES; k++) L[k] = log_prior[k];
        // Evidence factor: 0 with no plays, grows to a cap as the seat
        // reveals ~minDec decisions (TS 2009).
        double ev = (double)prof->decisions / (double)min_dec;
        if (ev > 1.4) ev = 1.4;
        // (1) trump-conservation rate (TS 2011-2016).
        if (prof->cards_played >= 8) {
            double t = ev * ((double)prof->trumps_played / (double)prof->cards_played - 0.30);
            L[CORDITE_POL_HANDWRITTEN] -= 7 * t;
            L[CORDITE_POL_ESPRESSO]    -= 6 * t;
            L[CORDITE_POL_HUMAN]       -= 5 * t;
            L[CORDITE_POL_RANDOM]      += 6 * t;
            L[CORDITE_POL_GREEDY]      += 6 * t;
            L[CORDITE_POL_SIMPLE]      += 2 * t;
            L[CORDITE_POL_PASSIVE]     -= 4 * t;   // passive hoards trumps (takes instead)
        }
        // (2) discrete weak tells (TS 2018-2021).
        double sig = ev * (prof->weak
            / (double)(prof->decisions > 1 ? prof->decisions : 1));
        L[CORDITE_POL_RANDOM]      += 5 * sig;
        L[CORDITE_POL_GREEDY]      += 5 * sig;
        L[CORDITE_POL_HANDWRITTEN] -= 9 * sig;
        L[CORDITE_POL_ESPRESSO]    -= 7 * sig;
        L[CORDITE_POL_PASSIVE]     -= 5 * sig;
        L[CORDITE_POL_HUMAN]       -= 5 * sig;
        // (3) defender style: pickup-rate (TS 2023-2029).
        int dops = prof->pickups + prof->defends;
        if (dops >= 3) {
            double pdev = ev * ((double)prof->pickups / (double)dops - 0.30);
            L[CORDITE_POL_PASSIVE]     += 6 * pdev;   // takes a lot -> timid
            L[CORDITE_POL_HUMAN]       -= 6 * pdev;   // never gives up -> stubborn
            L[CORDITE_POL_HANDWRITTEN] -= 1 * pdev;
        }
        // (4) negative inference: declined a known-held cheap cover
        // (TS 2034-2038) — certain evidence, weighted per instance, capped.
        if (prof->declined_cover > 0) {
            int dcc = prof->declined_cover < 3 ? prof->declined_cover : 3;
            double dc = ev * (double)dcc;
            L[CORDITE_POL_PASSIVE]     += 1.2 * dc;
            L[CORDITE_POL_RANDOM]      += 0.6 * dc;
            L[CORDITE_POL_HANDWRITTEN] -= 1.5 * dc;
            L[CORDITE_POL_ESPRESSO]    -= 1.2 * dc;
            L[CORDITE_POL_HUMAN]       -= 1.5 * dc;
        }
        // softmax (TS 2040-2044).
        double mx = L[0];
        for (int k = 1; k < CORDITE_NUM_POLICIES; k++) if (L[k] > mx) mx = L[k];
        double sum = 0.0;
        double ws[CORDITE_NUM_POLICIES];
        for (int k = 0; k < CORDITE_NUM_POLICIES; k++) {
            double e = fm_exp(L[k] - mx);
            ws[k] = e;
            sum += e;
        }
        double non_strong = 0.0;
        for (int k = 0; k < CORDITE_NUM_POLICIES; k++) {
            ws[k] /= sum;
            if (k != CORDITE_POL_HANDWRITTEN && k != CORDITE_POL_ESPRESSO) {
                non_strong += ws[k];
            }
        }
        // Per-seat commit threshold (TS 2045-2054): a strong seat's leaked
        // weak mass pins EXACTLY to handwritten (zero rollout perturbation);
        // a genuinely-weak seat keeps its full posterior blend.
        if (non_strong < commit_thresh) continue;   // keep the one-hot default
        for (int k = 0; k < CORDITE_NUM_POLICIES; k++) w[p][k] = ws[k];
        any_deviate = 1;
    }
    return any_deviate;
}

// ---------- choose ----------------------------------------------------------
// Mirrors FulminateStrategy.chooseMove (fulminate_strategy.ts:111-141):
// profile -> posterior -> install -> corditeChoose -> ALWAYS clear.
int fulminate_strategy_choose(const Game *g, int bot_idx,
                              const LegalMoves *moves, void *ctx) {
    if (moves->n == 0) return -1;
    if (moves->n == 1) return 0;   // TS wrapper short-circuits before profiling

    static _Thread_local FmSeatProfile profiles[MAX_PLAYERS];
    fm_profile_seats(g, bot_idx, profiles);

    double w[MAX_PLAYERS][CORDITE_NUM_POLICIES];
    if (fm_seat_weights_from_profiles(g, bot_idx, profiles, w)) {
        cordite_set_seat_weights((const double (*)[CORDITE_NUM_POLICIES])w,
                                 g->num_players);
    }
    // No meaningful non-strong mass -> weights stay off and this call IS
    // cordite (the exact fast path), matching the TS null-weights contract.
    int idx = cordite_strategy_choose(g, bot_idx, moves, ctx);
    cordite_clear_seat_weights();   // always restore the cordite-identical default

    if (idx < 0 || idx >= moves->n) idx = 0;   // TS fallback: first legal move
    return idx;
}
