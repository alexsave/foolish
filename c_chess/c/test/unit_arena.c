// unit_arena.c - an arena's rules (lichess's): a win scores 2, a draw 1, a loss 0; after two wins in
// a row a player is on fire and a win scores 4 and a draw 2 until a loss or a draw ends it; a draw
// before 20 plies scores nothing; and the names an arena may have.
#include "arena.h"
#include "t.h"

static void points(void) {
    TEST("a win, a draw, a loss");
    CHECK(cc_arena_points(CC_ARENA_WIN, 0, 40) == 2, "win %d", cc_arena_points(CC_ARENA_WIN, 0, 40));
    CHECK(cc_arena_points(CC_ARENA_DRAW, 0, 40) == 1, "draw %d", cc_arena_points(CC_ARENA_DRAW, 0, 40));
    CHECK(cc_arena_points(CC_ARENA_LOSS, 0, 40) == 0, "loss %d", cc_arena_points(CC_ARENA_LOSS, 0, 40));

    TEST("one win in a row is not fire");
    CHECK(cc_arena_points(CC_ARENA_WIN, 1, 40) == 2, "after one win %d", cc_arena_points(CC_ARENA_WIN, 1, 40));
    CHECK(cc_arena_points(CC_ARENA_DRAW, 1, 40) == 1, "draw after one win %d", cc_arena_points(CC_ARENA_DRAW, 1, 40));
    CHECK(!cc_arena_on_fire(1) && !cc_arena_on_fire(0), "on fire after one win");

    TEST("on fire: a win scores 4, a draw 2, a loss 0");
    CHECK(cc_arena_on_fire(2) && cc_arena_on_fire(7) && cc_arena_on_fire(255), "not on fire after two wins");
    CHECK(cc_arena_points(CC_ARENA_WIN, 2, 40) == 4, "third win %d", cc_arena_points(CC_ARENA_WIN, 2, 40));
    CHECK(cc_arena_points(CC_ARENA_WIN, 9, 40) == 4, "tenth win %d", cc_arena_points(CC_ARENA_WIN, 9, 40));
    CHECK(cc_arena_points(CC_ARENA_DRAW, 2, 40) == 2, "draw on fire %d", cc_arena_points(CC_ARENA_DRAW, 2, 40));
    CHECK(cc_arena_points(CC_ARENA_LOSS, 3, 40) == 0, "loss on fire %d", cc_arena_points(CC_ARENA_LOSS, 3, 40));

    TEST("a draw before 20 plies scores nothing, on fire or not");
    CHECK(cc_arena_points(CC_ARENA_DRAW, 0, 19) == 0, "19 plies %d", cc_arena_points(CC_ARENA_DRAW, 0, 19));
    CHECK(cc_arena_points(CC_ARENA_DRAW, 0, CC_ARENA_DRAW_PLIES) == 1, "20 plies %d", cc_arena_points(CC_ARENA_DRAW, 0, CC_ARENA_DRAW_PLIES));
    CHECK(cc_arena_points(CC_ARENA_DRAW, 4, 0) == 0, "an early draw on fire %d", cc_arena_points(CC_ARENA_DRAW, 4, 0));
    TEST("a quick win counts in full");
    CHECK(cc_arena_points(CC_ARENA_WIN, 0, 4) == 2 && cc_arena_points(CC_ARENA_WIN, 2, 4) == 4, "a mate in two");
}

static void streaks(void) {
    TEST("wins build a streak, anything else ends it");
    CHECK(cc_arena_streak(CC_ARENA_WIN, 0) == 1 && cc_arena_streak(CC_ARENA_WIN, 1) == 2 && cc_arena_streak(CC_ARENA_WIN, 2) == 3, "wins");
    CHECK(cc_arena_streak(CC_ARENA_DRAW, 5) == 0, "a draw kept the streak: %d", cc_arena_streak(CC_ARENA_DRAW, 5));
    CHECK(cc_arena_streak(CC_ARENA_LOSS, 5) == 0, "a loss kept the streak: %d", cc_arena_streak(CC_ARENA_LOSS, 5));
    CHECK(cc_arena_streak(CC_ARENA_DRAW, 0) == 0 && cc_arena_streak(CC_ARENA_LOSS, 1) == 0, "from none");
    TEST("a streak stops counting at 255 (a byte) and stays on fire");
    CHECK(cc_arena_streak(CC_ARENA_WIN, 255) == 255, "%d", cc_arena_streak(CC_ARENA_WIN, 255));

    TEST("a run: W W W D W L as lichess scores it");
    static const int run[] = { CC_ARENA_WIN, CC_ARENA_WIN, CC_ARENA_WIN, CC_ARENA_DRAW, CC_ARENA_WIN, CC_ARENA_LOSS };
    static const int want[] = { 2, 2, 4, 2, 2, 0 };
    int s = 0, total = 0;
    for (int i = 0; i < 6; i++) {
        int p = cc_arena_points(run[i], s, 60);
        CHECK(p == want[i], "game %d: %d, want %d", i + 1, p, want[i]);
        total += p;
        s = cc_arena_streak(run[i], s);
    }
    CHECK(total == 12 && s == 0, "total %d streak %d", total, s);
}

static int ok(const char *s) { return cc_arena_name_ok(s, strlen(s)); }

static void names(void) {
    TEST("an arena's name: 3 to 30 of letters, digits, space, '-' and '_'");
    CHECK(ok("Hourly Blitz") && ok("abc") && ok("Mon-Fri_arena 2"), "plain names refused");
    CHECK(ok("abcdefghijklmnopqrstuvwxyz0123"), "30 refused");
    CHECK(!ok("abcdefghijklmnopqrstuvwxyz01234"), "31 taken");
    CHECK(!ok("ab") && !ok(""), "too short taken");
    CHECK(!ok("a/b c") && !ok("a<b>c") && !ok("caf\xc3\xa9") && !ok("tab\there"), "a byte outside the set taken");
    TEST("no space at either end");
    CHECK(!ok(" abc") && !ok("abc ") && ok("a  b"), "the ends");
    TEST("the length is the caller's, a NUL inside is refused");
    CHECK(cc_arena_name_ok("abcdef", 3) && !cc_arena_name_ok("ab\0de", 5), "length");
}

int main(void) {
    points();
    streaks();
    names();
    return report("unit_arena");
}
