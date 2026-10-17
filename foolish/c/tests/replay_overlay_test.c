// M8, the overlay interleave gate (docs/BOTS_WASM_MEMORY_PLAN.md, wasm_overlay.h).
//
// The bots modules alias the replay codec's scratch (g_rec and g_bn, replay.c)
// INTO the solver's arena solve_ws (cordite_sim.c): the two are never live at
// once, because a bot decision and a replay encode or decode are top-level calls
// that never nest. That is legal only while every replay call writes its scratch
// before it reads it, and while the replay buffers do not overlap each other. This
// drives both families in one process, interleaved, and holds the codec to the
// game it encodes:
//
//   1. a finished game's code, decoded, is the game that was played;
//   2. the encode is byte-identical before and after a burst of solver decisions
//      that has written over the replay scratch (checked: the arena's bytes
//      change), and so is the decode;
//   3. move by move, a solver decision between every two encodes never moves
//      the code.
//
// Built from objects of its own (build/obj/overlay) with the shipped bots
// module's capacity flags and -DCD_WASM_OVERLAY, the overlay the browser's
// Oracle module and the test module carry. It needs the solver arena to be one
// static (the host variant already compiles _Thread_local away), which is why
// it is a native program at all: the overlay is wasm-only by design, because a
// native build with threads would race on it.
//
// This was e2e/replay_solver_overlay.test.ts, which drove the same three
// checks through a C Table on bots_test.wasm.
//
// MUTATION-CHECKED (each applied to c/src, rebuilt, run, and put back): see the
// commit that added this file.
#include <stdio.h>
#include <string.h>

#include "bot_roster.h"
#include "game.h"
#include "legal.h"
#include "replay.h"
#include "strategy.h"
#include "wasm_overlay.h"

#ifndef CD_WASM_OVERLAY
#error "replay_overlay_test is built with -DCD_WASM_OVERLAY (c/Makefile OVERLAY_CFLAGS)"
#endif

static int g_fail, g_pass;
#define CHECK(c, msg) do { if (c) g_pass++; else { g_fail++; fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } } while (0)

static void seed_bytes(unsigned char *seed, int np, int s) {
    for (int i = 0; i < FOOLISH_SEED_LEN; i++) seed[i] = (unsigned char)(i * 31 + s * 13 + np);
}

static void deal(Game *g, int np, const unsigned char *seed, int s) {
    game_set_seed((uint32_t)(s + 1));
    random_strategy_set_seed((uint32_t)(s + 1));
    game_set_deal_seed_bytes(seed, FOOLISH_SEED_LEN);
    memset(g, 0, sizeof *g);
    g->num_players = (int8_t)np;
    for (int i = 0; i < np; i++) g->players[i].status = PLAYER_STATUS_READY;
    start_game(g);
}

// Up to `decisions` decisions of `brain` on `g`; the number made.
static int drive(Game *g, const char *brain, int decisions) {
    static LegalMoves moves;
    const int idx = bot_roster_find(brain);
    int made = 0;
    while (made < decisions && game_done(g) < 0) {
        int acted = 0;
        for (int pi = 0; pi < g->num_players && !acted; pi++) {
            if (!should_bot_act(g, pi)) continue;
            calculate_legal_moves(g, pi, &moves);
            if (moves.n == 0) continue;
            const int k = bot_roster_choose(idx, g, pi, &moves);
            if (k < 0 || k >= moves.n) return made;
            acted = legal_move_apply(g, pi, &moves.moves[k]);
        }
        if (!acted) break;
        made++;
    }
    return made;
}

// FNV-1a over the replay's slice of the arena: whether the solver wrote it.
static uint64_t scratch_hash(void) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned i = CD_OVL_REC_OFF; i < CD_OVL_IO_OFF; i++) h = (h ^ cd_overlay[i]) * 1099511628211ULL;
    return h;
}

