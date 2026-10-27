// unit_arenas.c - arenas through the state and the journal without a server: the pairing rule
// (adjacent by rating, the busy and the resting skipped, no rematch while others are free, colors
// balanced), points and the streak through the finish of real games, the 20-ply draw rule, a
// first-move abort withdrawing the absent, withdrawing and coming back, the refusals, the end (no
// pairing after it, a game in play still counts), the hourly schedule, and a restart from the
// journal that brings back the arenas, their players and their games in play.
#include "cc_server.h"
#include "arenas.h"
#include "journal.h"
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

#define NP 7
static const char *const NAMES[NP] = { "ann", "ben", "cat", "dan", "eve", "fay", "guest-zzz" };
static const int RATINGS[NP] = { 1900, 1800, 1700, 1600, 1500, 1400, 1300 };
static cc_player *P[NP];
static char IDS[NP][CB_UUID_LEN + 1];

static void players(void) {
    pthread_mutex_lock(&cc_lock);
    for (int i = 0; i < NP; i++) {
        snprintf(IDS[i], sizeof IDS[i], "0%d000000-0000-4000-8000-00000000000%d", i, i);
        P[i] = cc_player_of(IDS[i], NAMES[i]);
        P[i]->r[CC_BLITZ].rating = RATINGS[i];
        P[i]->r[CC_BLITZ].deviation = 60;
    }
    pthread_mutex_unlock(&cc_lock);
}

static void reload_players(void) {
    for (int i = 0; i < NP; i++) P[i] = cc_player_get(IDS[i]);
}

static cc_arena_player *ap_of(cc_arena *a, const cc_player *p) {
    for (uint32_t i = 0; i < a->n; i++) if (a->pl[i].p == p) return &a->pl[i];
    return NULL;
}

static cc_live *game_of(cc_arena *a, const cc_player *p) {
    cc_arena_player *ap = ap_of(a, p);
    return ap && ap->game ? cc_live_get(ap->game) : NULL;
}

static const cc_player *opponent(cc_arena *a, const cc_player *p) {
    cc_live *l = game_of(a, p);
    return !l ? NULL : l->p[0] == p ? l->p[1] : l->p[0];
}

static const char *const LINE[] = { "g1f3", "g8f6", "b1c3", "b8c6", "e2e3", "e7e6", "d2d3", "d7d6", "f1e2", "f8e7",
                                     "c1d2", "c8d7", "a2a3", "a7a6", "h2h3", "h7h6", "a1b1", "a8b8", "h1h2", "h8h7" };

// Plays n plies of LINE in l, at now.
static void plies(cc_live *l, int n, int64_t now) {
    for (int i = 0; i < n; i++) {
        ch_move m = ch_parse_uci(&l->g.pos, LINE[i], 4);
        CHECK(cc_game_move(&l->g, l->g.pos.stm, l->g.ply, m, now) == CC_OK, "move %s", LINE[i]);
        cc_live_changed(l, now);
    }
}

// Ends p's game: p wins (by the other's resignation), or it is drawn by agreement after n plies.
static void win(cc_arena *a, const cc_player *p, int64_t now) {
    cc_live *l = game_of(a, p);
    CHECK(l != NULL, "%s has no game", p->name);
    if (!l) return;
    plies(l, 2, now);
    CHECK(cc_game_resign(&l->g, cc_color_of(l, p) ^ 1, now) == CC_OK, "resign");
    cc_live_changed(l, now);
}

static void draw(cc_arena *a, const cc_player *p, int n, int64_t now) {
    cc_live *l = game_of(a, p);
    CHECK(l != NULL, "%s has no game", p->name);
    if (!l) return;
    plies(l, n, now);
    CHECK(cc_game_draw(&l->g, CH_WHITE, now) == CC_OK && cc_game_draw(&l->g, CH_BLACK, now) == CC_OK, "draw");
    cc_live_changed(l, now);
}

static int64_t T0, W0;   // the test's mono and wall origins
static uint32_t g_arena, g_playing_game;

