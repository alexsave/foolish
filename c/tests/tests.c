// The werewolf kernel's suite. Every test here is written to FAIL first and
// then checked by mutation - break the line the test is about, confirm this
// binary goes red, put it back. The ledger of those mutations is in
// docs/NIGHT_KERNEL.md; a test nobody has seen fail is a test that proves
// nothing.
//
// Three of these are the ones the product lives or dies on, and they are marked
// HEADLINE where they sit:
//
//   1. a villager's night record is byte-indistinguishable from a wolf's to a
//      third seat;
//   2. the wolves' line is unreadable by a non-wolf viewer;
//   3. a skip carried by a later player beats the skipped player's own late
//      move under Rule P.

#include "../src/ww_game.h"
#include "../src/ww_view.h"
#include "../src/ww_wire.h"
#include "../src/deal_rng.h"
#include <stdio.h>
#include <string.h>

static int n_pass = 0;
static int n_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { n_pass++; } \
    else { n_fail++; fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

// A seed that is never all-zero (the wire refuses those) and is a pure function
// of one integer, so a test that names seed 41 names the same deal forever.
static void seed_of(uint8_t s[32], unsigned v) {
    for (int i = 0; i < 32; i++) s[i] = (uint8_t)(v * 2654435761u + (unsigned)i * 97u + 1u);
}

static int count_role(const WwGame *g, int role) {
    int n = 0;
    for (int i = 0; i < g->n_players; i++) if (g->role[i] == role) n++;
    return n;
}

static int first_seat_with(const WwGame *g, int role, int skip_a, int skip_b, int skip_c) {
    for (int i = 0; i < g->n_players; i++)
        if (g->role[i] == role && i != skip_a && i != skip_b && i != skip_c) return i;
    return -1;
}

// ------------------------------------------------------------ the deal ------

static void test_wolf_count_is_the_classic_ladder(void) {
    CHECK(ww_wolf_count(5) == 1, "5 players, one wolf");
    CHECK(ww_wolf_count(6) == 1, "6 players, one wolf");
    CHECK(ww_wolf_count(7) == 2, "7 players, two wolves");
    CHECK(ww_wolf_count(9) == 2, "9 players, two wolves");
    CHECK(ww_wolf_count(10) == 3, "10 players, three wolves");
    CHECK(ww_wolf_count(4) == 0, "4 is not a table");
    CHECK(ww_wolf_count(11) == 0, "11 is not a table");
}

static void test_deal_refuses_a_table_outside_five_to_ten(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 1);
    CHECK(ww_deal(&g, s, 4) == WW_ECOUNT, "4 players refused");
    CHECK(ww_deal(&g, s, 11) == WW_ECOUNT, "11 players refused");
    CHECK(ww_deal(&g, s, 5) == WW_OK, "5 players dealt");
    CHECK(ww_deal(&g, s, 10) == WW_OK, "10 players dealt");
}

static void test_deal_hands_out_one_seer_and_the_wolf_count(void) {
    for (int n = WW_MIN_PLAYERS; n <= WW_MAX_PLAYERS; n++) {
        for (unsigned v = 1; v <= 40; v++) {
            WwGame g; uint8_t s[32]; seed_of(s, v * 31u + (unsigned)n);
            CHECK(ww_deal(&g, s, n) == WW_OK, "dealt");
            CHECK(count_role(&g, WW_ROLE_SEER) == 1, "exactly one seer");
            CHECK(count_role(&g, WW_ROLE_WOLF) == ww_wolf_count(n), "the wolf count");
            CHECK(count_role(&g, WW_ROLE_VILLAGER) == n - 1 - ww_wolf_count(n),
                  "the rest are villagers");
            CHECK(ww_alive_count(&g) == n, "everyone starts alive");
            CHECK(g.phase == WW_PHASE_NIGHT, "a dealt game is on night one");
        }
    }
}

static void test_the_deal_is_a_function_of_the_seed(void) {
    WwGame a, b; uint8_t s[32];
    seed_of(s, 7);
    ww_deal(&a, s, 8);
    ww_deal(&b, s, 8);
    CHECK(memcmp(a.role, b.role, sizeof a.role) == 0, "same seed, same roles");
    // And it is not a constant: over forty seeds the wolf set must move. A deal
    // that ignored the seed would pass the line above and fail this one.
    int moved = 0;
    for (unsigned v = 8; v < 48 && !moved; v++) {
        uint8_t t[32]; seed_of(t, v);
        ww_deal(&b, t, 8);
        if (memcmp(a.role, b.role, sizeof a.role) != 0) moved = 1;
    }
    CHECK(moved, "a different seed deals a different table");
}

static void test_the_seed_reaches_every_seat(void) {
    // Fisher-Yates over a bag can be got wrong in a way that still produces the
    // right counts: an off-by-one in the loop bound pins the last seat. Over 600
    // deals every seat must have been a wolf at least once.
    int was_wolf[WW_MAX_PLAYERS] = {0};
    for (unsigned v = 1; v <= 600; v++) {
        WwGame g; uint8_t s[32]; seed_of(s, v);
        ww_deal(&g, s, 8);
        for (int i = 0; i < 8; i++) if (g.role[i] == WW_ROLE_WOLF) was_wolf[i] = 1;
    }
    int all = 1;
    for (int i = 0; i < 8; i++) if (!was_wolf[i]) all = 0;
    CHECK(all, "every seat can be dealt a wolf");
}

// ------------------------------------------------------------ the night -----

// Send for every living seat except those in `skip`, in rotation order, each
// targeting `target` (or the first other living seat when they ARE the target).
// Returns how many records were accepted.
static int send_all_but(WwGame *g, int skip_a, int skip_b, int target) {
    uint8_t order[WW_MAX_PLAYERS];
    const int n = ww_night_order(g, g->night, order);
    int sent = 0;
    for (int i = 0; i < n; i++) {
        const int s = order[i];
        if (s == skip_a || s == skip_b) continue;
        int t = target;
        if (t == s || t == WW_NO_SEAT) {
            t = WW_NO_SEAT;
            for (int j = 0; j < n; j++) if (order[j] != s) { t = order[j]; break; }
        }
        if (ww_night_act(g, s, t, 0, 0, 0) == WW_OK) sent++;
    }
    return sent;
}

static void test_the_night_needs_every_living_seat(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 11);
    ww_deal(&g, s, 6);
    uint8_t order[WW_MAX_PLAYERS];
    const int n = ww_night_order(&g, 0, order);
    CHECK(n == 6, "six living seats in the rotation");
    for (int i = 0; i < n - 1; i++) {
        CHECK(ww_night_act(&g, order[i], order[(i + 1) % n], 0, 0, 0) == WW_OK, "accepted");
        CHECK(g.phase == WW_PHASE_NIGHT, "the night is still open");
    }
    CHECK(ww_night_act(&g, order[n - 1], order[0], 0, 0, 0) == WW_OK, "the last one");
    CHECK(g.phase == WW_PHASE_DAY, "the last record ends the night");
    CHECK(g.turn == 6, "six accepted records");
}

static void test_the_night_refuses_a_second_record_from_one_seat(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 12);
    ww_deal(&g, s, 6);
    CHECK(ww_night_act(&g, 0, 1, 0, 0, 0) == WW_OK, "first record");
    CHECK(ww_night_act(&g, 0, 2, 0, 0, 0) == WW_EDUP, "one record per seat per night");
    CHECK(g.turn == 1, "a refusal is not an action");
}

static void test_the_night_refuses_a_bad_target(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 13);
    ww_deal(&g, s, 6);
    CHECK(ww_night_act(&g, 0, 0, 0, 0, 0) == WW_ETARGET, "nobody picks themselves");
    CHECK(ww_night_act(&g, 0, 9, 0, 0, 0) == WW_ETARGET, "no seat 9 at a six-table");
    CHECK(ww_night_act(&g, 9, 0, 0, 0, 0) == WW_ESEAT, "seat 9 cannot send either");
    CHECK(ww_night_act(&g, 0, WW_NO_SEAT, 0, 0, 0) == WW_OK, "no choice is a choice");
}

