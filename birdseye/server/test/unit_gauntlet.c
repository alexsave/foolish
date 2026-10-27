// unit_gauntlet.c - the gauntlet (gauntlet.h): the ladder each run is drawn with (each brain's best
// bot, weakest first), who sits first, a rung won climbs, a rung lost ends the run, draws and the
// game cap, the best run across runs, the titles, and the file read back by row id after a restart.
#include "gauntlet.h"
#include "t.h"
#include "tournament.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// 7 brains x 3 bots, then the human (row 21). Row 3b + k is brain b's k-th bot.
#define N     22
#define HUMAN 21

static int32_t g_rating[N];
static const char *g_key[N];

// Brain b's bots rated base[b] + 10k for k = 0..2 (so its best is row 3b + 2), with a twist: brain
// "octogen"'s best is its FIRST row (a tie on rating goes to the lower row), and handwritten is
// rated below random.
static void field(void) {
    static const int base[7] = { 1100, 1400, 1300, 1500, 1600, 1700, 1000 };
    for (int b = 0; b < 7; b++)
        for (int k = 0; k < 3; k++) {
            g_key[3 * b + k] = BE_GAUNT_BRAINS[b];
            g_rating[3 * b + k] = base[b] + 10 * k;
        }
    g_rating[3 * 4 + 0] = g_rating[3 * 4 + 2];   // octogen: rows 12 and 14 tie at the top
    g_key[HUMAN] = "human";
    g_rating[HUMAN] = 9999;                        // the human is never on the ladder, however rated
}

static void ladder(void) {
    field();
    be_w_gauntlet g;
    be_gaunt_init(&g, HUMAN);
    T_EQ_INT(g.state, BE_GAUNT_IDLE);
    T_CHECK(be_gaunt_new_run(&g, g_rating, g_key, N, HUMAN, 77));
    T_EQ_INT(g.state, BE_GAUNT_RUNNING);
    T_EQ_INT(g.run, 1);
    T_EQ_INT(g.rung, 0);
    T_EQ_INT(g.best_rung, 1);
    T_EQ_INT(g.best_won, 0);
    // Weakest first: handwritten 1020, random 1120, robusta 1320, firecracker 1420, blackpowder 1520,
    // octogen 1620 (row 12, the lower of the tie), cordite 1720.
    static const int want[7] = { 3 * 6 + 2, 3 * 0 + 2, 3 * 2 + 2, 3 * 1 + 2, 3 * 3 + 2, 3 * 4 + 0, 3 * 5 + 2 };
    for (int r = 0; r < 7; r++) {
        T_EQ_INT(g.rungs[r].bot, want[r]);
        T_EQ_INT(g.rungs[r].rating, g_rating[want[r]]);
        T_EQ_INT(g.rungs[r].games, 0);
        T_EQ_INT(g.rungs[r].live, 0);
    }
    for (int r = 1; r < 7; r++) T_CHECK(g.rungs[r].rating >= g.rungs[r - 1].rating);
    // A tie between brains keeps the brains' order: random and firecracker level, random first.
    g_rating[3 * 1 + 0] = g_rating[3 * 1 + 1] = 1000;
    g_rating[3 * 1 + 2] = g_rating[3 * 0 + 2];
    T_CHECK(be_gaunt_new_run(&g, g_rating, g_key, N, HUMAN, 78));
    T_EQ_INT(g.run, 2);
    T_EQ_INT(g.rungs[1].bot, 3 * 0 + 2);
    T_EQ_INT(g.rungs[2].bot, 3 * 1 + 2);
    // A brain with no bot: no ladder, the record untouched.
    field();
    g_key[3 * 5 + 0] = g_key[3 * 5 + 1] = g_key[3 * 5 + 2] = "other";
    be_w_gauntlet before = g;
    T_CHECK(!be_gaunt_new_run(&g, g_rating, g_key, N, HUMAN, 79));
    T_CHECK(memcmp(&before, &g, sizeof g) == 0);
}

// Plays the current rung's next game: the human wins (the bot is the fool), loses, or draws.
static void play(be_w_gauntlet *g, char how) {
    uint8_t seats[2];
    int r = be_gaunt_next(g, seats);
    T_EQ_INT(r, g->rung);
    if (r < 0) return;
    uint8_t bot = g->rungs[r].bot;
    // The human sits first in the rung's odd games.
    T_EQ_INT(seats[0], g->rungs[r].games % 2 == 0 ? HUMAN : bot);
    T_EQ_INT(seats[1], g->rungs[r].games % 2 == 0 ? bot : HUMAN);
    be_gaunt_started(g, "a1b2c3", 6, 100);
    T_EQ_INT(g->rungs[r].live, 1);
    T_EQ_INT(be_gaunt_next(g, seats), -1);   // one game of a rung at a time
    T_CHECK(be_gaunt_result(g, true, how == 'w' ? bot : how == 'l' ? HUMAN : BE_TOUR_NONE, 200));
}