static cc_arena *make(int64_t wall, uint32_t start_in, uint32_t dur, const char *name) {
    cw_arena_req q;
    memset(&q, 0, sizeof q);
    q.base_s = 180;
    q.rated = 1;
    q.start_in_s = start_in;
    q.duration_s = dur;
    memcpy(q.name, name, strlen(name));
    cc_arena *a = NULL;
    CHECK(cc_arena_create(P[0], &q, wall, &a) == CW_E_NONE && a, "create %s", name);
    return a;
}

static void pairing(void) {
    pthread_mutex_lock(&cc_lock);
    T0 = cc_mono();
    W0 = 1800000000000;   // a wall clock of the test's own
    cc_arena *a = make(W0, 60, 600, "Blitz Test");
    g_arena = a->id;
    TEST("an upcoming arena takes players and pairs nobody");
    static const int JOIN[5] = { 3, 0, 4, 2, 1 };   // not in rating order: the pairing sorts
    for (int i = 0; i < 5; i++) CHECK(cc_arena_join(a, P[JOIN[i]]) == CW_E_NONE, "join %s", NAMES[JOIN[i]]);
    CHECK(cc_arena_join(a, P[0]) == CW_E_ARENA_JOINED, "joined twice");
    CHECK(cc_arena_join(a, P[6]) == CW_E_NONE, "a guest may play an arena");
    CHECK(cc_arena_withdraw(a, P[6]) == CW_E_NONE && a->n == 5 && !ap_of(a, P[6]), "leaving before the start: gone");
    CHECK(cc_arena_withdraw(a, P[6]) == CW_E_ARENA_NOT_JOINED, "left twice");
    cc_arena_tick(T0, W0 + 59000);
    CHECK(a->status == CC_ARENA_UPCOMING && !a->games, "status %d games %u", a->status, a->games);

    TEST("a busy player is skipped");
    cw_seek tc = { 300, 0, 0 };
    cc_live *busy = cc_live_start(P[2], P[5], &tc, T0);   // cat plays fay outside the arena
    CHECK(P[2]->playing == 1, "playing %u", P[2]->playing);
    TEST("at its start it pairs neighbours by rating");
    cc_arena_tick(T0, W0 + 60000);
    CHECK(a->status == CC_ARENA_RUNNING, "status %d", a->status);
    CHECK(a->games == 2 && a->playing == 4, "games %u playing %u", a->games, a->playing);
    CHECK(opponent(a, P[0]) == P[1] && opponent(a, P[3]) == P[4], "ann-%s, dan-%s",
          opponent(a, P[0]) ? opponent(a, P[0])->name : "-", opponent(a, P[3]) ? opponent(a, P[3])->name : "-");
    CHECK(!ap_of(a, P[2])->game, "cat was busy");
    cc_live *l = game_of(a, P[0]);
    CHECK(l && l->arena == a->id && l->rated && l->speed == CC_BLITZ, "the game's arena %u", l ? l->arena : 0);
    TEST("a pairing pass when nobody is free pairs nothing");
    cc_arena_tick(T0 + 1000, W0 + 61000);
    CHECK(a->games == 2, "games %u", a->games);
    CHECK(cc_game_resign(&busy->g, CH_BLACK, T0) == CC_OK, "resign");
    cc_live_changed(busy, T0);
    CHECK(!P[2]->playing, "cat still playing");
    cc_arena_tick(T0 + 2000, W0 + 62000);
    CHECK(a->games == 2 && !ap_of(a, P[2])->game, "cat was paired alone");

    TEST("a win scores 2, a loss 0, through the game's finish");
    win(a, P[0], T0 + 3000);   // ann beats ben
    win(a, P[3], T0 + 3000);   // dan beats eve
    cc_arena_player *ann = ap_of(a, P[0]), *ben = ap_of(a, P[1]), *dan = ap_of(a, P[3]);
    CHECK(ann->score == 2 && ann->games == 1 && ann->streak == 1, "ann %u/%u/%u", ann->score, ann->games, ann->streak);
    CHECK(ben->score == 0 && ben->games == 1 && ben->streak == 0, "ben %u/%u", ben->score, ben->games);
    CHECK(dan->score == 2 && !dan->game && a->playing == 0, "dan %u, playing %u", dan->score, a->playing);
    CHECK(P[0]->rated[CC_BLITZ] == 1, "the arena's game was not rated");
    TEST("the standings: a tie in score goes to the higher rated");
    static uint8_t first[CW_ARENA_FRAME_MAX];
    cc_arena_fill(a, NULL, 0, first);
    static const char *const order[5] = { "ann", "dan", "ben", "cat", "eve" };   // they joined dan, ann, eve, cat, ben
    for (int i = 0; i < 5; i++) {
        cw_arena_row r;
        memcpy(&r, first + sizeof(cw_arena_hdr) + (size_t)i * sizeof r, sizeof r);
        CHECK(!strcmp(r.name, order[i]), "place %d: %s, want %s", i + 1, r.name, order[i]);
    }
    TEST("a player rests after a game, then is paired, never with the last opponent while others are free");
    cc_arena_tick(T0 + 4000, W0 + 64000);
    CHECK(a->games == 2, "paired while resting: %u", a->games);
    cc_arena_tick(T0 + 3000 + CC_ARENA_REST_MS, W0 + 66000);
    CHECK(a->games == 4, "games %u", a->games);
    // five free: eve waits (one game, finished with the rest, rated lowest); ann-ben was the last, so ann meets cat; ben dan
    CHECK(opponent(a, P[0]) == P[2], "ann met %s", opponent(a, P[0]) ? opponent(a, P[0])->name : "-");
    CHECK(opponent(a, P[1]) == P[3] || opponent(a, P[1]) == P[4], "ben met %s", opponent(a, P[1]) ? opponent(a, P[1])->name : "-");
    pthread_mutex_unlock(&cc_lock);
}

