// belief_sizing - how right octogen's retrospective belief is, and how often
// the candidate new signals fire, measured against the real hands.
//
// A measurement tool, never a player: it plays cnitro_eval's games (the same
// seeds, deal, seat order and bot_play loop), the observer (octogen or fusion)
// at seat 0 and one opponent brain at every other seat. It reads the real
// hands and the real legal menus only to SCORE inferences, after the seat that
// made them has chosen; nothing it computes reaches a decision.
//
// Two sources:
//   * the observer's own belief, through og_belief_probe (octogen_strategy.h),
//     at every observer decision with more than one legal move: RAW (every
//     void, floor and mc_tell the log scan inferred) and FINAL (what the sampled
//     worlds apply).
//   * every opponent decision, for the candidate signals (A, D below), with the
//     public condition of each one decided by a counterfactual: the seat's
//     hand plus one extra card, asked of the real move enumerator.
//
// Output: one line per game, "G key=value ...", plain text for an aggregator.
//
//   belief_sizing --observer=octogen --opp=cordite --players=4 --games=100 --seed-start=1

#include "../../src/game.h"
#include "../../src/legal.h"
#include "../../src/strategy.h"
#include "../../src/bot_play.h"
#include "../../src/bot_common.h"
#include "../../src/cordite_sim.h"
#include "../../src/octogen_strategy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

// ---------- the counters of one game ----------------------------------------

typedef struct {
    // The observer's belief, per inference (first sight), RAW stage.
    long void_n, void_wrong, void_n_told, void_wrong_told;          // told at sight
    long void_n_later, void_wrong_later;                            // untold at sight, told by the end
    long floor_n, floor_wrong;
    long void_wrong_late;                                           // right at first sight, wrong later (expected 0)
    // Per observer decision.
    long dec;                                                       // decisions probed
    long dec_applied, dec_applied_wrong;                            // FINAL: >= 1 applied void/floor, >= 1 wrong
    long dec_void_applied, dec_void_applied_wrong;
    long dec_floor_applied, dec_floor_applied_wrong;
    long dec_raw_told, dec_raw_told_wrong;                          // RAW voids on told seats (what a tell drops)
    long dec_any_told;                                              // >= 1 opponent told
    // Tells.
    long tells, tells_false, tells_void_path, tell_dec_sum, tell_step_sum, tell_deck_sum, tell_deckalive;
    long seats;                                                     // opponent seats this game
    // A: pinned proofs at opponent decisions.
    long a1, a1_pre, a1_any, a2, a2_pre, pin_bad;
    // D: naive constraints at opponent decisions (fires, and how many were true).
    long d1, d1_true, d2, d2_true, d3, d3_true, d4, d4_true;
    // A per seat: caught by A (A1 or A2) before mc_tell, or only by A; and what
    // dropping that seat's voids from then on would drop (FINAL, per decision).
    long seats_a, seats_a_first, seats_a_only, seats_told, a_lead_sum, a_lead_n;
    long dec_a_void, dec_a_void_wrong;
    long opp_dec;
    int  steps, finish;
} Ctr;

static Ctr C;

// ---------- per-game state ----------------------------------------------------

static Game *G;            // the real game, for the probe's bookkeeping
static int   g_step;       // moves applied
static int   g_obs_dec;    // observer decisions probed

static Card pins[MAX_PLAYERS][MAX_HAND_SIZE];   // the tool's own public pins (for A)
static int  pins_n[MAX_PLAYERS];
static bool told_seen[MAX_PLAYERS];              // the observer's mc_tell, as last probed
static bool told_end[MAX_PLAYERS];
static int  a_step[MAX_PLAYERS], tell_step[MAX_PLAYERS];   // first A fire, first tell seen (-1: none)
static bool vol_pickup[MAX_PLAYERS], strat_decline[MAX_PLAYERS];   // ground truth so far
static Card last_voids[MAX_PLAYERS][BOT_MAX_VOIDS];
static int  last_void_n[MAX_PLAYERS];
static bool last_void_wrong[MAX_PLAYERS][BOT_MAX_VOIDS];
static int  last_floor[MAX_PLAYERS];

// Inferences made on seats untold at sight, resolved against "told by the end".
typedef struct { int8_t seat; bool wrong; } Inf;
static Inf infs[4096];
static int infs_n;

