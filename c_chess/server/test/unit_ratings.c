// unit_ratings.c - a rating per speed, through the state and the journal without a server: rated
// games of two speeds move only their own speed's rating and count, a casual game moves nothing, a
// game, a seek and a state push show the rating of their own speed, the leaderboard of each speed
// holds its players, and a restart (the journal read back) brings back every rating, the points of
// each speed (the latest of each when there are more) and a game in play with its speed.
#include "cc_server.h"
#include "journal.h"
#include "config.h"
#include "store/store.h"
#include "util/err.h"
#include "../../c/test/t.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int cc_state_start_clock(void);

static const char *ID_A = "0a000000-0000-4000-8000-00000000000a", *ID_B = "0b000000-0000-4000-8000-00000000000b";
static cb_ctx g_ctx;
static cb_config g_cfg;
static char g_dir[256];

static void open_state(void) {
    char err[256];
    CHECK(cc_state_init(&g_ctx) == 0, "state");
    CHECK(cc_journal_recover(&g_ctx, err, sizeof err) == CB_OK, "%s", err);
    CHECK(cc_journal_start(&g_ctx) == CB_OK, "journal");
}

static void close_state(void) {
    cc_journal_sync();
    cc_state_free();
    cc_journal_stop();
}

static ch_move uci(const cc_live *l, const char *m) { return ch_parse_uci(&l->g.pos, m, 4); }

// A rated (or casual) game of base+inc between w and b, two moves each side, then `loser` resigns:
// its id.
static uint32_t play(cc_player *w, cc_player *b, int base, int inc, int rated, int loser) {
    pthread_mutex_lock(&cc_lock);
    cw_seek tc = { (uint16_t)base, (uint8_t)inc, (uint8_t)rated };
    int64_t now = cc_mono();
    cc_live *l = cc_live_start(w, b, &tc, now);
    static const char *const line[] = { "e2e4", "e7e5", "g1f3", "b8c6" };
    for (int i = 0; i < 4; i++) {
        CHECK(cc_game_move(&l->g, i & 1, i, uci(l, line[i]), now) == CC_OK, "move %s", line[i]);
        cc_live_changed(l, now);
    }
    CHECK(cc_game_resign(&l->g, loser, now) == CC_OK, "resign");
    cc_live_changed(l, now);
    uint32_t id = l->id;
    pthread_mutex_unlock(&cc_lock);
    return id;
}

static g2_player A_R[CC_SPEEDS], B_R[CC_SPEEDS];