static void climb_and_fall(void) {
    field();
    be_w_gauntlet g;
    be_gaunt_init(&g, HUMAN);
    T_CHECK(be_gaunt_new_run(&g, g_rating, g_key, N, HUMAN, 1));
    // Rung 1: 3-2 with a draw between; the results are kept game by game.
    play(&g, 'w');
    play(&g, 'l');
    play(&g, 'd');
    play(&g, 'l');
    play(&g, 'w');
    T_EQ_INT(g.rung, 0);
    play(&g, 'w');
    T_EQ_INT(g.rungs[0].games, 6);
    T_EQ_INT(g.rungs[0].wins_h, 3);
    T_EQ_INT(g.rungs[0].wins_b, 2);
    static const uint8_t res[6] = { BE_GAUNT_G_HUMAN, BE_GAUNT_G_BOT, BE_GAUNT_G_DRAW, BE_GAUNT_G_BOT, BE_GAUNT_G_HUMAN, BE_GAUNT_G_HUMAN };
    for (int i = 0; i < 6; i++) T_EQ_INT(g.rungs[0].result[i], res[i]);
    T_CHECK(g.rungs[0].done_ms == 200);
    T_EQ_INT(g.rung, 1);   // climbed
    T_EQ_INT(g.state, BE_GAUNT_RUNNING);
    T_EQ_INT(g.best_rung, 2);
    T_EQ_INT(g.best_won, 1);
    T_EQ_INT(g.games, 6);
    // A game given up before it was played: the rung asks again, nothing counted.
    uint8_t seats[2];
    T_EQ_INT(be_gaunt_next(&g, seats), 1);
    be_gaunt_started(&g, "d4e5f6", 6, 300);
    T_CHECK(be_gaunt_result(&g, false, BE_TOUR_NONE, 300));
    T_EQ_INT(g.rungs[1].games, 0);
    T_EQ_INT(g.rungs[1].live, 0);
    // Rung 2 lost 0-3: the run is over there.
    play(&g, 'l');
    play(&g, 'l');
    play(&g, 'l');
    T_EQ_INT(g.state, BE_GAUNT_LOST);
    T_EQ_INT(g.rung, 1);
    T_EQ_INT(be_gaunt_next(&g, seats), -1);
    T_CHECK(!be_gaunt_result(&g, true, HUMAN, 400));   // nothing more is counted
    T_EQ_INT(g.best_rung, 2);
    T_EQ_INT(g.best_won, 1);
    T_EQ_INT(g.best_run, 1);
    // A new run starts at rung 1; the best run stays until beaten.
    T_CHECK(be_gaunt_new_run(&g, g_rating, g_key, N, HUMAN, 500));
    T_EQ_INT(g.run, 2);
    T_EQ_INT(g.rung, 0);
    T_EQ_INT(g.games, 0);
    T_EQ_INT(g.state, BE_GAUNT_RUNNING);
    T_EQ_INT(g.rungs[0].games, 0);
    T_EQ_INT(g.best_rung, 2);
    T_EQ_INT(g.best_run, 1);
    play(&g, 'l');
    play(&g, 'l');
    play(&g, 'l');
    T_EQ_INT(g.state, BE_GAUNT_LOST);
    T_EQ_INT(g.best_rung, 2);   // rung 1 does not beat rung 2
    T_EQ_INT(g.best_run, 1);
    // Run 3 climbs all seven: the ladder is won.
    T_CHECK(be_gaunt_new_run(&g, g_rating, g_key, N, HUMAN, 600));
    for (int r = 0; r < 7; r++) {
        play(&g, 'w');
        play(&g, 'w');
        play(&g, 'w');
    }
    T_EQ_INT(g.state, BE_GAUNT_WON);
    T_EQ_INT(g.rung, 7);
    T_EQ_INT(g.best_rung, 7);
    T_EQ_INT(g.best_won, 7);
    T_EQ_INT(g.best_run, 3);
    T_EQ_INT(be_gaunt_next(&g, seats), -1);
}