static bool in_hand(const Player *pl, Card c) {
    for (int j = 0; j < pl->hand_count; j++) if (card_eq(pl->hand[j], c)) return true;
    return false;
}

// The real hand holds an unpinned card one void forbids (pinned: the belief's own).
static bool void_wrong(const Game *g, const BotBelief *b, int p, Card v) {
    const Player *pl = &g->players[p];
    for (int j = 0; j < pl->hand_count; j++) {
        Card c = pl->hand[j];
        if (bot_set_contains(b->pinned[p], b->pinned_n[p], c)) continue;
        if (can_cover(v, c, g->power_suit)) return true;
    }
    return false;
}
static bool floor_wrong(const Game *g, const BotBelief *b, int p) {
    const Player *pl = &g->players[p];
    for (int j = 0; j < pl->hand_count; j++) {
        Card c = pl->hand[j];
        if (bot_set_contains(b->pinned[p], b->pinned_n[p], c)) continue;
        if (bot_floor_forbidden(b, g, p, c)) return true;
    }
    return false;
}

static void probe(int stage, const Game *g, int bot_idx, const BotBelief *b, const bool mc_tell[MAX_PLAYERS]) {
    if (bot_idx != 0 || g != G) return;   // an opponent octogen's own belief, or a world
    if (stage == OG_PROBE_RAW) {
        C.dec++;
        g_obs_dec++;
        bool any_told = false, raw_told = false, raw_told_wrong = false;
        for (int p = 1; p < g->num_players; p++) {
            if (g->players[p].status != PLAYER_STATUS_IN) continue;
            if (mc_tell[p] && !told_seen[p]) {
                told_seen[p] = true;
                tell_step[p] = g_step;
                C.tells++;
                if (!vol_pickup[p] && !strat_decline[p]) C.tells_false++;
                if (vol_pickup[p]) C.tells_void_path++;
                C.tell_dec_sum += g_obs_dec;
                C.tell_step_sum += g_step;
                C.tell_deck_sum += g->deck_count;
                if (g->deck_count > 0 || g->has_flipped) C.tell_deckalive++;
            }
            if (mc_tell[p]) any_told = true;
            // Voids: the list only grows until a draw clears it, so a matching
            // prefix is the same inferences and the rest are new.
            int n = b->void_n[p], keep = 0;
            while (keep < n && keep < last_void_n[p] && card_eq(b->voids[p][keep], last_voids[p][keep])) keep++;
            if (keep < last_void_n[p]) keep = 0;   // cleared and rebuilt
            for (int k = 0; k < n; k++) {
                bool w = void_wrong(g, b, p, b->voids[p][k]);
                if (mc_tell[p]) { raw_told = true; if (w) raw_told_wrong = true; }
                if (k < keep) {
                    if (w && !last_void_wrong[p][k]) C.void_wrong_late++;
                    last_void_wrong[p][k] = w || last_void_wrong[p][k];
                    continue;
                }
                last_void_wrong[p][k] = w;
                C.void_n++;
                if (w) C.void_wrong++;
                if (mc_tell[p]) { C.void_n_told++; if (w) C.void_wrong_told++; }
                else if (infs_n < (int)(sizeof infs / sizeof infs[0])) infs[infs_n++] = (Inf){ (int8_t)p, w };
            }
            for (int k = 0; k < n; k++) last_voids[p][k] = b->voids[p][k];
            last_void_n[p] = n;
            if (b->floor_v[p] > 0 && b->floor_v[p] != last_floor[p]) {
                C.floor_n++;
                if (floor_wrong(g, b, p)) C.floor_wrong++;
            }
            last_floor[p] = b->floor_v[p];
        }
        if (any_told) C.dec_any_told++;
        if (raw_told) { C.dec_raw_told++; if (raw_told_wrong) C.dec_raw_told_wrong++; }
        return;
    }
    // FINAL: what the worlds apply.
    bool av = false, avw = false;   // voids on A-caught seats the tell has not caught
    for (int p = 1; p < g->num_players; p++) {
        if (g->players[p].status != PLAYER_STATUS_IN || a_step[p] < 0 || mc_tell[p]) continue;
        for (int k = 0; k < b->void_n[p]; k++) {
            av = true;
            if (void_wrong(g, b, p, b->voids[p][k])) avw = true;
        }
    }
    if (av) { C.dec_a_void++; if (avw) C.dec_a_void_wrong++; }
    bool app = false, app_w = false, va = false, vw = false, fa = false, fw = false;
    for (int p = 1; p < g->num_players; p++) {
        if (g->players[p].status != PLAYER_STATUS_IN) continue;
        for (int k = 0; k < b->void_n[p]; k++) {
            va = app = true;
            if (void_wrong(g, b, p, b->voids[p][k])) vw = app_w = true;
        }
        if (b->floor_v[p] > 0) {
            fa = app = true;
            if (floor_wrong(g, b, p)) fw = app_w = true;
        }
    }
    if (app) { C.dec_applied++; if (app_w) C.dec_applied_wrong++; }
    if (va) { C.dec_void_applied++; if (vw) C.dec_void_applied_wrong++; }
    if (fa) { C.dec_floor_applied++; if (fw) C.dec_floor_applied_wrong++; }
}

