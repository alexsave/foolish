// L1-budget measurement harness — plays thousands of real engine games and
// reports the OBSERVED maxima that size every static buffer in the wasm
// modules (docs/WASM_L1_BUDGET.md):
//
//   menu          peak calculate_legal_moves count      -> MAX_LEGAL_MOVES
//   snaps/deal    engine_snap_hook fires in start_game  -> MAX_SNAPS (rules)
//   snaps/action  hook fires in one handle_* call       -> MAX_SNAPS (guards)
//   logs/action   logs appended by one handle_* call    -> MAX_LOGS (marshal-reset builds)
//   pairs/log     peak GameLog.num_pairs                -> log-export io math
//   state bytes   peak state_put(VIEW_UNMASKED) size    -> IO_CAP floor
//   enc in/out    replay encode input / blob bytes      -> replay io cap
//   rec, bn       replay coder peaks (REPLAY_STATS)     -> REPLAY_REC_CAP / _BN_CAP
//
// Build with the WASM cap set (MAX_LOG_PAIRS=64 MAX_BATTLES=64) and headroom
// caps (MAX_LOGS=2048 MAX_LEGAL_MOVES=16384) so the observations are TRUE
// maxima, not clamps. Random strategy is the degenerate-game generator;
// handwritten is the realistic one — both run, per player count 2..8.
//
// Usage: l1_measure [games_per_config] [seed0]   (defaults 500, 1337)

#include "../src/game.h"
#include "../src/legal.h"
#include "../src/strategy.h"
#include "../src/replay.h"
#include "../src/view.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ACTIONS 100000
#define ENC_CAP (1 << 20)

extern int replay_stat_max_rec;
extern int replay_stat_max_bn;

static unsigned char g_enc_in[ENC_CAP];
static unsigned char g_enc_out[ENC_CAP];
static unsigned char g_state[1 << 16];

// observed maxima
static int mx_menu, mx_snaps_deal, mx_snaps_action, mx_logs_action, mx_pairs,
           mx_state, mx_enc_in, mx_blob, mx_logs_game, mx_hand, mx_battles,
           mx_move_cards;

static int enc_err[32];
static int mx_rec, mx_bn;
static double mx_rec_per_byte;
static int g_snap_count;
static void count_snap(const Game *g, int tag, int aux) {
    (void)g; (void)tag; (void)aux;
    g_snap_count++;
}
#define MAXX(v, x) do { if ((x) > (v)) (v) = (x); } while (0)

static uint32_t g_sh_seed = 1;
static uint32_t sh_rand(void) {
    g_sh_seed = g_sh_seed * 1664525u + 1013904223u;
    return g_sh_seed;
}

static unsigned char wire_of(Card c) {
    if (c.suit < 0 || c.value < 1) return (unsigned char)REPLAY_CARD_HIDDEN;
    return (unsigned char)(c.suit * 13 + (c.value - 1));
}

// The true initial hands, captured at deal time - the game itself forgets them
// (draw_card splices each played card out) and the encode input needs them.
static unsigned char g_init_hand[MAX_PLAYERS][CARDS_PER_PLAYER];
static int g_init_len[MAX_PLAYERS];

