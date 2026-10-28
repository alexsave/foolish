// unit_cup.c - cup.h's bookkeeping: the round robin's fixed order one match at a time, who
// sits first, the standings' keys and head to head, the final's seats, the champion.
#include "t.h"
#include "cup.h"

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

int main(void) {
    T_RUN(the_round_robin_plays_in_order_one_match_at_a_time);
    T_RUN(the_standings_seat_the_final_and_the_final_crowns);
    T_RUN(head_to_head_breaks_a_level_tie);
    return T_DONE();
}
