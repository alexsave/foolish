// Two bot drives on two threads, each under its own pre-action hook.
//
// bot_drive_pre_action_hook is how a host seeds every decision of a drive
// (bot_drive.h). The native server runs one bot thread per game, and table.c
// and the wasm bridge install the hook around each drive and put the previous
// one back. With the hook a process-wide global, a second thread's install
// replaces the first thread's in the middle of its drive, and that drive's
// decisions are then seeded from the other game's base: a bot's move stops
// being a function of its own game. The hook is per thread for that reason,
// like engine_snap_hook (game.h) beside it.
//
// The test makes the race deterministic instead of hoping for it: both threads
// install their hook, meet at a rendezvous, and only then drive. A shared hook
// is by then the last install, so one of the two drives runs the other's hook
// on every decision.
//
// Built with real thread-local storage (make bot-drive-threads-test, the
// analyse variant's objects): the host variant compiles _Thread_local away, and
// there the test would be measuring nothing.
//
// Mutation-checked: drop _Thread_local from the hook's definition and
// declaration -> "drive N ran only its own hook" fails for one of the two
// drives (its foreign count is every decision it made).
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "bot_drive.h"
#include "game.h"
#include "strategy.h"

static int g_fail;
#define CHECK(c, msg) do { if (!(c)) { g_fail++; fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } } while (0)

enum { N_DRIVES = 2, MAX_CYCLES = 400 };

static _Thread_local int t_drive = -1;   // which drive this thread is running
static atomic_int g_own[N_DRIVES];       // hook i called on drive i's thread
static atomic_int g_foreign[N_DRIVES];   // hook i called on the OTHER drive's thread
static atomic_int g_installed;
static atomic_int g_main_saw_hook;

static void note(int hook, const Game *g, int seat, int phase) {
    (void)g; (void)seat; (void)phase;
    if (t_drive == hook) atomic_fetch_add(&g_own[hook], 1);
    else atomic_fetch_add(&g_foreign[hook], 1);
}
static void hook0(const Game *g, int seat, int phase) { note(0, g, seat, phase); }
static void hook1(const Game *g, int seat, int phase) { note(1, g, seat, phase); }
static void (*const k_hooks[N_DRIVES])(const Game *, int, int) = { hook0, hook1 };

static void make_game(Game *g, int n_players, int seed) {
    unsigned char s[32];
    for (int i = 0; i < 32; i++) s[i] = (unsigned char)(i * 29 + seed * 7 + n_players);
    game_set_seed((uint32_t)(seed + 1));
    random_strategy_set_seed((uint32_t)(seed + 1));
    game_set_deal_seed_bytes(s, 32);
    memset(g, 0, sizeof(*g));
    g->num_players = (int8_t)n_players;
    for (int i = 0; i < n_players; i++) g->players[i].status = PLAYER_STATUS_READY;
    start_game(g);
    for (int i = 0; i < n_players; i++) g->players[i].strategy_key = STRAT_HANDWRITTEN_PROD;
}

static void *drive_thread(void *arg) {
    t_drive = (int)(intptr_t)arg;
    Game g;
    make_game(&g, 3 + t_drive, 100 + t_drive);

    bot_drive_pre_action_hook = k_hooks[t_drive];
    atomic_fetch_add(&g_installed, 1);
    while (atomic_load(&g_installed) < N_DRIVES) { /* rendezvous: both hooks are in */ }

    BotDriveOut out;
    for (int c = 0; c < MAX_CYCLES; c++)
        if (bot_drive(&g, 0, BOT_DRIVE_MAX_ACTIONS, 0, 0, &out) <= 0 || out.ended >= 0) break;
    bot_drive_pre_action_hook = 0;
    return 0;
}

int main(void) {
    pthread_t th[N_DRIVES];
    for (int i = 0; i < N_DRIVES; i++)
        CHECK(pthread_create(&th[i], 0, drive_thread, (void *)(intptr_t)i) == 0, "a drive thread starts");
    while (atomic_load(&g_installed) < N_DRIVES) { }
    atomic_store(&g_main_saw_hook, bot_drive_pre_action_hook != 0);
    for (int i = 0; i < N_DRIVES; i++) pthread_join(th[i], 0);

    for (int i = 0; i < N_DRIVES; i++) {
        char msg[96];
        snprintf(msg, sizeof msg, "drive %d ran its own hook (%d calls)", i, atomic_load(&g_own[i]));
        CHECK(atomic_load(&g_own[i]) > 0, msg);
        snprintf(msg, sizeof msg, "drive %d ran only its own hook (%d calls from the other drive)",
                 i, atomic_load(&g_foreign[i]));
        CHECK(atomic_load(&g_foreign[i]) == 0, msg);
    }
    CHECK(!atomic_load(&g_main_saw_hook), "a thread that installed no hook sees none");

    if (g_fail) { fprintf(stderr, "bot_drive_threads_test: %d FAILED\n", g_fail); return 1; }
    printf("bot_drive_threads_test: ok (%d + %d hooked decisions)\n",
           atomic_load(&g_own[0]), atomic_load(&g_own[1]));
    return 0;
}