static void two_speeds(void) {
    pthread_mutex_lock(&cc_lock);
    cc_player *a = cc_player_of(ID_A, "alice"), *b = cc_player_of(ID_B, "bob");
    pthread_mutex_unlock(&cc_lock);
    TEST("a new player starts every speed at the start rating");
    for (int k = 0; k < CC_SPEEDS; k++)
        CHECK(a->r[k].rating == CC_RATING_START && a->r[k].deviation == CC_RD_START && !a->rated[k], "speed %d", k);

    TEST("a bullet game moves bullet alone");
    play(a, b, 60, 0, 1, CH_BLACK);   // alice (white) wins
    CHECK(a->r[CC_BULLET].rating > CC_RATING_START && b->r[CC_BULLET].rating < CC_RATING_START, "bullet %.1f %.1f", a->r[CC_BULLET].rating, b->r[CC_BULLET].rating);
    CHECK(a->rated[CC_BULLET] == 1 && b->rated[CC_BULLET] == 1, "rated %u %u", a->rated[CC_BULLET], b->rated[CC_BULLET]);
    for (int k = CC_BLITZ; k < CC_SPEEDS; k++)
        CHECK(a->r[k].rating == CC_RATING_START && a->r[k].deviation == CC_RD_START && !a->rated[k] && !b->rated[k], "speed %d moved", k);
    TEST("a blitz game moves blitz alone, from its own rating");
    g2_player bullet = a->r[CC_BULLET];
    pthread_mutex_lock(&cc_lock);
    cw_seek tc = { 180, 2, 1 };
    cc_live *l = cc_live_start(b, a, &tc, cc_mono());   // bob white
    CHECK(l->speed == CC_BLITZ, "speed %d", l->speed);
    CHECK(l->rating0[0] == 1500 && l->rating0[1] == 1500, "the game began at %d %d, not the blitz ratings", l->rating0[0], l->rating0[1]);
    cw_state s;
    cc_state_fill(l, &s, cc_mono());
    CHECK(s.rating[0] == 1500 && s.rating[1] == 1500, "the state push shows %d %d", s.rating[0], s.rating[1]);
    CHECK(cc_game_abort(&l->g, CH_WHITE, cc_mono()) == CC_OK, "abort");
    cc_live_changed(l, cc_mono());
    pthread_mutex_unlock(&cc_lock);
    CHECK(!a->rated[CC_BLITZ] && a->games == 1, "an aborted game counted: %u %u", a->rated[CC_BLITZ], a->games);
    play(b, a, 180, 2, 1, CH_BLACK);   // bob (white) wins the blitz game: he leads blitz, alice bullet
    CHECK(a->r[CC_BLITZ].rating < CC_RATING_START && b->r[CC_BLITZ].rating > CC_RATING_START, "blitz %.1f %.1f", a->r[CC_BLITZ].rating, b->r[CC_BLITZ].rating);
    CHECK(a->rated[CC_BLITZ] == 1 && b->rated[CC_BLITZ] == 1 && a->rated[CC_BULLET] == 1, "rated %u %u %u", a->rated[CC_BLITZ], b->rated[CC_BLITZ], a->rated[CC_BULLET]);
    CHECK(memcmp(&a->r[CC_BULLET], &bullet, sizeof bullet) == 0, "the blitz game moved bullet");
    play(a, b, 120, 1, 1, CH_BLACK);   // 2+1 is bullet too (after the blitz game: the points' order is by speed, not time)
    CHECK(a->rated[CC_BULLET] == 2 && a->rated[CC_BLITZ] == 1, "2+1: rated %u %u", a->rated[CC_BULLET], a->rated[CC_BLITZ]);

    TEST("a casual game moves no rating");
    g2_player before = a->r[CC_RAPID];
    play(a, b, 600, 0, 0, CH_BLACK);
    CHECK(!a->rated[CC_RAPID] && memcmp(&a->r[CC_RAPID], &before, sizeof before) == 0, "rapid moved");
    CHECK(a->games == 4 && a->wins == 3, "games %u wins %u", a->games, a->wins);

    TEST("a seek shows the rating of its speed");
    pthread_mutex_lock(&cc_lock);
    cc_live *none;
    cw_seek blitz = { 300, 3, 1 }, bul = { 60, 0, 1 };
    static cw_lobby lb;
    CHECK(cc_seek_add(a, &blitz, cc_mono(), &none) == CW_E_NONE && !none, "seek");
    cc_lobby_fill(&lb, cc_mono());
    CHECK(lb.n == 1 && lb.seeks[0].rating == cc_rating_int(&a->r[CC_BLITZ]), "blitz seek %d, rating %d", lb.seeks[0].rating, cc_rating_int(&a->r[CC_BLITZ]));
    CHECK(cc_seek_add(a, &bul, cc_mono(), &none) == CW_E_NONE && !none, "seek");
    cc_lobby_fill(&lb, cc_mono());
    CHECK(lb.n == 1 && lb.seeks[0].rating == cc_rating_int(&a->r[CC_BULLET]), "bullet seek %d, rating %d", lb.seeks[0].rating, cc_rating_int(&a->r[CC_BULLET]));
    cc_seek_cancel(a, cc_mono());
    pthread_mutex_unlock(&cc_lock);

    memcpy(A_R, a->r, sizeof A_R);
    memcpy(B_R, b->r, sizeof B_R);
}

// The leaderboards as they stand: alice above bob in bullet, bob above alice in blitz, nobody in
// rapid or classical.
static void leaderboards(void) {
    static cw_top top;
    pthread_mutex_lock(&cc_lock);
    TEST("a leaderboard per speed");
    int n = cc_top_fill(&top, CC_BULLET);
    CHECK(n == 8 + 2 * (int)sizeof(cw_top_row) && top.n == 2 && top.speed == CC_BULLET, "bullet: %d bytes, %u rows", n, top.n);
    CHECK(!strcmp(top.rows[0].name, "alice") && !strcmp(top.rows[1].name, "bob"), "bullet order %s %s", top.rows[0].name, top.rows[1].name);
    CHECK(top.rows[0].games == 2 && top.rows[0].rating == cc_rating_int(&A_R[CC_BULLET]), "bullet row %u %d", top.rows[0].games, top.rows[0].rating);
    CHECK(top.rows[0].rd == (uint16_t)(A_R[CC_BULLET].deviation + 0.5), "rd %u", top.rows[0].rd);
    n = cc_top_fill(&top, CC_BLITZ);
    CHECK(top.n == 2 && top.speed == CC_BLITZ && !strcmp(top.rows[0].name, "bob"), "blitz: %u rows, first %s", top.n, top.rows[0].name);
    CHECK(top.rows[1].games == 1 && top.rows[1].rating == cc_rating_int(&A_R[CC_BLITZ]), "blitz row %u %d", top.rows[1].games, top.rows[1].rating);
    n = cc_top_fill(&top, CC_RAPID);
    CHECK(n == 8 && top.n == 0 && top.speed == CC_RAPID, "rapid: a casual game made %u rows", top.n);
    n = cc_top_fill(&top, CC_CLASSICAL);
    CHECK(n == 8 && top.n == 0, "classical: %u rows", top.n);
    pthread_mutex_unlock(&cc_lock);
}