// Runs games until every player in the arena has played `rounds` more, resting between.
static int64_t g_now;

static void streaks_and_draws(void) {
    pthread_mutex_lock(&cc_lock);
    cc_arena *a = cc_arena_get(g_arena);
    g_now = T0 + 3000 + CC_ARENA_REST_MS;
    TEST("on fire: the third win in a row scores 4");
    win(a, P[0], g_now);   // ann's second
    cc_arena_player *ann = ap_of(a, P[0]);
    CHECK(ann->score == 4 && ann->streak == 2, "ann %u streak %u", ann->score, ann->streak);
    // end every other game: whoever is lower in the list loses
    for (int i = 1; i < 5; i++) if (game_of(a, P[i])) win(a, P[i], g_now);
    g_now += CC_ARENA_REST_MS;
    cc_arena_tick(g_now, W0 + 70000);
    CHECK(game_of(a, P[0]) != NULL, "ann was not paired");
    win(a, P[0], g_now);
    CHECK(ann->score == 8 && ann->streak == 3, "ann %u streak %u", ann->score, ann->streak);
    for (int i = 1; i < 5; i++) if (game_of(a, P[i])) win(a, P[i], g_now);

    TEST("a draw on fire scores 2 and ends the streak");
    g_now += CC_ARENA_REST_MS;
    cc_arena_tick(g_now, W0 + 75000);
    const cc_player *o = opponent(a, P[0]);
    cc_arena_player *op = o ? ap_of(a, o) : NULL;
    uint16_t o_score = op ? op->score : 0;
    int o_gets = op && cc_arena_on_fire(op->streak) ? 2 : 1;
    draw(a, P[0], 20, g_now);
    CHECK(ann->score == 10 && ann->streak == 0, "ann %u streak %u", ann->score, ann->streak);
    CHECK(op && op->score == o_score + o_gets, "the other side %u, was %u", op ? op->score : 0, o_score);
    for (int i = 1; i < 5; i++) if (game_of(a, P[i])) win(a, P[i], g_now);

    TEST("a draw before 20 plies scores nothing");
    g_now += CC_ARENA_REST_MS;
    cc_arena_tick(g_now, W0 + 80000);
    int k = 0;
    while (k < 5 && !game_of(a, P[k])) k++;
    CHECK(k < 5, "nobody was paired");
    if (k < 5) {
        cc_arena_player *x = ap_of(a, P[k]);
        o = opponent(a, P[k]);
        op = ap_of(a, o);
        uint16_t x_score = x->score, x_games = x->games;
        o_score = op->score;
        draw(a, P[k], 19, g_now);
        CHECK(x->score == x_score && x->games == x_games + 1 && x->streak == 0, "%s %u games %u", NAMES[k], x->score, x->games);
        CHECK(op->score == o_score && op->streak == 0, "the other side %u, was %u", op->score, o_score);
    }
    for (int i = 0; i < 5; i++) if (game_of(a, P[i])) win(a, P[i], g_now);

    TEST("white goes to whoever has had it less often, so colors stay level");
    for (int r = 0; r < 6; r++) {
        g_now += CC_ARENA_REST_MS;
        int d[5];
        for (int i = 0; i < 5; i++) d[i] = ap_of(a, P[i])->white - ap_of(a, P[i])->black;
        cc_arena_tick(g_now, W0 + 81000 + r * 4000);
        for (int i = 0; i < 5; i++) {
            cc_live *g = game_of(a, P[i]);
            if (!g || g->p[0] != P[i]) continue;
            int j = 0;
            while (j < 5 && P[j] != g->p[1]) j++;
            CHECK(j < 5 && d[i] <= d[j], "round %d: %s (%+d) white against %s (%+d)", r, NAMES[i], d[i], j < 5 ? NAMES[j] : "?", j < 5 ? d[j] : 0);
        }
        for (int i = 0; i < 5; i++) if (game_of(a, P[i])) win(a, P[i], g_now);
    }
    for (int i = 0; i < 5; i++) {
        cc_arena_player *x = ap_of(a, P[i]);
        int dd = x->white - x->black;
        CHECK(dd >= -2 && dd <= 2, "%s: %u white, %u black", NAMES[i], x->white, x->black);
    }
    int most = 0, least = 1000;
    for (int i = 0; i < 5; i++) { int g = ap_of(a, P[i])->games; most = g > most ? g : most; least = g < least ? g : least; }
    CHECK(most - least <= 2, "games played from %d to %d: the odd one out waits a pass, not the arena", least, most);

    TEST("the standings: by score, then rating");
    static uint8_t buf[CW_ARENA_FRAME_MAX];
    size_t n = cc_arena_fill(a, P[2], 123, buf);
    cw_arena_hdr h;
    memcpy(&h, buf, sizeof h);
    CHECK(n == sizeof h + 5 * sizeof(cw_arena_row) && h.n_rows == 5 && h.n_players == 5, "%zu bytes, %u rows", n, h.n_rows);
    CHECK(h.kind == CW_P_ARENA && h.status == CC_ARENA_RUNNING && h.speed == CC_BLITZ && h.base_s == 180 && h.now == 123, "header");
    CHECK(!strcmp(h.name, "Blitz Test") && !strcmp(h.by, "ann"), "name %s by %s", h.name, h.by);
    cw_arena_row rows[5];
    memcpy(rows, buf + sizeof h, sizeof rows);
    for (int i = 0; i < 5; i++) {
        CHECK(rows[i].rank == i + 1, "rank %u at %d", rows[i].rank, i);
        if (i) CHECK(rows[i - 1].score > rows[i].score || (rows[i - 1].score == rows[i].score && rows[i - 1].rating >= rows[i].rating),
                     "%s %u/%d above %s %u/%d", rows[i - 1].name, rows[i - 1].score, rows[i - 1].rating, rows[i].name, rows[i].score, rows[i].rating);
        cc_arena_player *x = ap_of(a, cc_player_by_name(rows[i].name));
        CHECK(x && rows[i].score == x->score && rows[i].games == x->games && rows[i].fire == (x->streak >= 2), "row %s", rows[i].name);
    }
    cc_arena_player *cat = ap_of(a, P[2]);
    CHECK(h.me_in == CW_ARENA_IN && h.me_rank == cat->rank && h.me_score == cat->score && h.me_games == cat->games, "me: in %u rank %u", h.me_in, h.me_rank);
    n = cc_arena_fill(a, P[5], 0, buf);
    memcpy(&h, buf, sizeof h);
    CHECK(h.me_in == CW_ARENA_OUT && !h.me_rank, "fay is not in it");
    pthread_mutex_unlock(&cc_lock);
}

