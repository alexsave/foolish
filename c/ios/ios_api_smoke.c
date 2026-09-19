// The bridge, exercised WITHOUT a Mac.
//
// `make -C c ios-lib` needs xcrun and the iOS SDKs; this builds the same sources
// with the host compiler and runs a whole night through the Swift entry points,
// so the bridge's invariants are testable in CI on Linux. What it proves is the
// half that a Swift unit test cannot: that the flat accessors agree with the
// masked blob they read, including for the seats that must learn nothing.
#include "ww_api.h"
#include "ww_game.h"
#include "ww_view.h"
#include "ww_wire.h"
#include <stdio.h>
#include <string.h>

static int n_pass = 0, n_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { n_pass++; } \
    else { n_fail++; fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

int main(void) {
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 7 + 3);

    CHECK(wwi_new_game(seed, 7) == WW_OK, "seven players dealt");
    CHECK(wwi_new_game(seed, 4) == WW_ECOUNT, "four is refused");
    CHECK(wwi_new_game(seed, 7) == WW_OK, "back to seven");
    CHECK(wwi_view_phase(0) == WW_PHASE_NIGHT, "night one");
    CHECK(wwi_view_n_players(0) == 7, "seven seats");

    static const char *names[7] = { "Alex", "Sveta", "Kim", "Lee", "Ana", "Bo", "Cy" };
    for (int i = 0; i < 7; i++)
        CHECK(wwi_roster_set(i, (const uint8_t *)names[i], (int)strlen(names[i])) == WW_OK, "joined");
    CHECK(wwi_roster_count() == 7, "seven joins");
    CHECK(wwi_name_taken((const uint8_t *)"Kim", 3), "Kim is on the roster");
    CHECK(!wwi_name_taken((const uint8_t *)"Nobody", 6), "Nobody is not");

    // Find the cast through the bridge only, the way Swift has to.
    int wolf = -1, pack = -1, seer = -1, vill = -1;
    for (int s = 0; s < 7; s++) {
        const int r = wwi_view_role_of(s, s);       // a seat always knows its own
        if (r == WW_ROLE_WOLF)          { if (wolf < 0) wolf = s; else pack = s; }
        else if (r == WW_ROLE_SEER)     seer = s;
        else if (r == WW_ROLE_VILLAGER && vill < 0) vill = s;
    }
    CHECK(wolf >= 0 && pack >= 0 && seer >= 0 && vill >= 0, "a wolf, a pack, a seer, a villager");

    // A non-wolf is told nothing about anyone else.
    for (int s = 0; s < 7; s++) {
        if (s == vill) continue;
        CHECK(wwi_view_role_of(vill, s) == WW_ROLE_UNKNOWN, "the villager knows only himself");
    }
    CHECK(wwi_view_decider(vill) == WW_NO_SEAT, "and is not told who decides");
    CHECK(wwi_view_decider(wolf) != WW_NO_SEAT, "a wolf is");
    CHECK(wwi_view_role_of(wolf, pack) == WW_ROLE_WOLF, "a wolf knows the pack");
    CHECK(wwi_view_role_of(pack, wolf) == WW_ROLE_WOLF, "both ways");
    CHECK(wwi_view_role_of(-1, wolf) == WW_ROLE_UNKNOWN, "a spectator knows nothing");

    // The line refused from a non-wolf, accepted from a wolf, and readable by
    // exactly the two wolves.
    const char *line = "the seer is seat three";
    CHECK(wwi_night_act(vill, wolf, (const uint8_t *)line, (int)strlen(line), 0) == WW_ECHAT,
          "a villager cannot carry a line");
    CHECK(wwi_night_act(wolf, vill, (const uint8_t *)line, (int)strlen(line), 0) == WW_OK,
          "a wolf can");
    CHECK(wwi_view_chat_count(wolf) == 1, "one row for the author");
    CHECK(wwi_view_chat_count(pack) == 1, "one row for the pack");
    CHECK(wwi_view_chat_count(vill) == 0, "none for a villager");
    CHECK(wwi_view_chat_count(seer) == 0, "none for the seer");
    CHECK(wwi_view_chat_count(-1) == 0, "none for a spectator");
    {
        uint8_t got[WW_CHAT_MAX];
        const int n = wwi_view_chat_line(wolf, 0, got, (int)sizeof got);
        CHECK(n == (int)strlen(line) && memcmp(got, line, (size_t)n) == 0, "the line comes back");
        CHECK(wwi_view_chat_seat(wolf, 0) == wolf, "attributed to its author");
        CHECK(wwi_view_chat_line(vill, 0, got, (int)sizeof got) == -1, "and a villager gets nothing");
        CHECK(wwi_view_chat_line(wolf, 0, got, 4) == -1, "a short buffer is refused, not truncated");
    }

    // A third seat sees only that the wolf sent.
    CHECK(wwi_view_sent(seer, wolf) == WW_SENT_YES, "the wolf sent");
    CHECK(wwi_view_sent(seer, seer) == WW_SENT_NO, "the seer has not");
    CHECK(wwi_view_own_target(wolf) == vill, "the wolf's own pick is his own");
    CHECK(wwi_view_own_target(seer) == WW_NO_SEAT, "and the seer has picked nobody");

    // The seer asks, and only the seer is answered.
    CHECK(wwi_night_act(seer, wolf, 0, 0, 0) == WW_OK, "the seer asks about the wolf");
    CHECK(wwi_view_reading(seer, wolf) == WW_TEAM_WOLVES, "and is told the truth");
    CHECK(wwi_view_reading(vill, wolf) == WW_TEAM_NONE, "a villager is told nothing");
    CHECK(wwi_view_reading(pack, wolf) == WW_TEAM_NONE, "nor the other wolf");

    // The rest of the table sends, and the last record resolves the night.
    for (int s = 0; s < 7; s++) {
        if (wwi_view_sent(s, s) != WW_SENT_NO) continue;
        const int t = (s == vill) ? wolf : vill;
        const int rc = (wwi_view_role_of(s, s) == WW_ROLE_WOLF)
            ? wwi_night_act(s, t, (const uint8_t *)"aye", 3, 0)
            : wwi_night_act(s, t, 0, 0, 0);
        CHECK(rc == WW_OK, "one record per seat");
    }
    CHECK(wwi_view_phase(0) == WW_PHASE_DAY, "the night is over");
    CHECK(wwi_view_victim(0, 0) == vill, "and the wolves took who they said");
    CHECK(wwi_view_alive(0, vill) == 0, "the victim is dead");
    CHECK(wwi_view_role_of(seer, vill) == WW_ROLE_VILLAGER, "a dead seat's role is public");

    // Seal, and the bubble is a game every device can rebuild.
    uint8_t bubble[2048];
    const int len = wwi_seal(bubble, (int)sizeof bubble, 0xC0FFEEULL, 1234, 0, 0);
    CHECK(len > 0, "sealed");
    CHECK(wwi_seal(bubble, 8, 1, 0, 0, 0) == WW_MSG_ESHORT, "a short buffer is refused");
    {
        // Adopting our own bubble must land on the same game. Checked through the
        // view, because that is all a client can see.
        const int victim = wwi_view_victim(0, 0);
        const int turn = wwi_view_turn(0);
        CHECK(wwi_adopt(bubble, len) == WW_MSG_EOK, "adopted");
        CHECK(wwi_view_victim(0, 0) == victim, "same victim");
        CHECK(wwi_view_turn(0) == turn, "same turn");
        CHECK(wwi_roster_count() == 7, "and the roster rode along");
        uint8_t nm[WW_NAME_MAX];
        const int nl = wwi_roster_name(0, nm, (int)sizeof nm);
        CHECK(nl == 4 && memcmp(nm, "Alex", 4) == 0, "with the names");
    }
    // A damaged payload leaves the device on the game it was showing.
    {
        uint8_t t[2048];
        memcpy(t, bubble, (size_t)len);
        t[1] = 9;                                    // an unknown format
        const int before = wwi_view_turn(0);
        CHECK(wwi_adopt(t, len) == WW_MSG_EFORMAT, "a damaged bubble is refused");
        CHECK(wwi_view_turn(0) == before, "and changes nothing");
    }
    // Rule P through the bridge: a longer chain wins, and a chain that will not
    // decode never beats one that will.
    {
        uint8_t junk[16];
        memset(junk, 0, sizeof junk);
        CHECK(wwi_prefer(bubble, len, junk, (int)sizeof junk) < 0, "a real chain beats junk");
        CHECK(wwi_prefer(junk, (int)sizeof junk, bubble, len) > 0, "in either order");
        CHECK(wwi_prefer(junk, 4, junk, 4) == 0, "and junk does not rank against junk");
        CHECK(wwi_prefer(bubble, len, bubble, len) == 0, "a chain equals itself");
    }
    // The two clocks.
    CHECK(wwi_send_floor_remaining(100, 100) == WW_SEND_FLOOR_S, "the floor starts full");
    CHECK(wwi_send_floor_remaining(100, 110) == 0, "and is spent on time");
    CHECK(!wwi_may_carry(100, 159) && wwi_may_carry(100, 160), "the carry gate opens on the minute");

    // Seat identity over the adopted roster.
    CHECK(wwi_seat_on_board(-1, 0, 2, 0, (const uint8_t *)"Kim", 3) == 2, "found by name");
    CHECK(wwi_seat_on_board(5, 0, 2, 0, (const uint8_t *)"Kim", 3) == 2,
          "and a stale numeric cache does not beat the name");
    CHECK(wwi_seat_in_lobby(-1, 0, 0, 0, (const uint8_t *)"Ghost", 5) == -1,
          "a lobby gives a named stranger nothing");

    printf("%d passed, %d failed\n", n_pass, n_fail);
    return n_fail ? 1 : 0;
}
