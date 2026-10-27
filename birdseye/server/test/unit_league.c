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

int main(void) {
    T_RUN(the_league_never_seats_the_human);
    return T_DONE();
}
