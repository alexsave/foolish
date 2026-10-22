// unit_tour.c - the bracket (tournament.h): who is left out, the seeds, the pairings, the
// best-of-five, draws, the winner sent on, the champion, and the file read back by bot id.
#include "t.h"
#include "tournament.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define N 49

static int32_t g_rating[N];
static bool g_random[N];

// 49 bots: bot i rated 1000 + i (so bot 48 is the best), bots 0..6 random.
static void field(void) {
    for (int i = 0; i < N; i++) {
        g_rating[i] = 1000 + i;
        g_random[i] = i < 7;
    }
    g_rating[3] = 900;   // the weakest random bot
    g_rating[10] = 800;  // weaker still, but not random: it plays
}

static void draw(void) {
    field();
    static be_w_tour t;
    T_CHECK(be_tour_draw(&t, g_rating, g_random, N, 5));
    T_EQ_INT(t.state, BE_TOUR_RUNNING);
    T_EQ_INT(t.dropped, 3);
    T_EQ_INT(t.champion, BE_TOUR_NONE);
    T_EQ_INT(t.seed_bot[0], 48);
    T_EQ_INT(t.seed_bot[47], 10);
    for (int s = 1; s < BE_TOUR_PLAYERS; s++) T_CHECK(t.seed_rating[s] <= t.seed_rating[s - 1]);
    // The opening round: 17 v 48 .. 32 v 33, each feeding the round-of-32 match of its lower seed.
    int seen[49] = { 0 };
    for (int j = 0; j < 16; j++) {
        const be_w_match *o = &t.matches[j];
        T_EQ_INT(o->round, 0);
        T_EQ_INT(o->seed_a + o->seed_b, 65);
        T_CHECK(o->seed_a >= 17 && o->seed_a <= 32);
        int side = -1;
        int next = be_tour_feeds(j, &side);
        T_EQ_INT(next, 16 + j);
        T_EQ_INT(side, 1);
        const be_w_match *r = &t.matches[16 + j];
        T_EQ_INT(r->round, 1);
        T_CHECK(r->seed_a >= 1 && r->seed_a <= 16);
        T_EQ_INT(r->seed_a + o->seed_a, 33);   // 1 v 32, 2 v 31 ... in the standard order
        T_EQ_INT(r->b, BE_TOUR_NONE);
        seen[o->seed_a]++;
        seen[o->seed_b]++;
        seen[r->seed_a]++;
    }
    for (int s = 1; s <= 48; s++) T_EQ_INT(seen[s], 1);
    // Seeds 1 and 2 are in opposite halves: they can meet only in the final.
    int half1 = -1, half2 = -1;
    for (int j = 0; j < 16; j++) {
        if (t.matches[16 + j].seed_a == 1) half1 = j / 8;
        if (t.matches[16 + j].seed_a == 2) half2 = j / 8;
    }
    T_CHECK(half1 >= 0 && half2 >= 0 && half1 != half2);
    // Fewer than 49 bots: no draw.
    T_CHECK(!be_tour_draw(&t, g_rating, g_random, 48, 5));
}

// Plays every match to its end: `fool_of(m)` names the loser of each game.
static void play_out_by(be_w_tour *t, int draws_first, bool upsets);
static void play_out(be_w_tour *t, int draws_first) { play_out_by(t, draws_first, false); }

static void play_out_by(be_w_tour *t, int draws_first, bool upsets) {
    int guard = 0;
    uint8_t seats[2];
    int m;
    while ((m = be_tour_next(t, seats)) >= 0 && guard++ < 10000) {
        be_w_match *x = &t->matches[m];
        T_CHECK(seats[0] != seats[1]);
        T_CHECK((seats[0] == x->a && seats[1] == x->b) || (seats[0] == x->b && seats[1] == x->a));
        be_tour_started(t, m, "abc", 3);
        uint8_t again[2];
        T_CHECK(be_tour_next(t, again) != m);   // a match plays one game at a time
        // The higher-rated bot wins, after `draws_first` drawn games in each match.
        bool a_lower = g_rating[x->a] < g_rating[x->b];
        uint8_t fool = x->games < draws_first ? BE_TOUR_NONE : (a_lower != upsets ? x->a : x->b);
        T_CHECK(be_tour_result(t, m, true, fool, 77));
    }
}

