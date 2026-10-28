// unit_cup.c - cup.h's bookkeeping: the round robin's fixed order one match at a time, who
// sits first, the standings' keys and head to head, the final's seats, the champion.
#include "t.h"
#include "cup.h"

#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Plays match m to the score `a_wins`-`b_wins`: the loser's wins first, so the match is decided by
// the last game.
static void play(be_w_cup *c, int m, int a_wins, int b_wins) {
    const be_w_match *x = &c->matches[m];
    uint8_t a = x->a, b = x->b;
    if (a_wins < b_wins)
        for (int i = 0; i < a_wins; i++) T_CHECK(be_cup_result(c, m, true, b, 1000));
    for (int i = 0; i < b_wins; i++) T_CHECK(be_cup_result(c, m, true, a, 1000));
    if (a_wins >= b_wins)
        for (int i = 0; i < a_wins; i++) T_CHECK(be_cup_result(c, m, true, b, 1000));
}

static void the_round_robin_plays_in_order_one_match_at_a_time(void) {
    be_w_cup c;
    const uint8_t bots[4] = { 10, 11, 12, 13 };   // kestrel, sapper, sonnet, haiku
    be_cup_draw(&c, bots, 5);
    uint8_t seats[2];
    T_EQ_INT(be_cup_next(&c, seats), 0);
    T_EQ_INT(seats[0], 10);   // kestrel v haiku, kestrel first in game 1
    T_EQ_INT(seats[1], 13);
    be_cup_started(&c, 0, "g1", 2);
    T_EQ_INT(be_cup_next(&c, seats), -1);   // its game is in play: nothing else starts
    T_CHECK(be_cup_result(&c, 0, true, 13, 1000));   // haiku the fool
    T_EQ_INT(be_cup_next(&c, seats), 0);
    T_EQ_INT(seats[0], 13);   // game 2: haiku first
    T_CHECK(be_cup_result(&c, 0, true, BE_TOUR_NONE, 1000));   // a draw is played again
    T_EQ_INT(c.result[1], BE_CUP_G_DRAW);
    T_EQ_INT(be_cup_next(&c, seats), 0);
    T_EQ_INT(seats[0], 10);   // game 3: kestrel first again
    play(&c, 0, 2, 0);
    T_EQ_INT(c.matches[0].winner, 10);
    T_EQ_INT(be_cup_next(&c, seats), 1);   // then sapper v sonnet
    T_EQ_INT(seats[0], 11);
    T_EQ_INT(seats[1], 12);
    char id[64];
    be_cup_match_id(&c, 0, id, sizeof id);
    T_EQ_STR(id, "m1-kestrel-v-haiku");
}

static void the_standings_seat_the_final_and_the_final_crowns(void) {
    be_w_cup c;
    const uint8_t bots[4] = { 0, 1, 2, 3 };
    be_cup_draw(&c, bots, 5);
    // kestrel beats haiku 3-0, sapper beats sonnet 3-2, kestrel beats sonnet 3-0, haiku beats sapper
    // 3-2, sonnet beats haiku 3-0, sapper beats kestrel 3-1.
    play(&c, 0, 3, 0);
    play(&c, 1, 3, 2);
    play(&c, 2, 3, 0);
    play(&c, 3, 2, 3);
    play(&c, 4, 3, 0);
    T_EQ_INT(c.matches[6].a, BE_TOUR_NONE);   // no final before the sixth match is decided
    play(&c, 5, 1, 3);
    // Match wins: kestrel 2, sapper 2, sonnet 1, haiku 1. Games: kestrel 7-3 (+4), sapper 8-6 (+2).
    // kestrel and sapper are level on matches; sapper has more game wins, which come before the
    // difference.
    T_EQ_INT(c.mw[0], 2);
    T_EQ_INT(c.mw[1], 2);
    T_EQ_INT(c.gw[0], 7);
    T_EQ_INT(c.gl[0], 3);
    T_EQ_INT(c.gl[1], 6);
    T_EQ_INT(c.gw[1], 8);
    T_EQ_INT(c.order[0], 1);   // sapper
    T_EQ_INT(c.order[1], 0);   // kestrel
    // sonnet 1 win, games 5-6; haiku 1 win, games 3-8: sonnet ahead on game wins.
    T_EQ_INT(c.order[2], 2);
    T_EQ_INT(c.order[3], 3);
    T_EQ_INT(c.matches[6].a, 1);   // the final: sapper (first) v kestrel
    T_EQ_INT(c.matches[6].b, 0);
    T_EQ_INT(c.state, BE_TOUR_RUNNING);
    uint8_t seats[2];
    T_EQ_INT(be_cup_next(&c, seats), 6);
    play(&c, 6, 1, 3);
    T_EQ_INT(c.state, BE_TOUR_DONE);
    T_EQ_INT(c.champion, 0);
    T_EQ_INT(be_cup_next(&c, seats), -1);
}