static void test_only_a_wolf_may_carry_a_line(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 14);
    ww_deal(&g, s, 8);
    const int wolf = first_seat_with(&g, WW_ROLE_WOLF, -1, -1, -1);
    const int vill = first_seat_with(&g, WW_ROLE_VILLAGER, -1, -1, -1);
    const int seer = first_seat_with(&g, WW_ROLE_SEER, -1, -1, -1);
    CHECK(wolf >= 0 && vill >= 0 && seer >= 0, "the deal has all three roles");
    CHECK(ww_night_act(&g, vill, wolf, "hello", 5, 0) == WW_ECHAT, "a villager cannot");
    CHECK(ww_night_act(&g, seer, wolf, "hello", 5, 0) == WW_ECHAT, "nor the seer");
    // Refused, not silently dropped: a client that believes it sent a line and
    // did not is a client that will send a second bubble, and a second bubble is
    // the one thing this design cannot afford.
    CHECK(g.turn == 0, "and neither attempt was accepted");
    char big[WW_CHAT_MAX + 1];
    memset(big, 'x', sizeof big);
    CHECK(ww_night_act(&g, wolf, vill, big, WW_CHAT_MAX + 1, 0) == WW_ECHAT, "over the cap");
    CHECK(ww_night_act(&g, wolf, vill, big, WW_CHAT_MAX, 0) == WW_OK, "at the cap");
}

static void test_the_decider_is_the_last_wolf_in_the_rotation(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 15);
    ww_deal(&g, s, 9);
    uint8_t order[WW_MAX_PLAYERS];
    for (int night = 0; night < 4; night++) {
        g.night = (uint8_t)night;
        const int n = ww_night_order(&g, night, order);
        int last = WW_NO_SEAT;
        for (int i = 0; i < n; i++) if (g.role[order[i]] == WW_ROLE_WOLF) last = order[i];
        CHECK(ww_night_decider(&g) == last, "the decider is the rotation's last wolf");
    }
}

static void test_the_rotation_moves_the_call(void) {
    // With two wolves the call has to alternate across the nights the rotation
    // sweeps past them. A decider that ignored the night would answer the same
    // seat every time.
    int seen_two = 0;
    for (unsigned v = 1; v <= 60 && !seen_two; v++) {
        WwGame g; uint8_t s[32]; seed_of(s, v);
        ww_deal(&g, s, 8);
        int a = -1, differs = 0;
        for (int night = 0; night < 8; night++) {
            g.night = (uint8_t)night;
            const int d = ww_night_decider(&g);
            if (a < 0) a = d; else if (d != a) differs = 1;
        }
        if (differs) seen_two = 1;
    }
    CHECK(seen_two, "the rotation hands the call to a different wolf");
}

static void test_the_kill_is_the_deciders_choice(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 16);
    ww_deal(&g, s, 8);
    const int d = ww_night_decider(&g);
    const int other = first_seat_with(&g, WW_ROLE_WOLF, d, -1, -1);
    CHECK(d != WW_NO_SEAT && other >= 0, "two wolves");
    const int mine = first_seat_with(&g, WW_ROLE_VILLAGER, -1, -1, -1);
    const int theirs = first_seat_with(&g, WW_ROLE_VILLAGER, mine, -1, -1);
    CHECK(mine >= 0 && theirs >= 0 && mine != theirs, "two villagers to choose between");
    // The other wolf names `theirs`; the decider names `mine`. The decider wins.
    CHECK(ww_night_act(&g, other, theirs, "take that one", 13, 0) == WW_OK, "other wolf");
    CHECK(ww_night_act(&g, d, mine, "no, this one", 12, 0) == WW_OK, "the decider");
    send_all_but(&g, other, d, mine);
    CHECK(g.phase == WW_PHASE_DAY, "the night resolved");
    CHECK(g.victim[0] == mine, "the decider's choice is the kill");
    CHECK(!ww_is_alive(&g, mine), "and the victim is dead");
    CHECK(ww_is_alive(&g, theirs), "the other wolf's pick lived");
}

// HEADLINE 3's companion on the kernel side: you cannot hold the night open for
// the deciding wolf, because waiting on one player announces that player.
static void test_a_skipped_decider_loses_the_kill_to_the_last_wolf_who_chose(void) {
    // Ten players, so there are THREE wolves and two of them can choose
    // differently while the decider sleeps. Two is the point: with only one
    // other wolf, "the most recent who chose" and "any wolf who chose" are the
    // same seat, and the test would not be able to tell them apart.
    WwGame g; uint8_t s[32];
    uint8_t order[WW_MAX_PLAYERS];
    int n = 0, d = WW_NO_SEAT, found = 0;
    for (unsigned v = 1; v <= 400 && !found; v++) {
        seed_of(s, v);
        ww_deal(&g, s, 10);
        n = ww_night_order(&g, 0, order);
        d = ww_night_decider(&g);
        // The decider must have somebody behind him to carry his pass, and the
        // other two wolves must both be ahead of him so their choices land in a
        // known order.
        if (d == WW_NO_SEAT || order[n - 1] == d) continue;
        int wolves_before = 0, ok = 1;
        for (int i = 0; i < n; i++) {
            if (g.role[order[i]] != WW_ROLE_WOLF) continue;
            if (order[i] == d) { if (wolves_before != 2) ok = 0; break; }
            wolves_before++;
        }
        if (ok && wolves_before == 2) found = 1;
    }
    CHECK(found, "a ten-table whose deciding wolf is last of three and not last in line");

    int early = -1, late = -1;
    for (int i = 0; i < n; i++) {
        if (g.role[order[i]] != WW_ROLE_WOLF || order[i] == d) continue;
        if (early < 0) early = order[i]; else late = order[i];
    }
    const int his = first_seat_with(&g, WW_ROLE_VILLAGER, order[n - 1], -1, -1);
    const int hers = first_seat_with(&g, WW_ROLE_VILLAGER, order[n - 1], his, -1);
    CHECK(early >= 0 && late >= 0 && his >= 0 && hers >= 0, "two wolves and two marks");

    // The two waking wolves disagree, in rotation order. The decider never
    // sends. The last seat in the rotation carries everyone late.
    CHECK(ww_night_act(&g, early, his, "take his", 8, 0) == WW_OK, "the earlier wolf");
    CHECK(ww_night_act(&g, late, hers, "no, hers", 8, 0) == WW_OK, "the later wolf");
    for (int i = 0; i < n - 1; i++) {
        const int who = order[i];
        if (who == d || who == early || who == late) continue;
        CHECK(ww_night_act(&g, who, who == his ? hers : his, 0, 0, 0) == WW_OK, "villager");
    }
    CHECK(g.phase == WW_PHASE_NIGHT, "still waiting on the decider and the carrier");
    CHECK(ww_night_act(&g, order[n - 1], his, 0, 0, 1) == WW_OK, "the carry");
    CHECK(g.phase == WW_PHASE_DAY, "the carry ended the night without him");

    int d_was_carried = 0;
    for (int i = 0; i < g.n_records; i++)
        if (g.rec[i].seat == d && (g.rec[i].flags & WW_REC_AUTO_PASS)) d_was_carried = 1;
    CHECK(d_was_carried, "the decider was passed for");
    CHECK(g.victim[0] == hers, "the kill fell to the MOST RECENT wolf who chose");
    CHECK(g.victim[0] != his, "not to the first one who spoke");
    // And from outside nothing happened out of the ordinary: the night ended the
    // way every night ends, with one record per living seat.
    CHECK(g.turn == n, "one record per living seat, as always");
}

static void test_a_night_with_no_wolf_choice_kills_nobody(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 21);
    ww_deal(&g, s, 8);
    uint8_t order[WW_MAX_PLAYERS];
    const int n = ww_night_order(&g, 0, order);
    for (int i = 0; i < n; i++) {
        const int who = order[i];
        const int t = (g.role[who] == WW_ROLE_WOLF) ? WW_NO_SEAT : order[(i + 1) % n];
        CHECK(ww_night_act(&g, who, t, 0, 0, 0) == WW_OK, "sent");
    }
    CHECK(g.phase == WW_PHASE_DAY, "the night still ended");
    CHECK(g.victim[0] == WW_NO_SEAT, "nobody died");
    CHECK(ww_alive_count(&g) == 8, "the table is intact");
}

static void test_the_carry_only_reaches_backwards(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 17);
    ww_deal(&g, s, 7);
    uint8_t order[WW_MAX_PLAYERS];
    CHECK(ww_night_order(&g, 0, order) == 7, "seven in the rotation");
    // The FIRST seat in the rotation carries: there is nobody ahead of it, so
    // the carry adds nothing. If carry reached forward, this one call would end
    // the night by itself, which is the failure the rotation exists to prevent.
    CHECK(ww_night_act(&g, order[0], order[1], 0, 0, 1) == WW_OK, "the first seat carries");
    CHECK(g.turn == 1, "and carried nobody");
    CHECK(g.phase == WW_PHASE_NIGHT, "one player cannot end the night alone");
    // The third seat carries: the second is late, so exactly one pass rides along.
    CHECK(ww_night_act(&g, order[2], order[0], 0, 0, 1) == WW_OK, "the third seat carries");
    CHECK(g.turn == 3, "one carried pass plus its own move");
    CHECK((g.rec[1].flags & WW_REC_AUTO_PASS) && g.rec[1].seat == order[1],
          "the pass names the seat that was late");
    CHECK(!(g.rec[2].flags & WW_REC_AUTO_PASS) && g.rec[2].seat == order[2],
          "and the carrier's own move follows it");
    CHECK(g.phase == WW_PHASE_NIGHT, "four seats still to send");
}