// The marshalled encode input (replay.h): header, the reveal stream, then the
// action stream. The reveals are the initial deal seat-major followed by each
// drawn stock card in draw order, minus the face-up flip (which IS the header
// trump and is never listed).
//
// This measures what the module's own marshalled entry (wasm_replay_encode_v6)
// is handed. Production encodes through replay_encode_v6_from_game, whose input
// is 32 bytes of seed - but both drive the same coder over the same atoms, so
// the choice-log and bignum peaks below are the peaks either way.
static int build_encode_input(const GameLog *logs, int num_logs, bool truncated,
                              int np, Card flipped, int first_attacker,
                              unsigned char *out) {
    if (truncated) return -1;
    int trump_id = flipped.suit * 13 + (flipped.value - 1);
    unsigned char flip_wire = wire_of(flipped);

    static unsigned char reveals[MAX_PLAYERS * CARDS_PER_PLAYER + MAX_DECK];
    int nr = 0;
    for (int s = 0; s < np; s++)
        for (int k = 0; k < g_init_len[s]; k++) reveals[nr++] = g_init_hand[s][k];

    int q = 7;
    int n_actions = 0;
    for (int i = 0; i < num_logs; i++) {
        const GameLog *l = &logs[i];
        bool info = l->log_type == LOG_ATTACK || l->log_type == LOG_COVER
                 || l->log_type == LOG_PASS || l->log_type == LOG_PICKUP;
        if (l->log_type == LOG_DRAW) {
            for (int j = 0; j < l->num_pairs; j++) {
                unsigned char w = wire_of(l->pairs[j].primary);
                if (w > 51) return -1;                  // a masked log cannot be encoded
                if (w != flip_wire) reveals[nr++] = w;  // skip the flip
            }
        }
        if (info) {
            out[q++] = (unsigned char)l->log_type;
            out[q++] = (unsigned char)l->player_idx;
            out[q++] = (unsigned char)l->num_pairs;
            for (int j = 0; j < l->num_pairs; j++) {
                out[q++] = wire_of(l->pairs[j].primary);
                out[q++] = card_is_none(l->pairs[j].target)
                    ? (unsigned char)REPLAY_CARD_NONE : wire_of(l->pairs[j].target);
            }
            n_actions++;
        } else if (l->log_type == LOG_DISCARD && i > 0
                   && logs[i - 1].log_type == LOG_GOOD) {
            out[q++] = (unsigned char)REPLAY_ROUND_END;
            out[q++] = 0xFF;
            out[q++] = 0;
            n_actions++;
        }
    }

    // Splice the reveals in front of the actions.
    memmove(out + 7 + nr, out + 7, (size_t)(q - 7));
    memcpy(out + 7, reveals, (size_t)nr);
    q += nr;

    out[0] = (unsigned char)np;
    out[1] = (unsigned char)trump_id;
    out[2] = (unsigned char)first_attacker;
    out[3] = (unsigned char)(n_actions & 0xff);
    out[4] = (unsigned char)((n_actions >> 8) & 0xff);
    out[5] = (unsigned char)(nr & 0xff);
    out[6] = (unsigned char)((nr >> 8) & 0xff);
    return q;
}

static void observe_state(const Game *g) {
    MAXX(mx_state, state_put(g, VIEW_UNMASKED, g_state));
    MAXX(mx_battles, g->num_battles);
    for (int i = 0; i < g->num_players; i++) MAXX(mx_hand, g->players[i].hand_count);
}

static bool play_game(Game *g, int np, int strat, uint32_t seed) {
    memset(g, 0, sizeof *g);
    g->num_players = (int8_t)np;
    for (int i = 0; i < np; i++) g->players[i].status = PLAYER_STATUS_READY;
    game_set_seed(seed);
    random_strategy_set_seed(seed ^ 0x9e3779b9u);
    g_sh_seed = seed ^ 0x5bd1e995u;

    g_snap_count = 0;
    start_game(g);
    MAXX(mx_snaps_deal, g_snap_count);
    observe_state(g);
    for (int i = 0; i < np; i++) {
        g_init_len[i] = g->players[i].hand_count;
        for (int k = 0; k < g_init_len[i]; k++)
            g_init_hand[i][k] = wire_of(g->players[i].hand[k]);
    }

    static LegalMoves moves;
    int actions = 0;
    while (game_done(g) < 0) {
        if (++actions > MAX_ACTIONS) return false;
        int elig[MAX_PLAYERS], n_e = 0;
        for (int i = 0; i < np; i++) if (should_bot_act(g, i)) elig[n_e++] = i;
        if (n_e == 0) return false;
        for (int i = n_e - 1; i > 0; i--) {
            int j = (int)(sh_rand() % (uint32_t)(i + 1));
            int t = elig[i]; elig[i] = elig[j]; elig[j] = t;
        }
        bool acted = false;
        for (int e = 0; e < n_e && !acted; e++) {
            int pi = elig[e];
            calculate_legal_moves(g, pi, &moves);
            if (moves.n == 0) continue;
            MAXX(mx_menu, moves.n);
            for (int k = 0; k < moves.n; k++) MAXX(mx_move_cards, moves.moves[k].n_cards);
            int mi = strat == STRAT_RANDOM
                ? random_strategy_choose(g, pi, &moves, 0)
                : handwritten_strategy_choose(g, pi, &moves, 0);
            const LegalMove *m = &moves.moves[mi];
            int logs_before = g->num_logs;
            g_snap_count = 0;
            switch (m->type) {
                case MOVE_ATTACK: acted = handle_attack(g, pi, m->cards, m->n_cards); break;
                case MOVE_COVER:  acted = handle_cover(g, pi, m->cards, m->attack_cards, m->n_cards); break;
                case MOVE_PASS:   acted = handle_pass(g, pi, m->cards, m->n_cards); break;
                case MOVE_PICKUP: acted = handle_pickup(g, pi); break;
                case MOVE_GOOD:   acted = handle_good(g, pi); break;
                default: break;
            }
            if (acted) {
                MAXX(mx_snaps_action, g_snap_count);
                MAXX(mx_logs_action, g->num_logs - logs_before);
                observe_state(g);
            }
        }
        if (!acted) return false;
    }
    return true;
}