// Draws: a rung drawn BE_GAUNT_GAMES times goes to the one ahead, else to the bot.
static void cap(void) {
    field();
    be_w_gauntlet g;
    be_gaunt_init(&g, HUMAN);
    T_CHECK(be_gaunt_new_run(&g, g_rating, g_key, N, HUMAN, 1));
    play(&g, 'w');
    for (int i = 0; i < BE_GAUNT_GAMES - 1; i++) play(&g, 'd');
    T_EQ_INT(g.rungs[0].games, BE_GAUNT_GAMES);
    T_EQ_INT(g.rung, 1);   // 1-0 after nine: his
    T_CHECK(be_gaunt_new_run(&g, g_rating, g_key, N, HUMAN, 2));
    for (int i = 0; i < BE_GAUNT_GAMES; i++) play(&g, 'd');
    T_EQ_INT(g.state, BE_GAUNT_LOST);   // level after nine: the bot's
}

static void titles(void) {
    field();
    be_w_gauntlet g;
    be_gaunt_init(&g, HUMAN);
    T_CHECK(be_gaunt_new_run(&g, g_rating, g_key, N, HUMAN, 1));
    char t[96];
    be_gaunt_game_title(&g, "Madrid 2", 8, t, sizeof t);
    T_EQ_STR(t, "Gauntlet 1/7: Madrid 2 - Game 1 of 5");
    play(&g, 'w');
    g.rung = 5;
    be_gaunt_game_title(&g, "St. Petersburg 7", 16, t, sizeof t);
    T_EQ_STR(t, "Gauntlet 6/7: St. Petersburg 7 - Game 1 of 5");   // the longest bot name fits
    T_CHECK(strlen(t) <= BE_GAUNT_TITLE_MAX);
    g.rungs[5].games = 6;
    be_gaunt_game_title(&g, "St. Petersburg 7", 16, t, sizeof t);
    T_EQ_STR(t, "Gauntlet 6/7: St. Petersburg 7 - Game 7");
    // A name that would pass the lobby's 50 characters is left out.
    be_gaunt_game_title(&g, "An Extremely Long Bot Name Indeed", 33, t, sizeof t);
    T_EQ_STR(t, "Gauntlet 6/7 - Game 7");
}

// The file: a run in the middle of a rung, read back after a "restart" whose list has the rows in
// another order.
static void restart(void) {
    field();
    be_w_gauntlet g;
    be_gaunt_init(&g, HUMAN);
    T_CHECK(be_gaunt_new_run(&g, g_rating, g_key, N, HUMAN, 1));
    play(&g, 'w');
    play(&g, 'w');
    play(&g, 'w');
    play(&g, 'l');
    be_gaunt_started(&g, "c0ffee", 6, 900);
    char ids[N][16];
    const char *idp[N], *rev[N];
    for (int i = 0; i < N; i++) {
        snprintf(ids[i], sizeof ids[i], "id-%02d", i);
        idp[i] = ids[i];
        rev[N - 1 - i] = ids[i];
    }
    char path[] = "/tmp/unit_gauntlet_XXXXXX";
    int fd = mkstemp(path);
    T_CHECK(fd >= 0);
    close(fd);
    T_CHECK(be_gaunt_save(&g, path, idp, N));
    be_w_gauntlet back;
    T_CHECK(be_gaunt_load(&back, path, rev, N));
    T_EQ_INT(back.state, BE_GAUNT_RUNNING);
    T_EQ_INT(back.run, 1);
    T_EQ_INT(back.rung, 1);
    T_EQ_INT(back.human, N - 1 - HUMAN);
    T_EQ_INT(back.best_rung, 2);
    for (int r = 0; r < 7; r++) {
        T_EQ_INT(back.rungs[r].bot, N - 1 - g.rungs[r].bot);
        T_EQ_INT(back.rungs[r].rating, g.rungs[r].rating);
        T_EQ_INT(back.rungs[r].games, g.rungs[r].games);
        T_EQ_INT(back.rungs[r].wins_h, g.rungs[r].wins_h);
        T_EQ_INT(back.rungs[r].wins_b, g.rungs[r].wins_b);
    }
    T_EQ_INT(back.rungs[1].live, 1);
    T_CHECK(back.rungs[1].gid_len == 6 && memcmp(back.rungs[1].gid, "c0ffee", 6) == 0);
    T_EQ_INT(back.rungs[1].result[0], BE_GAUNT_G_BOT);
    // A list that lacks a row the file names: not read.
    const char *fewer[N - 1];
    for (int i = 0; i < N - 1; i++) fewer[i] = idp[i + 1];
    T_CHECK(!be_gaunt_load(&back, path, fewer, N - 1));
    unlink(path);
    T_CHECK(!be_gaunt_load(&back, path, idp, N));   // none
}

int main(void) {
    ladder();
    climb_and_fall();
    cap();
    titles();
    restart();
    return t_done("unit_gauntlet");
}