static void test_a_carried_seat_cannot_then_send(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 18);
    ww_deal(&g, s, 7);
    uint8_t order[WW_MAX_PLAYERS];
    (void)ww_night_order(&g, 0, order);
    CHECK(ww_night_act(&g, order[1], order[0], 0, 0, 1) == WW_OK, "seat two carries seat one");
    CHECK(ww_night_act(&g, order[0], order[2], 0, 0, 0) == WW_EDUP,
          "the passed seat has already spent its record");
}

// ------------------------------------------------------- the day, and ends ---

static void test_a_lynch_starts_the_next_night(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 19);
    ww_deal(&g, s, 9);
    const int mark = first_seat_with(&g, WW_ROLE_VILLAGER, -1, -1, -1);
    send_all_but(&g, -1, -1, mark);
    CHECK(g.phase == WW_PHASE_DAY, "night one resolved");
    const int survivor = first_seat_with(&g, WW_ROLE_VILLAGER, mark, -1, -1);
    CHECK(ww_day_lynch(&g, survivor) == WW_OK, "the table votes");
    CHECK(g.night == 1, "night two");
    CHECK(g.phase == WW_PHASE_NIGHT, "and it is night again");
    CHECK(g.lynched[0] == survivor, "the day is on the record");
    CHECK(!ww_is_alive(&g, survivor), "the lynched seat is out");
    CHECK(ww_night_act(&g, survivor, mark, 0, 0, 0) == WW_ESEAT, "and cannot send again");
}

static void test_the_village_wins_when_the_pack_is_out(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 20);
    ww_deal(&g, s, 5);              // one wolf
    const int wolf = first_seat_with(&g, WW_ROLE_WOLF, -1, -1, -1);
    send_all_but(&g, -1, -1, WW_NO_SEAT);
    CHECK(g.phase == WW_PHASE_DAY, "the night resolved");
    CHECK(ww_day_lynch(&g, wolf) == WW_OK, "the table gets it right");
    CHECK(g.phase == WW_PHASE_OVER, "the game is over");
    CHECK(g.winner == WW_TEAM_VILLAGE, "the village won");
}

static void test_the_wolves_win_on_parity(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 22);
    ww_deal(&g, s, 5);              // one wolf, four others
    const int wolf = first_seat_with(&g, WW_ROLE_WOLF, -1, -1, -1);
    // Three nights of kills and no lynches takes 4 others down to 1, which is
    // parity with one wolf.
    for (int night = 0; night < 3 && g.phase != WW_PHASE_OVER; night++) {
        uint8_t order[WW_MAX_PLAYERS];
        const int n = ww_night_order(&g, g.night, order);
        int mark = WW_NO_SEAT;
        for (int i = 0; i < n; i++) if (order[i] != wolf) { mark = order[i]; break; }
        for (int i = 0; i < n; i++) {
            const int who = order[i];
            const int t = (who == wolf) ? mark : (order[i] == mark ? wolf : mark);
            ww_night_act(&g, who, t == who ? WW_NO_SEAT : t, 0, 0, 0);
        }
        if (g.phase == WW_PHASE_DAY && g.winner == WW_TEAM_NONE) ww_day_lynch(&g, WW_NO_SEAT);
    }
    CHECK(g.phase == WW_PHASE_OVER, "parity ends it");
    CHECK(g.winner == WW_TEAM_WOLVES, "the wolves won");
    CHECK(ww_is_alive(&g, wolf), "with the wolf still standing");
}

// ------------------------------------------------------------- the views ----

// The two games a third seat must not be able to tell apart: the same deal, with
// one wolf's role and one villager's role exchanged.
typedef struct { WwGame a, b; int wolf, vill, third, mark; } Pair;

static int build_pair(Pair *p) {
    uint8_t s[32];
    for (unsigned v = 1; v <= 400; v++) {
        seed_of(s, v);
        WwGame g;
        if (ww_deal(&g, s, 8) != WW_OK) continue;
        const int w = first_seat_with(&g, WW_ROLE_WOLF, -1, -1, -1);
        const int a = first_seat_with(&g, WW_ROLE_VILLAGER, -1, -1, -1);
        const int t = first_seat_with(&g, WW_ROLE_VILLAGER, a, -1, -1);
        const int m = first_seat_with(&g, WW_ROLE_VILLAGER, a, t, -1);
        if (w < 0 || a < 0 || t < 0 || m < 0) continue;
        p->wolf = w; p->vill = a; p->third = t; p->mark = m;
        p->a = g;
        p->b = g;
        p->b.role[w] = g.role[a];      // the counterfactual: it was the OTHER one
        p->b.role[a] = g.role[w];
        return 1;
    }
    return 0;
}

// Play one identical night into `g`, with the wolf line coming from `speaker`.
static void play_night_with_speaker(WwGame *g, int speaker, int mark, const char *line) {
    uint8_t order[WW_MAX_PLAYERS];
    const int n = ww_night_order(g, g->night, order);
    for (int i = 0; i < n; i++) {
        const int who = order[i];
        const int t = (who == mark) ? order[(i + 1) % n] : mark;
        if (who == speaker) ww_night_act(g, who, t, line, (int)strlen(line), 0);
        else                ww_night_act(g, who, t, 0, 0, 0);
    }
}

// HEADLINE 1. A wolf's record and a villager's record are the SAME BYTES to a
// third seat. Not "the role field is blank" - the whole view, byte for byte,
// with the two roles exchanged underneath it.
static void test_a_wolfs_record_is_byte_identical_to_a_villagers(void) {
    Pair p;
    CHECK(build_pair(&p), "a deal with a wolf and three villagers");
    const char *line = "the quiet one is watching me";
    play_night_with_speaker(&p.a, p.wolf, p.mark, line);   // A: the wolf speaks
    play_night_with_speaker(&p.b, p.vill, p.mark, line);   // B: the villager-now-wolf speaks
    CHECK(p.a.phase == WW_PHASE_DAY && p.b.phase == WW_PHASE_DAY, "both nights resolved");
    CHECK(p.a.victim[0] == p.b.victim[0], "the same seat died in both");

    unsigned char va[WW_VIEW_MAX], vb[WW_VIEW_MAX];
    const int la = ww_view_put(&p.a, p.third, va);
    const int lb = ww_view_put(&p.b, p.third, vb);
    CHECK(la == lb, "the third seat's view is the same LENGTH in both");
    CHECK(la == lb && memcmp(va, vb, (size_t)la) == 0,
          "the third seat's view is byte-identical with the roles exchanged");

    // And the two games are genuinely different, so the assertion above is not
    // comparing a game with itself. The wolf's OWN view must differ: in A he
    // sees the pack, in B he is a villager who sees nothing.
    const int wa = ww_view_put(&p.a, p.wolf, va);
    const int wb = ww_view_put(&p.b, p.wolf, vb);
    CHECK(wa != wb || memcmp(va, vb, (size_t)wa) != 0,
          "the wolf's own view DOES differ between the two games");
}

// A byte run search, so the test asks the question a curious player would:
// is the line anywhere in what my phone was given?
static int contains(const unsigned char *hay, int n, const char *needle) {
    const int m = (int)strlen(needle);
    for (int i = 0; i + m <= n; i++)
        if (memcmp(hay + i, needle, (size_t)m) == 0) return 1;
    return 0;
}