// ---------- counterfactuals -----------------------------------------------------

static Game *CF;
static LegalMoves *CFM;

static bool on_table(const Game *g, Card c) {
    for (int i = 0; i < g->num_battles; i++) {
        if (card_eq(g->table_battles[i].attack, c)) return true;
        if (!card_is_none(g->table_battles[i].defense) && card_eq(g->table_battles[i].defense, c)) return true;
    }
    return false;
}

// The menu seat p would have with `extra` in hand (into CFM). false if it cannot be added.
static bool cf_menu(const Game *g, int p, Card extra) {
    if (g->players[p].hand_count >= MAX_HAND_SIZE || on_table(g, extra)) return false;
    memcpy(CF, g, offsetof(Game, logs) + (size_t)g->num_logs * sizeof(GameLog));
    Player *pl = &CF->players[p];
    pl->hand[pl->hand_count++] = extra;
    calculate_legal_moves(CF, p, CFM);
    return true;
}

static bool move_has(const LegalMove *m, Card c) {
    for (int i = 0; i < m->n_cards; i++) if (card_eq(m->cards[i], c)) return true;
    return false;
}

// Some suit of value v, held by nobody's hand in a way that matters: the first suit
// (non-trump if `nontrump`) whose card is not on the table and not in p's hand.
static int cf_suits(const Game *g, int p, int v, bool nontrump, Card out[4]) {
    int n = 0;
    for (int s = 0; s < 4; s++) {
        if (nontrump && s == g->power_suit) continue;
        Card c = { (int8_t)s, (int8_t)v };
        if (on_table(g, c) || in_hand(&g->players[p], c)) continue;
        out[n++] = c;
    }
    return n;
}

// ---------- the opponent decision: signals A and D ------------------------------

