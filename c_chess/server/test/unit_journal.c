// unit_journal.c - the moves file across restarts: a game in play comes back with every move and its
// clocks after one restart, after a second (whose moves file is the first's rewrite, where only each
// side's last move carries a time), and with a move played between them.
#include "cc_server.h"
#include "journal.h"
#include "arenas.h"
#include "config.h"
#include "store/store.h"
#include "util/err.h"
#include "../../c/test/t.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static cb_ctx g_ctx;
static cb_config g_cfg;
static char g_dir[256];

static void open_state(void) {
    char err[256];
    CHECK(cc_state_init(&g_ctx) == 0, "state");
    CHECK(cc_journal_recover(&g_ctx, err, sizeof err) == CB_OK, "%s", err);
    CHECK(cc_journal_start(&g_ctx) == CB_OK, "journal");
    cc_arena_schedule(false);
}

static void close_state(void) {
    cc_journal_sync();
    cc_state_free();
    cc_journal_stop();
}

static const char *const LINE[] = { "e2e4", "e7e5", "g1f3", "b8c6", "f1b5", "a7a6", "b5a4", "g8f6", "e1g1", "f8e7", "f1e1" };
static uint32_t g_game;
static int64_t g_left[2];

// Plays LINE[from..to) in the game, a little time apart, through the journal as the move route does.
static void play(int from, int to) {
    pthread_mutex_lock(&cc_lock);
    cc_live *l = cc_live_get(g_game);
    int64_t now = cc_mono();
    for (int i = from; l && i < to; i++) {
        now += 700;
        ch_move m = ch_parse_uci(&l->g.pos, LINE[i], 4);
        CHECK(cc_game_move(&l->g, l->g.pos.stm, l->g.ply, m, now) == CC_OK, "move %s", LINE[i]);
        cc_journal_move(l);
        cc_live_changed(l, now);
    }
    if (l) { g_left[0] = l->g.left_ms[0]; g_left[1] = l->g.left_ms[1]; }
    pthread_mutex_unlock(&cc_lock);
}

// After a restart: the game in play at ply, its clocks as they were.
static void back(int ply, const char *when) {
    pthread_mutex_lock(&cc_lock);
    cc_live *l = cc_live_get(g_game);
    CHECK(l != NULL, "%s: the game is gone", when);
    if (l) {
        CHECK(l->g.ply == ply && l->g.status == CC_PLAYING, "%s: ply %d, status %d", when, l->g.ply, l->g.status);
        CHECK(l->g.left_ms[0] == g_left[0] && l->g.left_ms[1] == g_left[1], "%s: clocks %lld %lld, were %lld %lld", when,
              (long long)l->g.left_ms[0], (long long)l->g.left_ms[1], (long long)g_left[0], (long long)g_left[1]);
    }
    pthread_mutex_unlock(&cc_lock);
}

int main(void) {
    ch_init();
    snprintf(g_dir, sizeof g_dir, "/tmp/unit_journal_%d", (int)getpid());
    char cmd[300], db[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", g_dir, g_dir);
    if (system(cmd) != 0) return 1;
    snprintf(db, sizeof db, "%s/c.sqlite", g_dir);
    snprintf(g_cfg.db_path, sizeof g_cfg.db_path, "%s", db);
    g_ctx.cfg = &g_cfg;
    if (cb_store_open_sqlite(db, &g_ctx.store) != CB_OK) { fprintf(stderr, "no store\n"); return 1; }
    open_state();
    pthread_mutex_lock(&cc_lock);
    cc_player *w = cc_player_of("01000000-0000-4000-8000-000000000001", "ann");
    cc_player *b = cc_player_of("02000000-0000-4000-8000-000000000002", "ben");
    cw_seek tc = { 180, 2, 1 };
    cc_live *l = cc_live_start(w, b, &tc, cc_mono());
    g_game = l->id;
    pthread_mutex_unlock(&cc_lock);
    play(0, 10);

    TEST("one restart: every move and the clocks");
    close_state();
    open_state();
    back(10, "the first restart");
    TEST("a second restart, from the first's rewrite of the moves file");
    close_state();
    open_state();
    back(10, "the second restart");
    TEST("a move between restarts is kept too; a record the rules refuse changes nothing");
    play(10, 11);
    close_state();
    {   // an illegal move at the next ply, as a torn or foreign tail would leave (journal.c's 12-byte record)
        struct { uint32_t game; uint16_t ply, move; int32_t left_ms; } r = { g_game, 11, 0, 5000 };
        char path[320];
        snprintf(path, sizeof path, "%s-moves", db);
        FILE *f = fopen(path, "ab");
        CHECK(f && fwrite(&r, sizeof r, 1, f) == 1, "%s", "append");
        if (f) fclose(f);
    }
    open_state();
    back(11, "the third restart");
    close_state();
    open_state();
    back(11, "the fourth restart");

    close_state();
    cb_store_close(g_ctx.store);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) return 1;
    return report("unit_journal");
}