// HEADLINE 2. The wolves' channel is unreadable by a non-wolf viewer, and the
// test fails in both directions: absent from a villager's view, PRESENT in a
// wolf's. A masking bug that blanked the channel for everybody would pass the
// first half and fail the second.
static void test_the_wolf_line_is_absent_from_a_non_wolfs_view(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 23);
    ww_deal(&g, s, 8);
    const int wolf = ww_night_decider(&g);
    const int pack = first_seat_with(&g, WW_ROLE_WOLF, wolf, -1, -1);
    const int vill = first_seat_with(&g, WW_ROLE_VILLAGER, -1, -1, -1);
    const int seer = first_seat_with(&g, WW_ROLE_SEER, -1, -1, -1);
    CHECK(wolf != WW_NO_SEAT && pack >= 0 && vill >= 0 && seer >= 0, "the cast");
    const char *line = "take the seer tonight";
    CHECK(ww_night_act(&g, wolf, vill, line, (int)strlen(line), 0) == WW_OK, "the wolf speaks");

    unsigned char v[WW_VIEW_MAX];
    int n = ww_view_put(&g, vill, v);
    CHECK(!contains(v, n, line), "a villager's view does not carry the line");
    n = ww_view_put(&g, seer, v);
    CHECK(!contains(v, n, line), "nor the seer's");
    n = ww_view_put(&g, WW_VIEW_SPECTATOR, v);
    CHECK(!contains(v, n, line), "nor a spectator's");
    n = ww_view_put(&g, pack, v);
    CHECK(contains(v, n, line), "the other wolf CAN read it");
    n = ww_view_put(&g, wolf, v);
    CHECK(contains(v, n, line), "and so can its author");
}

// A reader for the view layout, so these tests assert FIELDS and not lengths.
// Deliberately written out from ww_view.h rather than shared with ww_view.c: a
// test that reuses the writer's own walk cannot catch the writer getting the
// layout wrong, which is the whole thing a bridge on the other side will hit.
typedef struct {
    int ok;
    int version, phase, n_players, night, alive, turn, winner, my_seat, my_role;
    int n_roles;   uint8_t role_seat[WW_MAX_PLAYERS], role_val[WW_MAX_PLAYERS];
    int n_rows;    uint8_t row_seat[WW_MAX_PLAYERS], row_sent[WW_MAX_PLAYERS];
    int have_own, own_target, decider;
    int n_chat;    uint8_t chat_seat[WW_MAX_PLAYERS], chat_len[WW_MAX_PLAYERS];
    char chat[WW_MAX_PLAYERS][WW_CHAT_MAX];
    int n_readings; uint8_t read_seat[WW_MAX_PLAYERS], read_team[WW_MAX_PLAYERS];
    int n_history;  uint8_t hist_victim[WW_MAX_NIGHTS], hist_lynched[WW_MAX_NIGHTS];
    int len;
} ViewRead;

static ViewRead view_read(const unsigned char *v, int n) {
    ViewRead r;
    memset(&r, 0, sizeof r);
    int q = 0;
    r.version = v[q++]; r.phase = v[q++]; r.n_players = v[q++]; r.night = v[q++];
    r.alive = v[q] | (v[q + 1] << 8); q += 2;
    r.turn  = v[q] | (v[q + 1] << 8); q += 2;
    r.winner = v[q++]; r.my_seat = v[q++]; r.my_role = v[q++];
    r.n_roles = v[q++];
    for (int i = 0; i < r.n_roles; i++) { r.role_seat[i] = v[q++]; r.role_val[i] = v[q++]; }
    r.n_rows = v[q++];
    for (int i = 0; i < r.n_rows; i++) { r.row_seat[i] = v[q++]; r.row_sent[i] = v[q++]; }
    r.have_own = v[q++]; r.own_target = v[q++]; r.decider = v[q++];
    r.n_chat = v[q++];
    for (int i = 0; i < r.n_chat; i++) {
        r.chat_seat[i] = v[q++];
        r.chat_len[i] = v[q++];
        memcpy(r.chat[i], v + q, r.chat_len[i]);
        q += r.chat_len[i];
    }
    r.n_readings = v[q++];
    for (int i = 0; i < r.n_readings; i++) { r.read_seat[i] = v[q++]; r.read_team[i] = v[q++]; }
    r.n_history = v[q++];
    for (int i = 0; i < r.n_history; i++) { q++; r.hist_victim[i] = v[q++]; r.hist_lynched[i] = v[q++]; }
    r.len = q;
    r.ok = (q == n);
    return r;
}

static void test_a_non_wolf_is_never_told_who_decides(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 24);
    ww_deal(&g, s, 8);
    const int d = ww_night_decider(&g);
    const int pack = first_seat_with(&g, WW_ROLE_WOLF, d, -1, -1);
    const int vill = first_seat_with(&g, WW_ROLE_VILLAGER, -1, -1, -1);
    const int seer = first_seat_with(&g, WW_ROLE_SEER, -1, -1, -1);
    unsigned char v[WW_VIEW_MAX];

    ViewRead r = view_read(v, ww_view_put(&g, vill, v));
    CHECK(r.ok, "a villager's view parses exactly");
    CHECK(r.decider == WW_NO_SEAT, "a villager is not told who decides");
    CHECK(r.n_roles == 1 && r.role_seat[0] == vill, "and knows only their own role");
    r = view_read(v, ww_view_put(&g, seer, v));
    CHECK(r.decider == WW_NO_SEAT, "nor is the seer");
    r = view_read(v, ww_view_put(&g, WW_VIEW_SPECTATOR, v));
    CHECK(r.decider == WW_NO_SEAT, "nor a spectator");
    r = view_read(v, ww_view_put(&g, d, v));
    CHECK(r.decider == d, "the decider is told it is him");
    CHECK(r.n_roles == 2, "and sees the pack");
    r = view_read(v, ww_view_put(&g, pack, v));
    CHECK(r.decider == d, "and so is the other wolf");
}

static void test_the_seers_reading_is_only_the_seers(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 25);
    ww_deal(&g, s, 8);
    const int seer = first_seat_with(&g, WW_ROLE_SEER, -1, -1, -1);
    const int wolf = first_seat_with(&g, WW_ROLE_WOLF, -1, -1, -1);
    const int vill = first_seat_with(&g, WW_ROLE_VILLAGER, -1, -1, -1);
    CHECK(ww_night_act(&g, seer, wolf, 0, 0, 0) == WW_OK, "the seer asks about a wolf");

    unsigned char v[WW_VIEW_MAX];
    ViewRead r = view_read(v, ww_view_put(&g, seer, v));
    CHECK(r.ok, "the seer's view parses exactly");
    CHECK(r.n_readings == 1, "one answer");
    CHECK(r.read_seat[0] == wolf, "about the seat they asked about");
    CHECK(r.read_team[0] == WW_TEAM_WOLVES, "and the answer is the truth");
    // The answer lands when the question is sent, not at dawn: the question was
    // asked and there is nobody to wait for.
    CHECK(g.phase == WW_PHASE_NIGHT, "while the night is still open");

    r = view_read(v, ww_view_put(&g, vill, v));
    CHECK(r.n_readings == 0, "a villager gets no readings");
    r = view_read(v, ww_view_put(&g, wolf, v));
    CHECK(r.n_readings == 0, "and neither does the wolf who was read");
    r = view_read(v, ww_view_put(&g, WW_VIEW_SPECTATOR, v));
    CHECK(r.n_readings == 0, "nor a spectator");

    // A villager who picked the same seat learns nothing from it, which is what
    // makes the seer's question indistinguishable from a dream.
    CHECK(ww_night_act(&g, vill, wolf, 0, 0, 0) == WW_OK, "a villager dreams of the same seat");
    r = view_read(v, ww_view_put(&g, vill, v));
    CHECK(r.n_readings == 0, "and is told nothing");
    CHECK(r.have_own == 1 && r.own_target == wolf, "beyond what they themselves picked");
}

static void test_a_third_seat_sees_only_that_a_seat_sent(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 29);
    ww_deal(&g, s, 8);
    uint8_t order[WW_MAX_PLAYERS];
    const int n = ww_night_order(&g, 0, order);
    const int wolf = ww_night_decider(&g);
    const int vill = first_seat_with(&g, WW_ROLE_VILLAGER, -1, -1, -1);
    const int third = first_seat_with(&g, WW_ROLE_VILLAGER, vill, -1, -1);
    CHECK(wolf != third && vill != third, "a genuine third seat");
    CHECK(ww_night_act(&g, wolf, vill, "this one", 8, 0) == WW_OK, "the wolf sends");
    CHECK(ww_night_act(&g, vill, wolf, 0, 0, 0) == WW_OK, "the villager sends");

    unsigned char v[WW_VIEW_MAX];
    ViewRead r = view_read(v, ww_view_put(&g, third, v));
    CHECK(r.ok && r.n_rows == 8, "one row per seat");
    int wolf_row = -1, vill_row = -1;
    for (int i = 0; i < r.n_rows; i++) {
        if (r.row_seat[i] == wolf) wolf_row = i;
        if (r.row_seat[i] == vill) vill_row = i;
    }
    CHECK(wolf_row >= 0 && vill_row >= 0, "both rows present");
    CHECK(r.row_sent[wolf_row] == WW_SENT_YES, "the wolf sent");
    CHECK(r.row_sent[vill_row] == WW_SENT_YES, "the villager sent");
    CHECK(r.row_sent[wolf_row] == r.row_sent[vill_row], "and the rows say the same thing");
    CHECK(r.n_chat == 0, "no channel");
    // A seat that has not sent reads as not sent, so the screen can say who the
    // night is still waiting on - which is safe precisely because it is everyone.
    int unsent = 0;
    for (int i = 0; i < r.n_rows; i++) if (r.row_sent[i] == WW_SENT_NO) unsent++;
    CHECK(unsent == n - 2, "and the rest have not");
}