static void leaving(void) {
    pthread_mutex_lock(&cc_lock);
    cc_arena *a = cc_arena_get(g_arena);
    TEST("a withdrawn player stays in the standings and is not paired");
    g_now += CC_ARENA_REST_MS;
    CHECK(cc_arena_withdraw(a, P[1]) == CW_E_NONE, "withdraw");
    CHECK(cc_arena_withdraw(a, P[1]) == CW_E_ARENA_NOT_JOINED, "withdrawn twice");
    cc_arena_tick(g_now, W0 + 110000);
    CHECK(!game_of(a, P[1]) && ap_of(a, P[1]) && ap_of(a, P[1])->withdrawn, "ben was paired");
    CHECK(a->n == 5, "ben left the standings");
    for (int i = 0; i < 5; i++) if (game_of(a, P[i])) win(a, P[i], g_now);
    TEST("and comes back");
    CHECK(cc_arena_join(a, P[1]) == CW_E_NONE && !ap_of(a, P[1])->withdrawn, "rejoin");

    TEST("an arena game is aborted only by the first-move clock, and the absent player is withdrawn");
    g_now += CC_ARENA_REST_MS;
    cc_arena_tick(g_now, W0 + 115000);
    cc_live *l = game_of(a, P[0]);
    CHECK(l != NULL, "ann was not paired");
    if (l) {
        uint16_t s0 = ap_of(a, l->p[0])->score, s1 = ap_of(a, l->p[1])->score;
        const cc_player *w = l->p[0], *b = l->p[1];
        int64_t nx;
        CHECK(cc_game_tick(&l->g, g_now + CC_FIRST_MOVE_MS + 1, &nx), "the first-move clock");
        cc_live_changed(l, g_now + CC_FIRST_MOVE_MS + 1);
        CHECK(ap_of(a, w)->withdrawn && !ap_of(a, b)->withdrawn, "white, who did not move, is withdrawn");
        CHECK(ap_of(a, w)->score == s0 && ap_of(a, b)->score == s1, "an aborted game scored");
        CHECK(cc_arena_join(a, (cc_player *)w) == CW_E_NONE, "white comes back");
        g_now += CC_FIRST_MOVE_MS + 1;
    }
    for (int i = 0; i < 5; i++) if (game_of(a, P[i])) win(a, P[i], g_now);
    pthread_mutex_unlock(&cc_lock);
}