static uint32_t g_playing;

static void restart(void) {
    // a blitz game in play across the restart
    pthread_mutex_lock(&cc_lock);
    cw_seek tc = { 300, 3, 1 };
    cc_live *l = cc_live_start(cc_player_get(ID_A), cc_player_get(ID_B), &tc, cc_mono());
    CHECK(cc_game_move(&l->g, CH_WHITE, 0, uci(l, "d2d4"), cc_mono()) == CC_OK, "move");
    cc_journal_move(l);
    cc_live_changed(l, cc_mono());
    g_playing = l->id;
    pthread_mutex_unlock(&cc_lock);
    close_state();
    open_state();

    TEST("a restart brings back every rating");
    pthread_mutex_lock(&cc_lock);
    cc_player *a = cc_player_get(ID_A), *b = cc_player_get(ID_B);
    CHECK(a && b, "players");
    for (int k = 0; a && b && k < CC_SPEEDS; k++) {
        CHECK(memcmp(&a->r[k], &A_R[k], sizeof A_R[k]) == 0, "alice speed %d: %.4f, was %.4f", k, a->r[k].rating, A_R[k].rating);
        CHECK(memcmp(&b->r[k], &B_R[k], sizeof B_R[k]) == 0, "bob speed %d: %.4f, was %.4f", k, b->r[k].rating, B_R[k].rating);
    }
    CHECK(a && a->rated[CC_BULLET] == 2 && a->rated[CC_BLITZ] == 1 && !a->rated[CC_RAPID] && !a->rated[CC_CLASSICAL], "rated counts");
    CHECK(a && a->games == 4 && a->wins == 3, "games");
    cc_live *g = cc_live_get(g_playing);
    CHECK(g && g->speed == CC_BLITZ && g->g.ply == 1, "the game in play: speed %d", g ? g->speed : -1);
    pthread_mutex_unlock(&cc_lock);
    leaderboards();

    TEST("a restart brings back the points of each speed");
    cw_point pts[CW_POINTS_MAX];
    int n = cc_db_points_of(g_ctx.store, a, pts, CW_POINTS_SPEED);
    CHECK(n == 3, "%d points", n);
    CHECK(n == 3 && pts[0].speed == CC_BULLET && pts[1].speed == CC_BULLET && pts[2].speed == CC_BLITZ, "speeds %d %d %d", pts[0].speed, pts[1].speed, pts[2].speed);
    CHECK(n == 3 && pts[0].at <= pts[1].at, "bullet not oldest first");
    CHECK(n == 3 && pts[1].rating == cc_rating_int(&A_R[CC_BULLET]) && pts[2].rating == cc_rating_int(&A_R[CC_BLITZ]), "ratings %d %d", pts[1].rating, pts[2].rating);
    CHECK(n == 3 && pts[0].rating > 1500 && pts[0].rating < pts[1].rating, "the first bullet point %d", pts[0].rating);
    TEST("the latest of each speed, past the cap");
    n = cc_db_points_of(g_ctx.store, a, pts, 1);
    CHECK(n == 2 && pts[0].speed == CC_BULLET && pts[1].speed == CC_BLITZ, "%d points, speeds %d %d", n, pts[0].speed, pts[1].speed);
    CHECK(n == 2 && pts[0].rating == cc_rating_int(&A_R[CC_BULLET]), "not the latest bullet point: %d", pts[0].rating);
}

int main(void) {
    ch_init();
    snprintf(g_dir, sizeof g_dir, "/tmp/unit_ratings_%d", (int)getpid());
    char cmd[300], db[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", g_dir, g_dir);
    if (system(cmd) != 0) return 1;
    snprintf(db, sizeof db, "%s/c.sqlite", g_dir);
    snprintf(g_cfg.db_path, sizeof g_cfg.db_path, "%s", db);
    g_ctx.cfg = &g_cfg;
    if (cb_store_open_sqlite(db, &g_ctx.store) != CB_OK) { fprintf(stderr, "no store\n"); return 1; }
    open_state();
    two_speeds();
    leaderboards();
    restart();
    close_state();
    cb_store_close(g_ctx.store);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) return 1;
    return report("unit_ratings");
}