static void test_a_carried_seat_reads_as_carried(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 30);
    ww_deal(&g, s, 8);
    uint8_t order[WW_MAX_PLAYERS];
    (void)ww_night_order(&g, 0, order);
    CHECK(ww_night_act(&g, order[1], order[0], 0, 0, 1) == WW_OK, "seat two carries seat one");
    unsigned char v[WW_VIEW_MAX];
    const ViewRead r = view_read(v, ww_view_put(&g, order[3], v));
    CHECK(r.ok, "parses");
    for (int i = 0; i < r.n_rows; i++) {
        if (r.row_seat[i] == order[0]) CHECK(r.row_sent[i] == WW_SENT_CARRIED, "carried");
        else if (r.row_seat[i] == order[1]) CHECK(r.row_sent[i] == WW_SENT_YES, "sent");
    }
    // Carried is published on purpose: the carrier's own bubble already says
    // whose passes it brought, so hiding it here would hide nothing and would
    // leave the screen unable to explain why a seat is done without sending.
}

static void test_a_dead_seats_role_is_public(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 26);
    ww_deal(&g, s, 8);
    const int mark = first_seat_with(&g, WW_ROLE_VILLAGER, -1, -1, -1);
    const int other = first_seat_with(&g, WW_ROLE_VILLAGER, mark, -1, -1);
    const int before = ww_view_measure(&g, other);
    send_all_but(&g, -1, -1, mark);
    CHECK(!ww_is_alive(&g, mark), "the marked seat died");
    const int after = ww_view_measure(&g, other);
    // One more role row (2 bytes) than before. The night's rows did not change
    // count - the view lists every seat, alive or not.
    CHECK(after == before + 2, "a death publishes exactly one role");
}

static void test_a_spectator_learns_no_role(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 27);
    ww_deal(&g, s, 8);
    unsigned char v[WW_VIEW_MAX];
    const ViewRead r = view_read(v, ww_view_put(&g, WW_VIEW_SPECTATOR, v));
    CHECK(r.ok, "a spectator still gets a view that parses");
    CHECK(r.my_seat == WW_NO_SEAT, "with no seat");
    CHECK(r.my_role == WW_ROLE_UNKNOWN, "and no role");
    CHECK(r.n_roles == 0, "and not one role row");
    CHECK(r.n_chat == 0 && r.n_readings == 0 && r.decider == WW_NO_SEAT, "and nothing private");
    CHECK(r.n_rows == 8, "but the public board is all there");
}

static void test_a_dead_wolf_loses_the_channel(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 28);
    ww_deal(&g, s, 8);
    const int d = ww_night_decider(&g);
    const int pack = first_seat_with(&g, WW_ROLE_WOLF, d, -1, -1);
    const int mark = first_seat_with(&g, WW_ROLE_VILLAGER, -1, -1, -1);
    const int spare = first_seat_with(&g, WW_ROLE_VILLAGER, mark, -1, -1);
    CHECK(pack >= 0 && mark >= 0 && spare >= 0, "two wolves and two villagers");
    // Night one takes `mark`, then the table lynches the other wolf, so the pack
    // is down to one - and the corpse must not be able to read what he says next.
    send_all_but(&g, -1, -1, mark);
    CHECK(g.victim[0] == mark, "night one took the marked villager");
    CHECK(ww_day_lynch(&g, pack) == WW_OK, "the table lynches a wolf");
    CHECK(g.phase == WW_PHASE_NIGHT, "and night two begins");
    const int live = ww_night_decider(&g);
    CHECK(live == d, "the surviving wolf decides");
    const char *line = "they got you";
    CHECK(ww_night_act(&g, live, spare, line, (int)strlen(line), 0) == WW_OK, "he speaks");
    unsigned char v[WW_VIEW_MAX];
    int n = ww_view_put(&g, pack, v);
    CHECK(!contains(v, n, line), "the dead wolf cannot read the channel");
    n = ww_view_put(&g, live, v);
    CHECK(contains(v, n, line), "but he can read his own line");
}

static void test_view_measure_matches_what_put_writes(void) {
    for (unsigned seed = 1; seed <= 20; seed++) {
        WwGame g; uint8_t s[32]; seed_of(s, seed);
        ww_deal(&g, s, 9);
        const int wolf = ww_night_decider(&g);
        CHECK(ww_night_act(&g, wolf, (wolf + 1) % 9, "aaaa", 4, 0) == WW_OK, "a line");
        for (int viewer = WW_VIEW_SPECTATOR; viewer < 9; viewer++) {
            unsigned char v[WW_VIEW_MAX];
            const int put = ww_view_put(&g, viewer, v);
            CHECK(put == ww_view_measure(&g, viewer), "measure agrees with put");
            CHECK(put <= WW_VIEW_MAX, "and fits the documented maximum");
        }
    }
}

// -------------------------------------------------------------- the wire ----

static void fill_roster(WwEnvelope *e, int n) {
    e->n_joins = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        e->joins[i].seat = (uint8_t)i;
        e->joins[i].name_len = 4;
        e->joins[i].name[0] = 'S'; e->joins[i].name[1] = 'e';
        e->joins[i].name[2] = 'a'; e->joins[i].name[3] = (char)('0' + i);
    }
}

// Seal `g` and encode it, returning the byte length.
static int seal_bytes(const WwGame *g, uint64_t game_id, const uint8_t seed[32],
                      unsigned char *out, int cap, WwEnvelope *e_out) {
    static unsigned char body[4096];
    WwEnvelope e;
    ww_envelope_init(&e);
    e.game_id = game_id;
    for (int i = 0; i < 32; i++) e.seed[i] = seed[i];
    fill_roster(&e, g->n_players);
    if (ww_msg_seal(&e, g, body, (int)sizeof body) != WW_MSG_EOK) return -1;
    const int n = ww_msg_encode(&e, out, cap);
    if (e_out) *e_out = e;
    return n;
}

static void test_the_envelope_round_trips(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 31);
    ww_deal(&g, s, 8);
    const int wolf = ww_night_decider(&g);
    ww_night_act(&g, wolf, (wolf + 1) % 8, "one bubble only", 15, 0);
    ww_night_act(&g, (wolf + 2) % 8, wolf, 0, 0, 0);

    unsigned char buf[4096];
    const int n = seal_bytes(&g, 0x0123456789abcdefULL, s, buf, (int)sizeof buf, 0);
    CHECK(n > 0, "sealed");
    WwEnvelope d;
    CHECK(ww_msg_decode(buf, n, &d) == WW_MSG_EOK, "decoded");
    CHECK(d.game_id == 0x0123456789abcdefULL, "the id survived");
    CHECK(d.n_players == 8 && d.night == 0 && d.round == 0, "the header survived");
    CHECK(d.turn == 2 && d.n_records == 2, "two records");
    CHECK(d.n_joins == 8, "and the roster");
    CHECK(ww_msg_measure(&d) == n, "measure agrees with encode");

    WwGame r;
    CHECK(ww_msg_replay(&d, &r) == WW_MSG_EOK, "replayed");
    CHECK(memcmp(r.role, g.role, sizeof r.role) == 0, "the same roles came back");
    CHECK(r.turn == g.turn && r.night == g.night && r.phase == g.phase, "the same state");
    CHECK(r.n_records == g.n_records, "and the same chain");
    for (int i = 0; i < g.n_records; i++) {
        CHECK(r.rec[i].seat == g.rec[i].seat, "record seat");
        CHECK(r.rec[i].target == g.rec[i].target, "record target");
        CHECK(r.rec[i].flags == g.rec[i].flags, "record flags");
        CHECK(r.rec[i].chat_len == g.rec[i].chat_len, "record line length");
        CHECK(memcmp(r.rec[i].chat, g.rec[i].chat, g.rec[i].chat_len) == 0, "record line");
    }
}