int main(int argc, char **argv) {
    int games = argc > 1 ? atoi(argv[1]) : 500;
    uint32_t seed0 = argc > 2 ? (uint32_t)atoi(argv[2]) : 1337u;
    static Game g;
    engine_snap_hook = count_snap;

    long played = 0, encoded = 0;
    for (int strat = 0; strat < 2; strat++) {
        for (int np = 2; np <= MAX_PLAYERS; np++) {
            for (int i = 0; i < games; i++) {
                uint32_t seed = seed0 + (uint32_t)(i * 7919) + (uint32_t)np * 104729u
                              + (uint32_t)strat * 1299709u;
                if (!play_game(&g, np, strat, seed)) continue;
                played++;
                MAXX(mx_logs_game, g.num_logs);
                for (int li = 0; li < g.num_logs; li++) MAXX(mx_pairs, g.logs[li].num_pairs);
                bool truncated = g.num_logs >= MAX_LOGS;
                int fa = replay_first_attacker_from_logs(g.logs, g.num_logs);
                if (fa < 0) continue;
                int in_len = build_encode_input(g.logs, g.num_logs, truncated,
                                                np, g.flipped, fa, g_enc_in);
                if (in_len < 0) continue;
                MAXX(mx_enc_in, in_len);
                replay_stat_max_rec = 0; replay_stat_max_bn = 0;
                int blob = replay_encode_v6(g_enc_in, in_len, g_enc_out, ENC_CAP);
                if (blob > 0) {
                    MAXX(mx_blob, blob); encoded++;
                    MAXX(mx_rec, replay_stat_max_rec); MAXX(mx_bn, replay_stat_max_bn);
                    double r = (double)replay_stat_max_rec / (double)in_len;
                    if (r > mx_rec_per_byte) mx_rec_per_byte = r;
                }
                else { int e = -blob; if (e >= 0 && e < 32) enc_err[e]++; }
            }
        }
    }

    printf("games played           %ld  (encoded %ld)\n", played, encoded);
    printf("menu moves        max  %d   (MAX_LEGAL_MOVES)\n", mx_menu);
    printf("move n_cards      max  %d   (MAX_MOVE_CARDS)\n", mx_move_cards);
    printf("snaps in deal     max  %d   (MAX_SNAPS, rules: start_game)\n", mx_snaps_deal);
    printf("snaps per action  max  %d   (MAX_SNAPS, guards)\n", mx_snaps_action);
    printf("logs per action   max  %d   (MAX_LOGS, marshal-reset builds)\n", mx_logs_action);
    printf("logs per game     max  %d   (session log, bots/TS mirror)\n", mx_logs_game);
    printf("pairs per log     max  %d   (MAX_LOG_PAIRS)\n", mx_pairs);
    printf("hand count        max  %d   (MAX_HAND_SIZE)\n", mx_hand);
    printf("battles           max  %d   (MAX_BATTLES)\n", mx_battles);
    printf("state_put bytes   max  %d   (IO floor)\n", mx_state);
    printf("replay enc input  max  %d   (replay io)\n", mx_enc_in);
    printf("replay blob       max  %d   (replay io / MAX_INT_BYTES)\n", mx_blob);
    printf("replay rec        max  %d   (REPLAY_REC_CAP)\n", mx_rec);
    printf("replay bn limbs   max  %d   (REPLAY_BN_CAP)\n", mx_bn);
    printf("rec per input byte max %.4f\n", mx_rec_per_byte);
    for (int e = 0; e < 32; e++) if (enc_err[e]) printf("encode err %-2d          %d games\n", e, enc_err[e]);
    return 0;
}