static void best_of_five(void) {
    field();
    static be_w_tour t;
    be_tour_draw(&t, g_rating, g_random, N, 5);
    play_out(&t, 2);
    T_EQ_INT(t.state, BE_TOUR_DONE);
    T_EQ_INT(t.champion, 48);   // the top seed wins every match
    T_EQ_INT(t.finished_ms, 77);
    for (int i = 0; i < BE_TOUR_MATCHES; i++) {
        const be_w_match *x = &t.matches[i];
        T_CHECK(x->winner != BE_TOUR_NONE);
        T_EQ_INT((x->wins_a == BE_TOUR_WIN) + (x->wins_b == BE_TOUR_WIN), 1);
        T_EQ_INT(x->games, 2 + BE_TOUR_WIN);   // two draws, then three straight
        T_EQ_INT(x->live, 0);
        // The winner is the one the other was the fool of three times: the higher-rated here.
        uint8_t loser = x->winner == x->a ? x->b : x->a;
        T_CHECK(g_rating[x->winner] > g_rating[loser]);
    }
    T_EQ_INT(t.games, BE_TOUR_MATCHES * (2 + BE_TOUR_WIN));
    // The final is between seeds 1 and 2.
    const be_w_match *f = &t.matches[BE_TOUR_MATCHES - 1];
    T_EQ_INT(f->seed_a + f->seed_b, 3);
}

// Every match an upset: the lower-rated bot wins it, so the winner is never simply side a.
static void upsets(void) {
    field();
    static be_w_tour t;
    be_tour_draw(&t, g_rating, g_random, N, 5);
    play_out_by(&t, 0, true);
    T_EQ_INT(t.state, BE_TOUR_DONE);
    for (int i = 0; i < BE_TOUR_MATCHES; i++) {
        const be_w_match *x = &t.matches[i];
        uint8_t loser = x->winner == x->a ? x->b : x->a;
        T_CHECK(g_rating[x->winner] < g_rating[loser]);
    }
    T_EQ_INT(t.champion, t.seed_bot[47]);   // the bottom seed wins it all
}

static void a_game_given_up_is_played_again(void) {
    field();
    static be_w_tour t;
    be_tour_draw(&t, g_rating, g_random, N, 5);
    uint8_t seats[2];
    int m = be_tour_next(&t, seats);
    be_tour_started(&t, m, "abc", 3);
    T_CHECK(be_tour_result(&t, m, false, BE_TOUR_NONE, 1));
    T_EQ_INT(t.matches[m].games, 0);
    T_EQ_INT(be_tour_next(&t, seats), m);
    // Endless draws: the higher seed goes on after BE_TOUR_MAX_GAMES.
    for (int g = 0; g < BE_TOUR_MAX_GAMES; g++) be_tour_result(&t, m, true, BE_TOUR_NONE, 1);
    const be_w_match *x = &t.matches[m];
    T_EQ_INT(x->winner, x->seed_a < x->seed_b ? x->a : x->b);
}

static void the_file(void) {
    field();
    static be_w_tour t, back;
    be_tour_draw(&t, g_rating, g_random, N, 5);
    play_out(&t, 0);
    char ids[N][16];
    const char *p[N], *rev[N];
    for (int i = 0; i < N; i++) {
        snprintf(ids[i], sizeof ids[i], "bot-%02d", i);
        p[i] = ids[i];
        rev[N - 1 - i] = ids[i];
    }
    char path[64];
    snprintf(path, sizeof path, "/tmp/unit_tour_%d.bin", (int)getpid());
    T_CHECK(be_tour_save(&t, path, p, N));
    // Read back against a server that lists its bots in the reverse order: every index follows its id.
    T_CHECK(be_tour_load(&back, path, rev, N));
    T_EQ_INT(back.state, BE_TOUR_DONE);
    T_EQ_INT(back.champion, N - 1 - t.champion);
    T_EQ_INT(back.dropped, N - 1 - t.dropped);
    for (int s = 0; s < BE_TOUR_PLAYERS; s++) T_EQ_INT(back.seed_bot[s], N - 1 - t.seed_bot[s]);
    for (int i = 0; i < BE_TOUR_MATCHES; i++) {
        T_EQ_INT(back.matches[i].a, N - 1 - t.matches[i].a);
        T_EQ_INT(back.matches[i].winner, N - 1 - t.matches[i].winner);
        T_EQ_INT(back.matches[i].wins_a, t.matches[i].wins_a);
    }
    // A server without one of its bots: the file is not read.
    T_CHECK(!be_tour_load(&back, path, p, N - 1 > 0 ? 1 : 0));
    unlink(path);
    T_CHECK(!be_tour_load(&back, path, p, N));
}

int main(void) {
    T_RUN(draw);
    T_RUN(best_of_five);
    T_RUN(upsets);
    T_RUN(a_game_given_up_is_played_again);
    T_RUN(the_file);
    return T_DONE();
}