static void test_a_full_night_seals_inside_the_url_budget(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 32);
    ww_deal(&g, s, 10);
    uint8_t order[WW_MAX_PLAYERS];
    const int n = ww_night_order(&g, 0, order);
    char line[WW_CHAT_MAX];
    memset(line, 'w', sizeof line);
    for (int i = 0; i < n; i++) {
        const int who = order[i];
        const int t = order[(i + 1) % n];
        if (g.role[who] == WW_ROLE_WOLF) ww_night_act(&g, who, t, line, WW_CHAT_MAX, 0);
        else                             ww_night_act(&g, who, t, 0, 0, 0);
    }
    unsigned char buf[4096];
    const int len = seal_bytes(&g, 1, s, buf, (int)sizeof buf, 0);
    // The worst first night this game has: ten players, three wolves, every line
    // at the cap. base32 is 8 characters per 5 bytes.
    const int b32 = (len + 4) / 5 * 8;
    CHECK(len > 0 && len < 480, "a ten-player night seals small");
    CHECK(b32 < 1000, "and fits the URL budget as base32");
    if (n_fail) fprintf(stderr, "  (measured %d bytes, %d base32 chars)\n", len, b32);
}

static void test_decode_refuses_hostile_bytes(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 33);
    ww_deal(&g, s, 6);
    ww_night_act(&g, 0, 1, 0, 0, 0);
    unsigned char buf[4096];
    const int n = seal_bytes(&g, 9, s, buf, (int)sizeof buf, 0);
    WwEnvelope d;
    CHECK(ww_msg_decode(buf, n, &d) == WW_MSG_EOK, "the clean one decodes");

    // Truncation, at every length. None may be accepted and none may read past
    // the buffer - the ASan run is what proves the second half.
    for (int cut = 0; cut < n; cut++) {
        unsigned char t[4096];
        memcpy(t, buf, (size_t)cut);
        CHECK(ww_msg_decode(t, cut, &d) != WW_MSG_EOK, "a truncated envelope is refused");
    }
    // A trailing byte is as broken as a missing one.
    unsigned char longer[4096];
    memcpy(longer, buf, (size_t)n);
    longer[n] = 0;
    CHECK(ww_msg_decode(longer, n + 1, &d) == WW_MSG_ESHORT, "a trailing byte is refused");

    unsigned char t[4096];
    memcpy(t, buf, (size_t)n);
    t[0] = 0xF7;                        // the fork's magic
    CHECK(ww_msg_decode(t, n, &d) == WW_MSG_EMAGIC, "a Durak bubble is not this game");
    memcpy(t, buf, (size_t)n); t[1] = 2;
    CHECK(ww_msg_decode(t, n, &d) == WW_MSG_EFORMAT, "an unknown format is refused");
    memcpy(t, buf, (size_t)n); t[2] = 1;
    CHECK(ww_msg_decode(t, n, &d) == WW_MSG_EFIELD, "reserved flags mean reserved");
    memcpy(t, buf, (size_t)n); t[3] = 9;
    CHECK(ww_msg_decode(t, n, &d) == WW_MSG_EFIELD, "no such phase");
    memcpy(t, buf, (size_t)n); t[15] = 4;
    CHECK(ww_msg_decode(t, n, &d) == WW_MSG_EFIELD, "four is not a table");
    memcpy(t, buf, (size_t)n); t[16] = WW_MAX_NIGHTS;
    CHECK(ww_msg_decode(t, n, &d) == WW_MSG_EFIELD, "no eleventh night");
    memcpy(t, buf, (size_t)n);
    for (int i = 0; i < 32; i++) t[26 + i] = 0;
    CHECK(ww_msg_decode(t, n, &d) == WW_MSG_ESEED, "an all-zero seed is never a deal");
    memcpy(t, buf, (size_t)n); t[60] = WW_MAX_JOINS + 1;
    CHECK(ww_msg_decode(t, n, &d) == WW_MSG_EFIELD, "no eleventh seat in the roster");
}

static void test_decode_refuses_a_contradictory_record(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 34);
    ww_deal(&g, s, 6);
    ww_night_act(&g, 0, 1, 0, 0, 0);
    unsigned char buf[4096];
    const int n = seal_bytes(&g, 10, s, buf, (int)sizeof buf, 0);
    // The one record's frame is the last four bytes: seat, target, night, flags.
    const int f = n - 4;
    WwEnvelope d;
    unsigned char t[4096];
    memcpy(t, buf, (size_t)n); t[f + 3] = WW_REC_AUTO_PASS;
    CHECK(ww_msg_decode(t, n, &d) == WW_MSG_EFIELD, "a carried pass names nobody");
    memcpy(t, buf, (size_t)n); t[f + 3] = 0x80;
    CHECK(ww_msg_decode(t, n, &d) == WW_MSG_EFIELD, "an unknown flag bit is refused");
    memcpy(t, buf, (size_t)n); t[f + 2] = WW_MAX_NIGHTS;
    CHECK(ww_msg_decode(t, n, &d) == WW_MSG_EFIELD, "a record from night eleven");
    memcpy(t, buf, (size_t)n); t[f + 1] = WW_MAX_PLAYERS;
    CHECK(ww_msg_decode(t, n, &d) == WW_MSG_EFIELD, "a target off the table");
}

static void test_replay_refuses_a_header_that_lies(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 35);
    ww_deal(&g, s, 6);
    ww_night_act(&g, 0, 1, 0, 0, 0);
    ww_night_act(&g, 1, 0, 0, 0, 0);
    unsigned char buf[4096];
    const int n = seal_bytes(&g, 11, s, buf, (int)sizeof buf, 0);
    WwEnvelope d; WwGame r;
    CHECK(ww_msg_decode(buf, n, &d) == WW_MSG_EOK && ww_msg_replay(&d, &r) == WW_MSG_EOK,
          "the honest one replays");
    // The header states the game the body produces. A device that trusted the
    // header over the body would show a state nobody ever played.
    unsigned char t[4096];
    memcpy(t, buf, (size_t)n); t[16] = 3;                 // claim night four
    CHECK(ww_msg_decode(t, n, &d) == WW_MSG_EOK, "it still decodes");
    CHECK(ww_msg_replay(&d, &r) == WW_MSG_EREPLAY, "and refuses to replay");
    memcpy(t, buf, (size_t)n); t[3] = WW_PHASE_OVER;
    CHECK(ww_msg_decode(t, n, &d) == WW_MSG_EOK, "it still decodes");
    CHECK(ww_msg_replay(&d, &r) == WW_MSG_EREPLAY, "a claimed ending is refused");
    // Two records from the same seat: legal bytes, illegal game.
    const int f = n - 4;
    memcpy(t, buf, (size_t)n); t[f] = t[f - 4];
    CHECK(ww_msg_decode(t, n, &d) == WW_MSG_EOK, "it still decodes");
    CHECK(ww_msg_replay(&d, &r) == WW_MSG_EREPLAY, "but will not replay");

    // A CLAIMED `turn`, which is the one field worth forging. Rule P clause 2
    // prefers the longer chain and reads it off the header before anything is
    // replayed, so a bubble claiming turn 9000 would win every race in the
    // thread forever. It is worthless because it never replays.
    memcpy(t, buf, (size_t)n); t[12] = 0x28; t[13] = 0x23;      // turn = 9000
    WwChainKey forged, honest;
    CHECK(ww_chain_key(t, n, &forged) == WW_MSG_EOK, "the forgery makes a chain key");
    CHECK(ww_chain_key(buf, n, &honest) == WW_MSG_EOK, "and so does the real one");
    CHECK(ww_rule_p(&forged, &honest) < 0, "and it would indeed win the race");
    CHECK(ww_msg_decode(t, n, &d) == WW_MSG_EOK, "and it decodes");
    CHECK(ww_msg_replay(&d, &r) == WW_MSG_EREPLAY, "but it cannot be replayed, so it is never adopted");

    // A record that lies about its night. The night comes from the game rather
    // than the frame, so this does NOT change the state - which is the reason it
    // has to be refused: two byte strings replaying to one game make the digest
    // malleable, and the digest is Rule P's last tiebreak and every parent link.
    memcpy(t, buf, (size_t)n); t[f + 2] = 1;
    CHECK(ww_msg_decode(t, n, &d) == WW_MSG_EOK, "a lying night byte decodes");
    CHECK(ww_msg_replay(&d, &r) == WW_MSG_EREPLAY, "and is refused, so the digest is not malleable");
    {
        uint8_t da[SHA256_DIGEST_LEN], db[SHA256_DIGEST_LEN];
        ww_msg_digest(buf, n, da);
        ww_msg_digest(t, n, db);
        CHECK(memcmp(da, db, sizeof da) != 0, "the two byte strings do hash differently");
    }
}