static void opp_signals(const Game *g, int p, const LegalMoves *mv, const LegalMove *m) {
    C.opp_dec++;
    const Player *pl = &g->players[p];
    int power = g->power_suit;
    int unc = 0, covered = 0;
    for (int i = 0; i < g->num_battles; i++) {
        if (card_is_none(g->table_battles[i].defense)) unc++; else covered++;
    }
    for (int k = 0; k < pins_n[p]; k++) if (!in_hand(pl, pins[p][k])) C.pin_bad++;

    // A1: a pickup when a full cover of pinned cards was on the menu (proof it was voluntary).
    if (m->type == MOVE_PICKUP && unc > 0) {
        bool strict = false, any = false;
        for (int i = 0; i < mv->n; i++) {
            const LegalMove *x = &mv->moves[i];
            if (x->type != MOVE_COVER) continue;
            bool all_pinned = true, one_pinned = false;
            for (int k = 0; k < x->n_cards; k++) {
                bool pn = bot_set_contains(pins[p], pins_n[p], x->cards[k]);
                all_pinned &= pn;
                one_pinned |= pn;
            }
            if (one_pinned) any = true;
            if (all_pinned && x->n_cards == unc) strict = true;
        }
        if (strict) { C.a1++; if (!told_seen[p]) C.a1_pre++; if (a_step[p] < 0) a_step[p] = g_step; }
        if (any) C.a1_any++;
    }
    // A2: a cover or pickup when a transfer with a pinned card was on the menu.
    if (m->type == MOVE_COVER || m->type == MOVE_PICKUP) {
        for (int i = 0; i < mv->n; i++) {
            const LegalMove *x = &mv->moves[i];
            if (x->type != MOVE_PASS) continue;
            bool pinned = false;
            for (int k = 0; k < x->n_cards; k++) pinned |= bot_set_contains(pins[p], pins_n[p], x->cards[k]);
            if (pinned) { C.a2++; if (!told_seen[p]) C.a2_pre++; if (a_step[p] < 0) a_step[p] = g_step; break; }
        }
    }

    // D1: covered or picked up where a transfer would be legal with a card of the table's value.
    if ((m->type == MOVE_COVER || m->type == MOVE_PICKUP) && covered == 0 && g->num_battles > 0) {
        int v = g->table_battles[0].attack.value;
        bool one_value = true;
        for (int i = 1; i < g->num_battles; i++) one_value &= g->table_battles[i].attack.value == v;
        if (one_value) {
            Card ex[4];
            int ne = cf_suits(g, p, v, false, ex);
            bool possible = false;
            for (int e = 0; e < ne && !possible; e++) {
                if (!cf_menu(g, p, ex[e])) continue;
                for (int i = 0; i < CFM->n; i++)
                    if (CFM->moves[i].type == MOVE_PASS && move_has(&CFM->moves[i], ex[e])) { possible = true; break; }
            }
            if (possible) {
                C.d1++;
                bool real_pass = false;
                for (int i = 0; i < mv->n; i++) real_pass |= mv->moves[i].type == MOVE_PASS;
                if (!real_pass) C.d1_true++;   // "holds no card of that value"
            }
        }
    }

    // D2: said GOOD where a non-trump attack of a table value would be legal.
    if (m->type == MOVE_GOOD && g->num_battles > 0) {
        uint16_t vals = 0, legal_vals = 0;
        for (int i = 0; i < g->num_battles; i++) {
            Card a = g->table_battles[i].attack, d = g->table_battles[i].defense;
            vals |= (uint16_t)(1u << a.value);
            if (!card_is_none(d)) vals |= (uint16_t)(1u << d.value);
        }
        for (int v = 0; v <= ACE_VALUE; v++) {
            if (!(vals & (1u << v))) continue;
            Card ex[4];
            int ne = cf_suits(g, p, v, true, ex);
            for (int e = 0; e < ne; e++) {
                if (!cf_menu(g, p, ex[e])) continue;
                bool ok = false;
                for (int i = 0; i < CFM->n && !ok; i++)
                    if (CFM->moves[i].type == MOVE_ATTACK && move_has(&CFM->moves[i], ex[e])) ok = true;
                if (ok) { legal_vals |= (uint16_t)(1u << v); break; }
            }
        }
        if (legal_vals) {
            C.d2++;
            bool held = false;   // "holds no non-trump of those values": false if the menu had one
            for (int i = 0; i < mv->n && !held; i++) {
                const LegalMove *x = &mv->moves[i];
                if (x->type != MOVE_ATTACK) continue;
                for (int k = 0; k < x->n_cards; k++)
                    if (x->cards[k].suit != power && (legal_vals & (1u << x->cards[k].value))) held = true;
            }
            if (!held) C.d2_true++;
        }
    }

    // D3: every cover: "no unused card covers that attack more cheaply".
    if (m->type == MOVE_COVER) {
        C.d3++;
        bool cheaper = false;
        for (int k = 0; k < m->n_cards && !cheaper; k++) {
            Card a = m->attack_cards[k], c = m->cards[k];
            int sc = c.value + (c.suit == power ? 1000 : 0);
            for (int j = 0; j < pl->hand_count; j++) {
                Card y = pl->hand[j];
                if (move_has(m, y) || !can_cover(a, y, power)) continue;
                if (y.value + (y.suit == power ? 1000 : 0) < sc) { cheaper = true; break; }
            }
        }
        if (!cheaper) C.d3_true++;
    }

    // D4: a single non-trump card attack where a two-card attack of its value would be legal.
    if (m->type == MOVE_ATTACK && m->n_cards == 1 && m->cards[0].suit != power) {
        Card c = m->cards[0];
        Card ex[4];
        int ne = cf_suits(g, p, c.value, true, ex);
        bool possible = false;
        for (int e = 0; e < ne && !possible; e++) {
            if (!cf_menu(g, p, ex[e])) continue;
            for (int i = 0; i < CFM->n; i++) {
                const LegalMove *x = &CFM->moves[i];
                if (x->type == MOVE_ATTACK && x->n_cards >= 2 && move_has(x, c) && move_has(x, ex[e])) { possible = true; break; }
            }
        }
        if (possible) {
            C.d4++;
            bool second = false;   // "holds no second non-trump of that value"
            for (int i = 0; i < mv->n && !second; i++) {
                const LegalMove *x = &mv->moves[i];
                if (x->type != MOVE_ATTACK || x->n_cards < 2 || !move_has(x, c)) continue;
                for (int k = 0; k < x->n_cards; k++)
                    if (!card_eq(x->cards[k], c) && x->cards[k].value == c.value && x->cards[k].suit != power) second = true;
            }
            if (!second) C.d4_true++;
        }
    }
}

