// unit_league.c - league.h's pure rules: who the league may seat (never the human's row).
#include "t.h"
#include "league.h"

#include <string.h>

static void the_league_never_seats_the_human(void) {
    T_CHECK(be_league_may_seat("cordite", 7, 0, 1));
    T_CHECK(be_league_may_seat("cordite", 7, 3, 4));
    T_CHECK(!be_league_may_seat("cordite", 7, 4, 4));   // at its most tables already
    T_CHECK(!be_league_may_seat(BE_HUMAN_KEY, (int)strlen(BE_HUMAN_KEY), 0, 1));
    T_CHECK(!be_league_may_seat(BE_HUMAN_KEY, (int)strlen(BE_HUMAN_KEY), 0, 8));
    // A key that only begins like it is a bot's.
    T_CHECK(be_league_may_seat("humanoid", 8, 0, 1));
}

// A Bot Cup contest bot keeps state in statics: one table at a time, whatever the per-bot cap.
static void a_contest_bot_sits_at_one_table(void) {
    T_CHECK(be_league_may_seat("kestrel", 7, 0, 4));
    T_CHECK(!be_league_may_seat("kestrel", 7, 1, 4));
    T_CHECK(!be_league_may_seat("haiku", 5, 1, 8));
    T_CHECK(be_league_may_seat("haiku", 5, 0, 8));
    T_CHECK(be_league_may_seat("haik", 4, 1, 4));   // not a contest bot's key
}

// The "bots seated" gauge counts bots: a seat of the human's row is not one.
static void the_human_is_not_a_seated_bot(void) {
    const int two[] = {3, 48};
    T_CHECK(be_league_bot_seats(two, 2, 48) == 1);    // the human's table: one bot
    T_CHECK(be_league_bot_seats(two, 2, -1) == 2);    // no human in the field
    T_CHECK(be_league_bot_seats(two, 2, 7) == 2);     // a human elsewhere
    const int four[] = {0, 1, 2, 5};
    T_CHECK(be_league_bot_seats(four, 4, 0) == 3);    // row 0 is a real row, not "none"
    T_CHECK(be_league_bot_seats(four, 0, 0) == 0);
}

int main(void) {
    T_RUN(the_league_never_seats_the_human);
    T_RUN(the_human_is_not_a_seated_bot);
    T_RUN(a_contest_bot_sits_at_one_table);
    return T_DONE();
}
