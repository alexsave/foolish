// bench_engine.c - raw rules-engine throughput: whole bot games on the C Table,
// in-process, with no commit and no broadcast.
//
//   BENCH_GAMES=400 BENCH_PLAYERS=4 BENCH_STRATEGY=random ./build/bench_engine
//
// Each game is a lobby of bots sealed by the kernel (table_seal), dealt from its
// own seed by table_ready, then driven one action per table_bot_drive until the
// kernel says the game ended - the bot loop's work without the commit products.
// Before every drive the kernel's legal-move enumerator (legal.c) is asked for
// every seat still in, as a client asking for its menu would. Reports games/sec,
// actions/sec (an "action" is one applied bot move) and legal-move
// evaluations/sec; scripts/collect_metrics.mjs reads the last three lines.
//
// It was e2e/bench_engine.ts, the same loop through the whole-kernel wasm test
// module, until that module went (2026-10-17): the server plays the table
// natively (c_base), so the native number is the one that means something.
// Numbers before and after that move measure different builds.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/game.h"
#include "../src/legal.h"
#include "../src/roster.h"
#include "../src/table.h"

// Random play can circle forever (attack, take, attack, take); such a game stops here unfinished.
#define MAX_ACTIONS 5000

static Game g_game;
static TableSnaps g_snaps;
static Table g_table;
static LegalMoves g_moves;
static uint8_t g_row[1 << 16];

static int env_int(const char *name, int dflt) {
    const char *v = getenv(name);
    return v && *v ? atoi(v) : dflt;
}

// The 32-byte deal seed of game `s` at `np` seats (e2e/helpers/bot_table.ts seedBytes).
static void seed_bytes(int np, int s, uint8_t *out) {
    for (int i = 0; i < FOOLISH_SEED_LEN; i++) out[i] = (uint8_t)(i * 31 + s * 13 + np);
}

// A lobby of `np` `brain` bots, every seat ready, dealt from game `n`'s seed.
// 0, or the kernel's refusal.
static int deal(const char *brain, int np, int n) {
    static Game lobby;
    static Roster roster;
    memset(&lobby, 0, sizeof lobby);
    game_reset_to_lobby(&lobby, 0);
    memset(&roster, 0, sizeof roster);
    int rc = roster_set_title(&roster, "bench", 5);
    if (rc < 0) return rc;
    lobby.num_players = (int8_t)np;
    for (int s = 0; s < np; s++) {
        char id[16], name[16];
        const int il = snprintf(id, sizeof id, "seat-%d", s);
        const int nl = snprintf(name, sizeof name, "P%d", s + 1);
        rc = roster_seat_add(&roster, id, il, name, nl, brain, (int)strlen(brain));
        if (rc < 0) return rc;
        lobby.players[s].status = PLAYER_STATUS_READY;
    }
    table_init(&g_table, &g_game, &g_snaps);
    const int len = table_seal(&g_table, &lobby, &roster, g_row, (int)sizeof g_row);
    if (len < 0) return len;
    rc = table_load(&g_table, g_row, len, g_row + len, ROSTER_BYTES);
    if (rc != TABLE_OK) return rc;
    uint8_t seed[FOOLISH_SEED_LEN];
    char hex[2 * FOOLISH_SEED_LEN + 1];
    seed_bytes(np, n, seed);
    for (int i = 0; i < FOOLISH_SEED_LEN; i++) snprintf(hex + 2 * i, 3, "%02x", seed[i]);
    rc = table_ready(&g_table, "seat-0", 6, seed);
    if (rc != TABLE_OK) return rc;
    rc = table_set_deal_seed(&g_table, hex, 2 * FOOLISH_SEED_LEN);
    return rc < 0 ? rc : 0;
}

// One game: its actions and legal-move evaluations; 1 if it ended, 0 if not, or a refusal.
static int play(const char *brain, int np, int n, long *actions, long *evals) {
    const int rc = deal(brain, np, n);
    if (rc < 0) return rc;
    static BotDriveOut drv;
    for (int a = 0; a < MAX_ACTIONS; a++) {
        for (int s = 0; s < np; s++) {
            if (g_game.players[s].status != PLAYER_STATUS_IN) continue;
            calculate_legal_moves(&g_game, s, &g_moves);
            (*evals)++;
        }
        const int d = table_bot_drive(&g_table, NULL, 0, 1, &drv);
        if (d <= 0) return d < 0 ? d : 0;
        *actions += d;
        if (drv.stop == BOT_STOP_ENDED) return 1;
    }
    return 0;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(void) {
    const int games = env_int("BENCH_GAMES", 400);
    const int players = env_int("BENCH_PLAYERS", 4);
    const char *brain = getenv("BENCH_STRATEGY") && *getenv("BENCH_STRATEGY") ? getenv("BENCH_STRATEGY") : "random";
    long actions = 0, evals = 0;

    // Warmup, outside the timed window.
    for (int w = 0; w < 5; w++) {
        const int rc = play(brain, players, 1000000 + w, &actions, &evals);
        if (rc < 0) { fprintf(stderr, "bench_engine: the kernel refused the table (%d)\n", rc); return 1; }
    }
    actions = evals = 0;
    int finished = 0;
    const double t0 = now_s();
    for (int n = 0; n < games; n++) {
        const int rc = play(brain, players, n, &actions, &evals);
        if (rc < 0) { fprintf(stderr, "bench_engine: the kernel refused game %d (%d)\n", n, rc); return 1; }
        finished += rc;
    }
    const double dt = now_s() - t0;

    printf("engine bench: strategy=%s players=%d games=%d\n", brain, players, games);
    printf("  finished:    %d/%d\n", finished, games);
    printf("  wall:        %.2fs\n", dt);
    printf("  games/sec:   %.1f\n", games / dt);
    printf("  actions/sec: %.0f  (%ld total)\n", actions / dt, actions);
    printf("  legal-evals/sec: %.0f  (%ld total)\n", evals / dt, evals);
    return 0;
}