static void test_seal_prunes_a_past_nights_line(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 36);
    ww_deal(&g, s, 8);
    char line[WW_CHAT_MAX];
    memset(line, 'z', sizeof line);
    uint8_t order[WW_MAX_PLAYERS];
    int n = ww_night_order(&g, 0, order);
    for (int i = 0; i < n; i++) {
        const int who = order[i];
        if (g.role[who] == WW_ROLE_WOLF) ww_night_act(&g, who, order[(i + 1) % n], line, WW_CHAT_MAX, 0);
        else                             ww_night_act(&g, who, order[(i + 1) % n], 0, 0, 0);
    }
    unsigned char buf[4096];
    const int with_lines = seal_bytes(&g, 12, s, buf, (int)sizeof buf, 0);
    CHECK(contains(buf, with_lines, "zzzzzzzz"), "tonight's lines ride along");

    CHECK(ww_day_lynch(&g, order[0] == g.victim[0] ? order[1] : order[0]) == WW_OK, "a lynch");
    const int pruned = seal_bytes(&g, 12, s, buf, (int)sizeof buf, 0);
    CHECK(!contains(buf, pruned, "zzzzzzzz"), "last night's lines are gone");
    CHECK(pruned < with_lines, "and the bubble got smaller");
    // Pruning is display-only, so the pruned chain must still rebuild the game.
    WwEnvelope d; WwGame r;
    CHECK(ww_msg_decode(buf, pruned, &d) == WW_MSG_EOK, "the pruned envelope decodes");
    CHECK(ww_msg_replay(&d, &r) == WW_MSG_EOK, "and replays");
    CHECK(r.night == g.night && r.phase == g.phase && r.alive == g.alive,
          "to the same game");
}

// ------------------------------------------------------------- Rule P -------

// HEADLINE 3. Two players act for the same turn: seat C sends its own move, and
// seat D carries C's pass forward plus its own. D's chain has more accepted
// records, so it wins - and it wins EVEN WHEN the digest tiebreak would have
// picked the other one, which is what makes this a test of clause 2 rather than
// a coin flip that happened to land right.
static void test_a_carried_skip_beats_the_skipped_players_late_move(void) {
    unsigned char bx[4096], by[4096];
    WwChainKey kx, ky, kp;
    int nx = 0, ny = 0, found = 0;
    uint8_t s[32];

    for (unsigned v = 1; v <= 600 && !found; v++) {
        seed_of(s, v);
        WwGame base;
        if (ww_deal(&base, s, 5) != WW_OK) continue;
        uint8_t order[WW_MAX_PLAYERS];
        const int n = ww_night_order(&base, 0, order);
        if (n != 5) continue;
        // The parent both forks are composed against: the first two seats sent.
        ww_night_act(&base, order[0], order[1], 0, 0, 0);
        ww_night_act(&base, order[1], order[0], 0, 0, 0);

        WwGame x = base, y = base;
        // X: the third seat's own move, on its own. Turn 3.
        if (ww_night_act(&x, order[2], order[0], 0, 0, 0) != WW_OK) continue;
        // Y: the FOURTH seat carries the third seat's pass and moves. Turn 4.
        if (ww_night_act(&y, order[3], order[0], 0, 0, 1) != WW_OK) continue;
        if (x.phase != WW_PHASE_NIGHT || y.phase != WW_PHASE_NIGHT) continue;

        unsigned char bp[4096];
        const int np = seal_bytes(&base, 100 + v, s, bp, (int)sizeof bp, 0);
        if (np <= 0 || ww_chain_key(bp, np, &kp) != WW_MSG_EOK) continue;
        nx = seal_bytes(&x, 100 + v, s, bx, (int)sizeof bx, 0);
        ny = seal_bytes(&y, 100 + v, s, by, (int)sizeof by, 0);
        if (nx <= 0 || ny <= 0) continue;
        if (ww_chain_key(bx, nx, &kx) != WW_MSG_EOK) continue;
        if (ww_chain_key(by, ny, &ky) != WW_MSG_EOK) continue;
        // Both name the parent, so neither is the other's child and the ancestry
        // clause is out of it.
        for (int i = 0; i < WW_PARENT_LEN; i++) { kx.parent8[i] = kp.digest[i]; ky.parent8[i] = kp.digest[i]; }
        // The case worth testing is the one where the digest disagrees with the
        // count: X's digest sorts FIRST, so clause 4 alone would prefer X.
        if (memcmp(kx.digest, ky.digest, SHA256_DIGEST_LEN) < 0) found = 1;
    }
    CHECK(found, "a fork where the digest tiebreak would have picked the loser");
    CHECK(kx.round == ky.round, "the same round, so clause 1 is out of it");
    CHECK(kx.n_joins == ky.n_joins, "the same roster, so clause 3 is out of it");
    CHECK(ky.turn == kx.turn + 1, "the carrier's chain is one record longer");
    CHECK(memcmp(kx.digest, ky.digest, SHA256_DIGEST_LEN) < 0,
          "and the digest tiebreak favours the shorter chain");
    CHECK(ww_rule_p(&ky, &kx) < 0, "the carried skip wins");
    CHECK(ww_rule_p(&kx, &ky) > 0, "in either argument order");

    // And the winner is a game every device can rebuild, with the skip in it.
    WwEnvelope d; WwGame r;
    CHECK(ww_msg_decode(by, ny, &d) == WW_MSG_EOK && ww_msg_replay(&d, &r) == WW_MSG_EOK,
          "the winning chain replays");
    int carried = 0;
    for (int i = 0; i < r.n_records; i++) if (r.rec[i].flags & WW_REC_AUTO_PASS) carried++;
    CHECK(carried == 1, "and it carries exactly one skip");
    CHECK(r.turn == 4, "four accepted records");
}

static void test_rule_p_prefers_a_child_over_its_parent(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 41);
    ww_deal(&g, s, 6);
    ww_night_act(&g, 0, 1, 0, 0, 0);
    unsigned char bp[4096];
    const int np = seal_bytes(&g, 7, s, bp, (int)sizeof bp, 0);
    WwChainKey kp; CHECK(ww_chain_key(bp, np, &kp) == WW_MSG_EOK, "parent key");

    ww_night_act(&g, 1, 0, 0, 0, 0);
    unsigned char bc[4096];
    const int nc = seal_bytes(&g, 7, s, bc, (int)sizeof bc, 0);
    WwChainKey kc; CHECK(ww_chain_key(bc, nc, &kc) == WW_MSG_EOK, "child key");
    for (int i = 0; i < WW_PARENT_LEN; i++) kc.parent8[i] = kp.digest[i];

    CHECK(ww_rule_p(&kc, &kp) < 0, "a child outranks the parent it names");
    CHECK(ww_rule_p(&kp, &kc) > 0, "in either argument order");
    // Even when every other field is made to argue the other way, which is the
    // whole reason the clause ranks first.
    WwChainKey liar = kp;
    liar.round = 9; liar.turn = 999; liar.n_joins = 10;
    CHECK(ww_rule_p(&kc, &liar) < 0, "and it outranks a parent claiming anything");
}

static void test_rule_p_is_a_total_order(void) {
    // Antisymmetry and irreflexivity over a spread of real keys, plus a
    // transitivity sweep. A tiebreak that is not a total order is a tiebreak two
    // devices can disagree about, which forks the game.
    WwChainKey k[24];
    int n = 0;
    for (unsigned v = 1; v <= 8; v++) {
        WwGame g; uint8_t s[32]; seed_of(s, v);
        ww_deal(&g, s, 6);
        for (int step = 0; step < 3; step++) {
            ww_night_act(&g, step, (step + 1) % 6, 0, 0, 0);
            unsigned char b[4096];
            const int len = seal_bytes(&g, v, s, b, (int)sizeof b, 0);
            if (len > 0 && n < 24 && ww_chain_key(b, len, &k[n]) == WW_MSG_EOK) n++;
        }
    }
    CHECK(n == 24, "twenty-four keys");
    int ok = 1;
    for (int i = 0; i < n; i++) {
        if (ww_rule_p(&k[i], &k[i]) != 0) ok = 0;
        for (int j = 0; j < n; j++) {
            const int a = ww_rule_p(&k[i], &k[j]);
            const int b = ww_rule_p(&k[j], &k[i]);
            if ((a < 0) != (b > 0)) ok = 0;
            if ((a == 0) != (b == 0)) ok = 0;
        }
    }
    CHECK(ok, "antisymmetric and irreflexive");
    int trans = 1;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            for (int m = 0; m < n; m++)
                if (ww_rule_p(&k[i], &k[j]) < 0 && ww_rule_p(&k[j], &k[m]) < 0
                    && !(ww_rule_p(&k[i], &k[m]) < 0)) trans = 0;
    CHECK(trans, "and transitive");
}