static void refusals(void) {
    pthread_mutex_lock(&cc_lock);
    TEST("refused: a guest's arena, a bad name, a bad time control or length, a fifth upcoming");
    cw_arena_req q;
    memset(&q, 0, sizeof q);
    q.base_s = 60;
    q.duration_s = 600;
    memcpy(q.name, "Fine Name", 9);
    cc_arena *a = NULL;
    CHECK(cc_arena_create(P[6], &q, W0, &a) == CW_E_ARENA_GUEST && !a, "a guest's");
    memcpy(q.name, "x/", 3);
    CHECK(cc_arena_create(P[1], &q, W0, &a) == CW_E_ARENA_NAME, "a name");
    memcpy(q.name, "Fine Name", 10);
    q.duration_s = CC_ARENA_DURATION_MIN - 1;
    CHECK(cc_arena_create(P[1], &q, W0, &a) == CW_E_BAD, "too short");
    q.duration_s = CC_ARENA_DURATION_MAX + 1;
    CHECK(cc_arena_create(P[1], &q, W0, &a) == CW_E_BAD, "too long");
    q.duration_s = 600;
    q.start_in_s = CC_ARENA_START_MAX + 1;
    CHECK(cc_arena_create(P[1], &q, W0, &a) == CW_E_BAD, "too far ahead");
    q.start_in_s = 600;
    q.base_s = 0;
    CHECK(cc_arena_create(P[1], &q, W0, &a) == CW_E_BAD, "0+0");
    q.base_s = 60;
    for (int i = 0; i < CC_ARENA_PER_CREATOR; i++) CHECK(cc_arena_create(P[1], &q, W0, &a) == CW_E_NONE, "upcoming %d", i);
    CHECK(cc_arena_create(P[1], &q, W0, &a) == CW_E_ARENA_TOO_MANY, "a fifth");
    pthread_mutex_unlock(&cc_lock);
}

static int g_k;   // the player whose game is in play across the end and the restart
static uint16_t g_score, g_games;
static uint32_t g_made;   // games the arena made