// Ground truth for the tell check, before the move is applied.
static void truth_before(const Game *g, int p, const LegalMoves *mv, const LegalMove *m) {
    int unc = 0;
    for (int i = 0; i < g->num_battles; i++) unc += card_is_none(g->table_battles[i].defense);
    if (m->type == MOVE_PICKUP && unc == 1)
        for (int i = 0; i < mv->n; i++) if (mv->moves[i].type == MOVE_COVER) { vol_pickup[p] = true; break; }
    if (m->type == MOVE_GOOD && bot_in_count(g) > 2)
        for (int i = 0; i < mv->n && !strat_decline[p]; i++) {
            const LegalMove *x = &mv->moves[i];
            if (x->type != MOVE_ATTACK) continue;
            for (int k = 0; k < x->n_cards; k++) if (x->cards[k].suit != g->power_suit) strat_decline[p] = true;
        }
}

// The tool's public pins: a pickup pins the table; a played card leaves.
static void pins_apply(const Game *g, int p, const LegalMove *m) {
    if (m->type == MOVE_PICKUP) {
        for (int i = 0; i < g->num_battles; i++) {
            bot_pin_add(pins[p], &pins_n[p], g->table_battles[i].attack);
            if (!card_is_none(g->table_battles[i].defense)) bot_pin_add(pins[p], &pins_n[p], g->table_battles[i].defense);
        }
    }
    for (int k = 0; k < m->n_cards; k++) bot_pin_remove(pins[p], &pins_n[p], m->cards[k]);
}

// ---------- one game --------------------------------------------------------------

static void play(uint32_t seed, int pc, int obs, int opp) {
    memset(&C, 0, sizeof C);
    memset(pins_n, 0, sizeof pins_n);
    memset(told_seen, 0, sizeof told_seen);
    memset(vol_pickup, 0, sizeof vol_pickup);
    memset(strat_decline, 0, sizeof strat_decline);
    memset(last_void_n, 0, sizeof last_void_n);
    memset(last_floor, 0, sizeof last_floor);
    infs_n = 0;
    for (int p = 0; p < MAX_PLAYERS; p++) a_step[p] = tell_step[p] = -1;
    g_step = g_obs_dec = 0;

    cd_sim_solve_reset();   // as cnitro_eval: a cold solver TT per game
    game_set_seed(seed ? seed : 1);
    random_strategy_set_seed(seed ? seed : 1);
    memset(G, 0, sizeof *G);
    G->num_players = (int8_t)pc;
    for (int i = 0; i < pc; i++) {
        G->players[i].status = PLAYER_STATUS_READY;
        G->players[i].strategy_key = (int8_t)(i == 0 ? obs : opp);
    }
    start_game(G);
    C.seats = pc - 1;

    BotPlay bp;
    bot_play_init(&bp, G, 4000, BOT_PLAY_SHUFFLE, NULL);
    for (int pi; (pi = bot_play_next(&bp)) >= 0;) {
        const LegalMoves *mv = &bp.moves;
        int idx = strategy_choose(G->players[pi].strategy_key, G, pi, mv);
        if (idx < 0 || idx >= mv->n) continue;
        const LegalMove *m = &mv->moves[idx];
        if (pi != 0) {
            opp_signals(G, pi, mv, m);
            truth_before(G, pi, mv, m);
        }
        pins_apply(G, pi, m);
        bot_play_apply(&bp, idx);
        g_step++;
        for (int p = 0; p < pc; p++) if (G->players[p].status != PLAYER_STATUS_IN) pins_n[p] = 0;
    }
    for (int p = 0; p < pc; p++) told_end[p] = told_seen[p];
    for (int p = 1; p < pc; p++) {
        if (tell_step[p] >= 0) C.seats_told++;
        if (a_step[p] < 0) continue;
        C.seats_a++;
        if (tell_step[p] < 0) C.seats_a_only++;
        else if (a_step[p] < tell_step[p]) { C.seats_a_first++; C.a_lead_sum += tell_step[p] - a_step[p]; C.a_lead_n++; }
    }
    for (int i = 0; i < infs_n; i++) {
        if (!told_end[infs[i].seat]) continue;
        C.void_n_later++;
        if (infs[i].wrong) C.void_wrong_later++;
    }
    C.steps = g_step;
    C.finish = -1;
    if (game_done(G) >= 0)
        for (int i = 0; i < G->num_eliminated; i++) if (G->elimination_order[i] == 0) C.finish = i + 1;
}