static void test_a_started_chain_outranks_a_lobby(void) {
    WwGame g; uint8_t s[32]; seed_of(s, 42);
    ww_deal(&g, s, 6);
    unsigned char b[4096];
    const int n = seal_bytes(&g, 3, s, b, (int)sizeof b, 0);
    WwChainKey live; CHECK(ww_chain_key(b, n, &live) == WW_MSG_EOK, "live key");

    WwEnvelope e; ww_envelope_init(&e);
    e.phase = WW_PHASE_LOBBY;
    e.n_players = 6;
    for (int i = 0; i < 32; i++) e.seed[i] = s[i];
    fill_roster(&e, 6);
    e.body = 0; e.body_len = 0; e.n_records = 0;
    unsigned char lb[4096];
    const int ln = ww_msg_encode(&e, lb, (int)sizeof lb);
    CHECK(ln > 0, "a lobby encodes");
    WwChainKey lobby; CHECK(ww_chain_key(lb, ln, &lobby) == WW_MSG_EOK, "lobby key");
    CHECK(ww_rule_p(&live, &lobby) < 0, "a dealt game outranks the invite");
    CHECK(ww_rule_p(&lobby, &live) > 0, "in either argument order");
    WwGame r;
    WwEnvelope d;
    CHECK(ww_msg_decode(lb, ln, &d) == WW_MSG_EOK, "and the lobby decodes");
    CHECK(ww_msg_replay(&d, &r) == WW_MSG_EOK, "and replays to no game");
    CHECK(r.phase == WW_PHASE_LOBBY, "still a lobby");
}

// ------------------------------------------------------------- the clocks ---

static void test_the_send_floor_counts_down(void) {
    CHECK(ww_send_floor_remaining(1000, 1000) == WW_SEND_FLOOR_S, "the whole floor at once");
    CHECK(ww_send_floor_remaining(1000, 1005) == WW_SEND_FLOOR_S - 5, "half spent");
    CHECK(ww_send_floor_remaining(1000, 1010) == 0, "spent");
    CHECK(ww_send_floor_remaining(1000, 5000) == 0, "and stays spent");
    // The clock is unix seconds mod 65536, so an interval that straddles the
    // roll must still be that interval. A signed subtraction gets this wrong by
    // 65536 and the floor would never expire.
    CHECK(ww_send_floor_remaining(65530, 4) == WW_SEND_FLOOR_S - 10, "across the wrap");
    CHECK(ww_send_floor_remaining(65530, 6) == 0, "and spends across it");
}

static void test_the_carry_gate_opens_after_a_minute(void) {
    CHECK(!ww_may_carry(1000, 1000), "not at once");
    CHECK(!ww_may_carry(1000, 1059), "not a second early");
    CHECK(ww_may_carry(1000, 1060), "on the minute");
    CHECK(ww_may_carry(65500, 24), "and across the wrap");
}

// ------------------------------------------------------- the whole night ----

static void test_one_full_night_end_to_end(void) {
    // The milestone, as one test: deal from a seed, every living seat sends one
    // record, the view hides what it must, the kill resolves, and the bubble
    // that carries it replays to the same game on another device.
    uint8_t s[32]; seed_of(s, 99);
    WwGame g;
    CHECK(ww_deal(&g, s, 7) == WW_OK, "seven players");
    uint8_t order[WW_MAX_PLAYERS];
    const int n = ww_night_order(&g, 0, order);
    const int decider = ww_night_decider(&g);
    int mark = -1;
    for (int i = 0; i < n; i++)
        if (g.role[order[i]] != WW_ROLE_WOLF) { mark = order[i]; break; }
    CHECK(mark >= 0, "somebody to take");

    unsigned char prev[4096];
    int prev_len = 0;
    for (int i = 0; i < n; i++) {
        const int who = order[i];
        const int t = (who == mark) ? decider : mark;
        const int rc = (g.role[who] == WW_ROLE_WOLF)
            ? ww_night_act(&g, who, t, "him", 3, 0)
            : ww_night_act(&g, who, t, 0, 0, 0);
        CHECK(rc == WW_OK, "one record per seat");
        // Each send is one bubble, and each bubble is the whole game so far.
        unsigned char buf[4096];
        WwEnvelope e;
        const int len = seal_bytes(&g, 0xABCDEFULL, s, buf, (int)sizeof buf, &e);
        CHECK(len > 0, "sealed");
        if (prev_len) {
            WwChainKey a, b;
            CHECK(ww_chain_key(buf, len, &a) == WW_MSG_EOK, "child key");
            CHECK(ww_chain_key(prev, prev_len, &b) == WW_MSG_EOK, "parent key");
            for (int j = 0; j < WW_PARENT_LEN; j++) a.parent8[j] = b.digest[j];
            CHECK(ww_rule_p(&a, &b) < 0, "each bubble outranks the one before it");
        }
        memcpy(prev, buf, (size_t)len);
        prev_len = len;
    }
    CHECK(g.phase == WW_PHASE_DAY, "the night is over");
    CHECK(g.victim[0] == mark, "and the wolves took who they said");
    CHECK(ww_alive_count(&g) == 6, "six left");

    WwEnvelope d; WwGame r;
    CHECK(ww_msg_decode(prev, prev_len, &d) == WW_MSG_EOK, "the last bubble decodes");
    CHECK(ww_msg_replay(&d, &r) == WW_MSG_EOK, "and replays");
    CHECK(r.alive == g.alive && r.victim[0] == g.victim[0], "to the same morning");
    // Every seat's view agrees with the game it was built from, and no seat's
    // view carries the wolves' line but a wolf's.
    for (int seat = 0; seat < 7; seat++) {
        unsigned char v[WW_VIEW_MAX];
        const int len = ww_view_put(&r, seat, v);
        CHECK(len == ww_view_measure(&r, seat), "measure agrees");
        const int is_wolf = r.role[seat] == WW_ROLE_WOLF && ww_is_alive(&r, seat);
        CHECK(contains(v, len, "him") == is_wolf, "only living wolves read the channel");
    }
}

int main(void) {
    test_wolf_count_is_the_classic_ladder();
    test_deal_refuses_a_table_outside_five_to_ten();
    test_deal_hands_out_one_seer_and_the_wolf_count();
    test_the_deal_is_a_function_of_the_seed();
    test_the_seed_reaches_every_seat();

    test_the_night_needs_every_living_seat();
    test_the_night_refuses_a_second_record_from_one_seat();
    test_the_night_refuses_a_bad_target();
    test_only_a_wolf_may_carry_a_line();
    test_the_decider_is_the_last_wolf_in_the_rotation();
    test_the_rotation_moves_the_call();
    test_the_kill_is_the_deciders_choice();
    test_a_skipped_decider_loses_the_kill_to_the_last_wolf_who_chose();
    test_a_night_with_no_wolf_choice_kills_nobody();
    test_the_carry_only_reaches_backwards();
    test_a_carried_seat_cannot_then_send();

    test_a_lynch_starts_the_next_night();
    test_the_village_wins_when_the_pack_is_out();
    test_the_wolves_win_on_parity();

    test_a_wolfs_record_is_byte_identical_to_a_villagers();
    test_the_wolf_line_is_absent_from_a_non_wolfs_view();
    test_a_non_wolf_is_never_told_who_decides();
    test_the_seers_reading_is_only_the_seers();
    test_a_third_seat_sees_only_that_a_seat_sent();
    test_a_carried_seat_reads_as_carried();
    test_a_dead_seats_role_is_public();
    test_a_spectator_learns_no_role();
    test_a_dead_wolf_loses_the_channel();
    test_view_measure_matches_what_put_writes();

    test_the_envelope_round_trips();
    test_a_full_night_seals_inside_the_url_budget();
    test_decode_refuses_hostile_bytes();
    test_decode_refuses_a_contradictory_record();
    test_replay_refuses_a_header_that_lies();
    test_seal_prunes_a_past_nights_line();

    test_a_carried_skip_beats_the_skipped_players_late_move();
    test_rule_p_prefers_a_child_over_its_parent();
    test_rule_p_is_a_total_order();
    test_a_started_chain_outranks_a_lobby();

    test_the_send_floor_counts_down();
    test_the_carry_gate_opens_after_a_minute();
    test_one_full_night_end_to_end();

    printf("%d passed, %d failed\n", n_pass, n_fail);
    return n_fail ? 1 : 0;
}