static void the_end(void) {
    pthread_mutex_lock(&cc_lock);
    cc_arena *a = cc_arena_get(g_arena);
    g_now += CC_ARENA_REST_MS;
    cc_arena_tick(g_now, W0 + 120000);
    g_k = 0;
    while (g_k < 4 && !game_of(a, P[g_k])) g_k++;
    cc_live *l = game_of(a, P[g_k]);
    CHECK(l != NULL, "nobody paired");
    const cc_player *o = opponent(a, P[g_k]);
    for (int i = 0; i < 5; i++) if (game_of(a, P[i]) && P[i] != P[g_k] && P[i] != o) win(a, P[i], g_now);
    g_playing_game = l ? l->id : 0;
    g_score = ap_of(a, P[g_k])->score;
    g_games = ap_of(a, P[g_k])->games;
    g_made = a->games;
    TEST("the end: finished, and no pairing after it");
    uint32_t games = a->games;
    cc_arena_tick(g_now + 60000, W0 + 660000);
    CHECK(a->status == CC_ARENA_FINISHED && a->games == games && a->playing == 2, "status %d games %u playing %u", a->status, a->games, a->playing);
    CHECK(cc_arena_join(a, P[5]) == CW_E_ARENA_OVER, "joined a finished arena");
    pthread_mutex_unlock(&cc_lock);
}

static void restart(void) {
    TEST("a restart brings back the arena, its players and its game in play");
    close_state();
    open_state();
    reload_players();
    pthread_mutex_lock(&cc_lock);
    cc_arena *a = cc_arena_get(g_arena);
    CHECK(a && a->status == CC_ARENA_FINISHED && a->n == 5 && !strcmp(a->name, "Blitz Test"), "arena %p", (void *)a);
    if (!a) { pthread_mutex_unlock(&cc_lock); return; }
    CHECK(a->games == g_made && g_made > 20, "games made %u, were %u", a->games, g_made);
    cc_live *l = game_of(a, P[g_k]);
    CHECK(a->playing == 2 && l && l->id == g_playing_game && l->arena == g_arena, "the game in play");
    cc_arena_player *k = ap_of(a, P[g_k]);
    uint16_t before = k->score;
    CHECK(before == g_score && k->games == g_games && before >= 4, "%s %u/%u, was %u/%u", NAMES[g_k], k->score, k->games, g_score, g_games);
    TEST("a game in play at the end still counts");
    int gets = cc_arena_on_fire(k->streak) ? 4 : 2;
    win(a, P[g_k], cc_mono());
    CHECK(k->score == before + gets && a->playing == 0, "after the end: %u, was %u", k->score, before);
    cc_arena_rank(a);
    uint16_t final_score[5];
    char final_who[5][CW_NAME + 1];   // the players are freed by the restart: their names
    for (int i = 0; i < 5; i++) { snprintf(final_who[i], sizeof final_who[i], "%s", a->pl[a->rank[i]].p->name); final_score[i] = a->pl[a->rank[i]].score; }
    // an arena that ended hours ago: not brought back by a restart, but its id is taken
    int64_t past = cc_wall() - 3 * 3600000LL;
    cc_arena *old = make(past, 0, CC_ARENA_DURATION_MIN, "Old");
    uint32_t old_id = old ? old->id : 0;
    cc_arena_tick(cc_mono(), past + 60000);
    CHECK(old && old->status == CC_ARENA_FINISHED, "the old arena did not end");
    pthread_mutex_unlock(&cc_lock);
    close_state();
    open_state();
    reload_players();
    pthread_mutex_lock(&cc_lock);
    TEST("an arena finished over an hour ago stays in the journal; ids go on after the journal's");
    CHECK(!cc_arena_get(old_id), "the old arena came back into memory");
    cc_arena *b = make(W0 + 700000, 0, 60, "After");
    CHECK(b && b->id == old_id + 1, "id %u, the old one %u", b ? b->id : 0, old_id);
    pthread_mutex_unlock(&cc_lock);
    close_state();
    open_state();
    reload_players();
    TEST("the final standings read back from the journal");
    static uint8_t buf[CW_ARENA_FRAME_MAX];
    int n = cc_db_arena(g_ctx.store, g_arena, IDS[0], W0, buf);
    cw_arena_hdr h;
    memcpy(&h, buf, sizeof h);
    CHECK(n == (int)(sizeof h + 5 * sizeof(cw_arena_row)) && h.status == CC_ARENA_FINISHED && h.n_rows == 5, "%d bytes", n);
    CHECK(h.me_in == CW_ARENA_IN && h.me_rank >= 1 && !strcmp(h.by, "ann"), "me %u rank %u by %s", h.me_in, h.me_rank, h.by);
    CHECK(h.games == g_made && h.n_players == 5, "games %u (made %u), players %u", h.games, g_made, h.n_players);
    for (int i = 0; i < 5 && n > 0; i++) {
        cw_arena_row r;
        memcpy(&r, buf + sizeof h + (size_t)i * sizeof r, sizeof r);
        CHECK(r.rank == i + 1 && !strcmp(r.name, final_who[i]) && r.score == final_score[i], "row %d: %s %u, at the end %s %u", i,
              r.name, r.score, final_who[i], final_score[i]);
    }
    CHECK(cc_db_arena(g_ctx.store, 999999, NULL, W0, buf) == 0, "no such arena");
}