#define F(x) printf(" " #x "=%ld", (long)C.x)

int main(int argc, char **argv) {
    const char *obs_s = "octogen", *opp_s = "handwritten";
    int pc = 2, games = 10;
    uint32_t seed0 = 1;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--observer=", 11)) obs_s = argv[i] + 11;
        else if (!strncmp(argv[i], "--opp=", 6)) opp_s = argv[i] + 6;
        else if (!strncmp(argv[i], "--players=", 10)) pc = atoi(argv[i] + 10);
        else if (!strncmp(argv[i], "--games=", 8)) games = atoi(argv[i] + 8);
        else if (!strncmp(argv[i], "--seed-start=", 13)) seed0 = (uint32_t)strtoul(argv[i] + 13, NULL, 10);
        else { fprintf(stderr, "unknown flag %s\n", argv[i]); return 2; }
    }
    int obs = parse_strategy(obs_s), opp = parse_strategy(opp_s);
    if (obs < 0 || opp < 0 || pc < 2 || pc > MAX_PLAYERS) { fprintf(stderr, "bad strategy or players\n"); return 2; }
    G = calloc(1, sizeof *G);
    CF = calloc(1, sizeof *CF);
    CFM = calloc(1, sizeof *CFM);
    if (!G || !CF || !CFM) return 1;
    og_belief_probe = probe;
    for (int k = 0; k < games; k++) {
        uint32_t seed = seed0 + (uint32_t)k;
        play(seed, pc, obs, opp);
        printf("G obs=%s opp=%s pc=%d seed=%u", obs_s, opp_s, pc, seed);
        F(void_n); F(void_wrong); F(void_n_told); F(void_wrong_told); F(void_n_later); F(void_wrong_later);
        F(void_wrong_late); F(floor_n); F(floor_wrong);
        F(dec); F(dec_applied); F(dec_applied_wrong); F(dec_void_applied); F(dec_void_applied_wrong);
        F(dec_floor_applied); F(dec_floor_applied_wrong); F(dec_raw_told); F(dec_raw_told_wrong); F(dec_any_told);
        F(tells); F(tells_false); F(tells_void_path); F(tell_dec_sum); F(tell_step_sum); F(tell_deck_sum); F(tell_deckalive);
        F(seats); F(a1); F(a1_pre); F(a1_any); F(a2); F(a2_pre); F(pin_bad);
        F(seats_a); F(seats_a_first); F(seats_a_only); F(seats_told); F(a_lead_sum); F(a_lead_n);
        F(dec_a_void); F(dec_a_void_wrong);
        F(d1); F(d1_true); F(d2); F(d2_true); F(d3); F(d3_true); F(d4); F(d4_true); F(opp_dec);
        F(steps); F(finish);
        printf("\n");
        fflush(stdout);
    }
    return 0;
}