// A burst of solver decisions on fresh games: the heaviest searching brains, so
// the endgame solver runs and fills the arena the replay scratch lives in.
static int g_burst;
static int hammer_solver(void) {
    static Game g;
    static const char *const brains[] = { "octogen", "blackpowder", "cordite" };
    unsigned char seed[FOOLISH_SEED_LEN];
    int made = 0;
    for (int b = 0; b < 3; b++) {
        seed_bytes(seed, 2, 7000 + g_burst);
        deal(&g, 2, seed, 7000 + g_burst++);
        made += drive(&g, brains[b], 400);
    }
    return made;
}

static int is_info(int type) {
    return type == LOG_ATTACK || type == LOG_COVER || type == LOG_PASS || type == LOG_PICKUP;
}

// The decoded stream's info records, against the game's own log: same count,
// and each the same type by the same seat.
static int decode_is_the_game(const Game *g, const unsigned char *dec, int dn) {
    ReplayDecodedLog l;
    int at = REPLAY_DEC_HDR, k = 0, rc;
    while ((rc = replay_decoded_log(dec, dn, &at, &l)) > 0) {
        if (!is_info(l.log_type)) continue;
        while (k < g->num_logs && !is_info(g->logs[k].log_type)) k++;
        if (k >= g->num_logs || g->logs[k].log_type != l.log_type || g->logs[k].player_idx != l.seat) return 0;
        k++;
    }
    while (k < g->num_logs && !is_info(g->logs[k].log_type)) k++;
    return rc == 0 && k == g->num_logs;
}

int main(void) {
    static Game game, row;
    static unsigned char code1[1 << 16], code2[1 << 16], dec1[1 << 20], dec2[1 << 20];
    unsigned char seed[FOOLISH_SEED_LEN];

    // A finished game gives a non-trivial stream to encode.
    seed_bytes(seed, 2, 81);
    deal(&game, 2, seed, 81);
    drive(&game, "octogen", 10000);
    CHECK(game_done(&game) >= 0, "the octogen game is played to its end");

    const int n1 = replay_encode_v6_from_game(&game, seed, FOOLISH_SEED_LEN, 1 << 30, code1, (int)sizeof code1);
    CHECK(n1 > 0, "the finished game encodes");
    const int d1 = n1 > 0 ? replay_decode(code1, n1, dec1, (int)sizeof dec1) : -1;
    CHECK(d1 > 0 && decode_is_the_game(&game, dec1, d1), "the code decodes to the game that was played");

    // The burst writes over the replay scratch, or this proves nothing.
    const uint64_t before = scratch_hash();
    const int made = hammer_solver();
    CHECK(made > 100 && scratch_hash() != before, "the solver burst made its decisions over the replay scratch");

    const int n2 = replay_encode_v6_from_game(&game, seed, FOOLISH_SEED_LEN, 1 << 30, code2, (int)sizeof code2);
    CHECK(n2 == n1 && memcmp(code1, code2, (size_t)n1) == 0, "the encode is byte-identical after a solver burst");

    // The decode, sandwiched by solver work, re-inits its scratch too.
    hammer_solver();
    const int d2 = replay_decode(code1, n1, dec2, (int)sizeof dec2);
    CHECK(d2 == d1 && memcmp(dec1, dec2, (size_t)d1) == 0, "the decode is byte-identical after a solver burst");

    // Move by move: a decision between every two encodes never moves the code.
    int stable = 1, steps = 0;
    for (int k = 0; k < 3; k++) {
        unsigned char s2[FOOLISH_SEED_LEN];
        seed_bytes(s2, 2, 9000 + k);
        deal(&row, 2, s2, 9000 + k);
        for (int step = 0; step < 12 && game_done(&row) < 0; step++) {
            if (drive(&row, "blackpowder", 1) == 0) break;
            steps++;
            const int n = replay_encode_v6_from_game(&game, seed, FOOLISH_SEED_LEN, 1 << 30, code2, (int)sizeof code2);
            if (n != n1 || memcmp(code1, code2, (size_t)n1) != 0) stable = 0;
        }
    }
    CHECK(steps > 20 && stable, "an encode between every two solver decisions never moves the code");

    if (g_fail) { fprintf(stderr, "replay_overlay_test: %d failed\n", g_fail); return 1; }
    printf("replay_overlay_test: ok (%d checks, %d burst decisions, code %d bytes)\n", g_pass, made, n1);
    return 0;
}