static void head_to_head_breaks_a_level_tie(void) {
    be_w_cup c;
    const uint8_t bots[4] = { 0, 1, 2, 3 };
    be_cup_draw(&c, bots, 5);
    play(&c, 0, 3, 0);   // kestrel beats haiku
    play(&c, 1, 3, 0);   // sapper beats sonnet
    play(&c, 2, 3, 0);   // kestrel beats sonnet
    play(&c, 3, 0, 3);   // haiku beats sapper
    play(&c, 4, 3, 0);   // sonnet beats haiku
    play(&c, 5, 0, 3);   // sapper beats kestrel
    // kestrel 2-1 games 6-3; sapper 2-1 games 6-3; level on every key but head to head: sapper.
    T_EQ_INT(c.gw[0], c.gw[1]);
    T_EQ_INT(c.gl[0], c.gl[1]);
    T_EQ_INT(c.order[0], 1);
    T_EQ_INT(c.order[1], 0);
}

// The file a story writes, read back whole.
static char *story_file(be_cup_story *s, const char *const names[2], int fool) {
    char path[] = "/tmp/unit_cup_story_XXXXXX";
    int fd = mkstemp(path);
    T_CHECK(fd >= 0);
    close(fd);
    T_CHECK(be_cup_story_write(s, names, fool, 4, path));
    FILE *f = fopen(path, "r");
    static char buf[1 << 20];
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    buf[n] = 0;
    if (f) fclose(f);
    remove(path);
    return buf;
}

static int count(const char *hay, const char *needle) {
    int n = 0;
    for (const char *p = strstr(hay, needle); p; p = strstr(p + 1, needle)) n++;
    return n;
}

// A game of hundreds of moves is written whole: no line cut, every move there.
static void a_long_game_is_written_whole(void) {
    be_cup_story s;
    memset(&s, 0, sizeof s);
    const char *names[2] = { "kestrel (Fable)", "haiku (Haiku)" };
    be_cup_story_deal(&s, names, 0, 0, 7, 0, 23);
    int8_t su[2], va[2];
    // 700 pickups of two cards drawn at random (a fixed stream), so no stretch repeats and nothing
    // folds; the stock count each line carries numbers it.
    uint32_t x = 12345;
    for (int i = 0; i < 700; i++) {
        for (int c = 0; c < 2; c++) {
            x = x * 1664525u + 1013904223u;
            su[c] = (int8_t)((x >> 24) % 4);
            va[c] = (int8_t)(5 + (x >> 16) % 9);
        }
        be_cup_story_event(&s, names, 0, BE_EV_PICKUP, (int)((x >> 8) % 2), su, va, 2, 1000 + i);
    }
    char *out = story_file(&s, names, 1);
    T_EQ_INT(count(out, "picks up 2 cards"), 700);
    T_CHECK(strstr(out, "move 700: ") != NULL);
    T_CHECK(strstr(out, "stock 1699\n") != NULL);   // the last line whole
    T_CHECK(strstr(out, "Moves: 700 ") != NULL);
    T_CHECK(strstr(out, "Fool: haiku (Haiku)") != NULL);
    be_cup_story_reset(&s);
}

// A pass loop: the same 4 moves 50 times is told once, with its count, in both the moments and the
// move list; what comes before and after is told as it was.
static void a_repeated_stretch_is_folded(void) {
    be_cup_story s;
    memset(&s, 0, sizeof s);
    const char *names[2] = { "sapper (Opus)", "haiku (Haiku)" };
    be_cup_story_deal(&s, names, 0, 0, 7, 0, 23);
    int8_t su[3] = { 2, 3, 1 }, va[3] = { 5, 5, 9 };
    be_cup_story_event(&s, names, 0, BE_EV_ATTACK, 0, su + 2, va + 2, 1, 0);   // move 1: a one-off
    for (int r = 0; r < 50; r++) {                                             // moves 2..201
        be_cup_story_event(&s, names, 0, BE_EV_ATTACK, 0, su, va, 1, 0);
        be_cup_story_event(&s, names, 0, BE_EV_PASS, 1, su + 1, va + 1, 1, 0);
        be_cup_story_event(&s, names, 0, BE_EV_PASS, 0, su + 2, va + 2, 1, 0);
        be_cup_story_event(&s, names, 0, BE_EV_PICKUP, 1, su, va, 3, 0);
    }
    be_cup_story_event(&s, names, 0, BE_EV_OUT, 0, su, va, 0, 0);              // move 202
    char *out = story_file(&s, names, 1);
    T_CHECK(strstr(out, "moves 2-201: the same 4-move cycle repeated 50 times") != NULL);
    T_CHECK(strstr(out, "[moves 2-201: 50 x (s:A 6C | h:P 6D | s:P TH | h:T 6C 6D TH)]") != NULL);
    T_EQ_INT(count(out, "passes the attack on"), 2);   // the first round's two passes, not 100
    T_CHECK(strstr(out, "move 202: sapper (Opus) goes out") != NULL);
    T_CHECK(strstr(out, "Moves: 202 ") != NULL);
    T_CHECK(strstr(out, "Passes: sapper (Opus) 50, haiku (Haiku) 50") != NULL);   // the counts are of every move
    be_cup_story_reset(&s);
}

int main(void) {
    T_RUN(the_round_robin_plays_in_order_one_match_at_a_time);
    T_RUN(the_standings_seat_the_final_and_the_final_crowns);
    T_RUN(head_to_head_breaks_a_level_tie);
    T_RUN(a_long_game_is_written_whole);
    T_RUN(a_repeated_stretch_is_folded);
    return T_DONE();
}