static void schedule(void) {
    pthread_mutex_lock(&cc_lock);
    cc_arena_schedule(true);
    TEST("the schedule: one upcoming arena per speed on the hour");
    int64_t wall = 1800000000000 / 3600000 * 3600000 + 30 * 60000;   // half past an hour
    cc_arena_tick(cc_mono(), wall);
    static cw_arena_list list;
    cc_arena_list_fill(&list, wall);
    int hourly = 0;
    for (int i = 0; i < list.n; i++) {
        cw_arena_item *r = &list.rows[i];
        if (strncmp(r->name, "Hourly", 6)) continue;
        hourly++;
        CHECK(r->status == CC_ARENA_UPCOMING && r->starts_at == wall + 30 * 60000, "%s at %lld", r->name, (long long)r->starts_at);
        CHECK(r->ends_at - r->starts_at == (r->base_s == 60 ? 27 : 57) * 60000, "%s lasts %lld", r->name, (long long)(r->ends_at - r->starts_at));
    }
    CHECK(hourly == 3, "%d hourly arenas", hourly);
    cc_arena_tick(cc_mono(), wall + 1000);
    cc_arena_list_fill(&list, wall);
    int again = 0;
    for (int i = 0; i < list.n; i++) again += !strncmp(list.rows[i].name, "Hourly", 6);
    CHECK(again == 3, "a second pass made more: %d", again);
    TEST("at the hour they run, and the next hour's are made");
    cc_arena_tick(cc_mono(), wall + 30 * 60000);
    cc_arena_list_fill(&list, wall + 30 * 60000);
    int running = 0, upcoming = 0;
    for (int i = 0; i < list.n; i++) {
        if (strncmp(list.rows[i].name, "Hourly", 6)) continue;
        running += list.rows[i].status == CC_ARENA_RUNNING;
        upcoming += list.rows[i].status == CC_ARENA_UPCOMING && list.rows[i].starts_at == wall + 90 * 60000;
    }
    CHECK(running == 3 && upcoming == 3, "running %d upcoming %d", running, upcoming);
    TEST("the list: running first, then upcoming by start, then finished");
    for (int i = 1; i < list.n; i++) {
        static const int group[] = { 1, 0, 2 };
        CHECK(group[list.rows[i - 1].status] <= group[list.rows[i].status], "%s before %s", list.rows[i - 1].name, list.rows[i].name);
    }
    cc_arena_schedule(false);
    pthread_mutex_unlock(&cc_lock);
}

int main(void) {
    ch_init();
    snprintf(g_dir, sizeof g_dir, "/tmp/unit_arenas_%d", (int)getpid());
    char cmd[300], db[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", g_dir, g_dir);
    if (system(cmd) != 0) return 1;
    snprintf(db, sizeof db, "%s/c.sqlite", g_dir);
    snprintf(g_cfg.db_path, sizeof g_cfg.db_path, "%s", db);
    g_ctx.cfg = &g_cfg;
    if (cb_store_open_sqlite(db, &g_ctx.store) != CB_OK) { fprintf(stderr, "no store\n"); return 1; }
    open_state();
    players();
    pairing();
    streaks_and_draws();
    leaving();
    refusals();
    the_end();
    restart();
    schedule();
    close_state();
    cb_store_close(g_ctx.store);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) return 1;
    return report("unit_arenas");
}
