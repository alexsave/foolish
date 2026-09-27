/* The words: the table is whole and clean (no hole, no unknown placeholder,
 * no dash, no final full stop, within its width), and every position says
 * the sentence the rules give it: bids, a call's caption (the call alone,
 * K8), the outcome (K9), the headlines, the lobby, the errors. */
#include "cn_check.h"
#include "../src/cn_say.h"
#include "../src/cn_msg.h"

static CnGame G;
static char out[512];
static const char *NAMES[6] = { "Alex", "Bo", "Cy", "Dee", "", 0 };

static int has_dash(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x93 || p[2] == 0x94)) return 1;
    return 0;
}

static void test_table(void)
{
    TEST("the table");
    static const char *known[] = { "who", "loser", "bid", "qty", "face", "n", "game", 0 };
    for (int k = 0; k < CN_K_COUNT; k++) {
        const char *s = cn_text(k);
        CHECK(s[0] || cn_key_may_be_empty(k), "%s is empty", cn_key_name(k));
        CHECK(!has_dash(s), "%s has an em or en dash", cn_key_name(k));
        size_t n = strlen(s);
        CHECK(n == 0 || s[n - 1] != '.' || !strcmp(cn_key_name(k), "CAP_JOIN"), "%s ends in a full stop", cn_key_name(k));
        CHECK(!cn_key_max(k) || cn_text_cols(s) <= cn_key_max(k), "%s is %d columns, the limit %d",
              cn_key_name(k), cn_text_cols(s), cn_key_max(k));
        for (const char *p = s; (p = strchr(p, '{')); p++) {
            const char *e = strchr(p, '}');
            CHECK(e != 0, "%s: an open brace", cn_key_name(k));
            if (!e) break;
            int ok = 0;
            for (int i = 0; known[i]; i++)
                ok |= (size_t)(e - p - 1) == strlen(known[i]) && !strncmp(p + 1, known[i], strlen(known[i]));
            CHECK(ok, "%s: unknown placeholder %.*s", cn_key_name(k), (int)(e - p + 1), p);
        }
    }
    CHECK(!strcmp(cn_key_name(CN_K_GAME_NAME), "GAME_NAME") && !strcmp(cn_text(CN_K_GAME_NAME), "Chui Niu"), "the name");
}

static void test_things(void)
{
    TEST("things");
    cn_say_bid(4, 3, 0, out, sizeof out);
    CHECK(!strcmp(out, "four 3s"), "%s", out);
    cn_say_bid(4, 3, 1, out, sizeof out);
    CHECK(!strcmp(out, "Four 3s"), "%s", out);
    cn_say_bid(1, 6, 0, out, sizeof out);
    CHECK(!strcmp(out, "one 6"), "%s", out);
    cn_say_bid(12, 2, 1, out, sizeof out);
    CHECK(!strcmp(out, "Twelve 2s"), "%s", out);
    cn_say_bid(13, 5, 1, out, sizeof out);
    CHECK(!strcmp(out, "13 5s"), "%s", out);
    CHECK(cn_say_bid(0, 3, 0, out, sizeof out) == -1 && cn_say_bid(3, 7, 0, out, sizeof out) == -1, "off the table");
    cn_say_seat(NAMES, 1, out, sizeof out);
    CHECK(!strcmp(out, "Bo"), "%s", out);
    cn_say_seat(NAMES, 4, out, sizeof out);
    CHECK(!strcmp(out, "Player 5"), "an empty name falls back: %s", out);
    cn_say_seat(0, 2, out, sizeof out);
    CHECK(!strcmp(out, "Player 3"), "no roster: %s", out);
    cn_say_dice_n(1, out, sizeof out);
    CHECK(!strcmp(out, "1 die"), "%s", out);
    cn_say_dice_n(4, out, sizeof out);
    CHECK(!strcmp(out, "4 dice"), "%s", out);
    CHECK(cn_say_bid(4, 3, 0, out, 4) == -1, "a small buffer");
}

/* A three-seat table with chosen dice. */
static void table(const uint8_t d[3][5])
{
    uint8_t seed[32];
    seed_wide(seed, 5);
    cn_new(&G, seed, 3);
    memcpy(G.dice, d, 15);
}

static void test_captions(void)
{
    TEST("captions");
    static const uint8_t d[3][5] = { { 3, 3, 1, 5, 6 }, { 2, 3, 4, 4, 1 }, { 6, 6, 5, 2, 3 } };
    table(d);
    cn_say_caption(&G, 0, NAMES, out, sizeof out);
    CHECK(!strcmp(out, "Dice rolled. Alex bids first"), "the start: %s", out);
    cn_say_headline(&G, 0, NAMES, out, sizeof out);
    CHECK(!strcmp(out, "Your turn: open the bidding"), "%s", out);
    cn_say_headline(&G, 1, NAMES, out, sizeof out);
    CHECK(!strcmp(out, "Alex's turn"), "%s", out);
    cn_say_subline(&G, NAMES, out, sizeof out);
    CHECK(!strcmp(out, "No bid yet"), "%s", out);
    cn_say_outcome(&G, NAMES, out, sizeof out);
    CHECK(!strcmp(out, ""), "no call yet: %s", out);

    /* the caption says what the plan says, and the plan is replayed from the
     * seed, so this game plays the dice the seed rolled: captions only */
    cn_apply(&G, 0, bid(4, 3));
    cn_say_caption(&G, 1, NAMES, out, sizeof out);
    CHECK(!strcmp(out, "Alex bid four 3s"), "%s", out);
    cn_say_headline(&G, 1, NAMES, out, sizeof out);
    CHECK(!strcmp(out, "Your turn: raise or call"), "%s", out);
    cn_say_subline(&G, NAMES, out, sizeof out);
    CHECK(!strcmp(out, "Bid to beat: four 3s by Alex"), "%s", out);
    cn_say_staged(&G, bid(5, 2), out, sizeof out);
    CHECK(!strcmp(out, "Send to bid five 2s"), "%s", out);
    cn_say_staged(&G, call_move(), out, sizeof out);
    CHECK(!strcmp(out, "Send to call four 3s"), "%s", out);

    /* THE CALL: its caption names the call only (K8), the outcome the rest */
    CnEvent ev[8];
    int n = cn_plan_move(&G, call_move(), ev, 8);
    cn_say_caption_of(ev, n, NAMES, out, sizeof out);
    CHECK(!strcmp(out, "Bo calls four 3s"), "a call's caption: %s", out);
    cn_say_outcome_of(ev, n, NAMES, out, sizeof out);
    CHECK(!strcmp(out, "Bo calls. Four 3s was true, Bo loses a die"), "true: %s", out);

    /* THE OUTCOME: false, out, and the end, built from events */
    CnEvent e2[5];
    memset(e2, 0, sizeof e2);
    e2[0].kind = CN_EV_CALL; e2[0].seat = 1; e2[0].other = 0; e2[0].q = 7; e2[0].f = 5;
    e2[1].kind = CN_EV_REVEAL; e2[1].q = 7; e2[1].f = 5; e2[1].count = 3;
    e2[2].kind = CN_EV_LOSE; e2[2].seat = 0; e2[2].count = 0;
    e2[3].kind = CN_EV_OUT; e2[3].seat = 0;
    e2[4].kind = CN_EV_ROUND; e2[4].seat = 1; e2[4].move = 9;
    cn_say_outcome_of(e2, 5, NAMES, out, sizeof out);
    CHECK(!strcmp(out, "Bo calls. Seven 5s was false, Alex loses a die. Alex is out"), "false and out: %s", out);
    cn_say_caption_of(e2, 5, NAMES, out, sizeof out);
    CHECK(!strcmp(out, "Bo calls seven 5s"), "a later round opens silently: %s", out);
    e2[4].kind = CN_EV_OVER; e2[4].seat = 1;
    cn_say_outcome_of(e2, 5, NAMES, out, sizeof out);
    CHECK(!strcmp(out, "Bo calls. Seven 5s was false, Alex loses a die. Bo wins"), "the end: %s", out);
}

static void test_screen_end(void)
{
    TEST("the screen at the end");
    uint8_t seed[32];
    for (uint32_t k = 0; k < 100; k++) {
        seed_wide(seed, 600 + k);
        cn_new(&G, seed, 3);
        int out_seen = 0;
        while (G.phase != CN_PH_OVER) {
            cn_apply(&G, G.turn, bot_move(&G));
            for (int s = 0; s < 3; s++)
                if (G.phase != CN_PH_OVER && G.dice_n[s] == 0) {
                    cn_say_headline(&G, s, NAMES, out, sizeof out);
                    CHECK(!strcmp(out, "You're out"), "game %u seat %d out: %s", k, s, out);
                    out_seen = 1;
                }
        }
        (void)out_seen;
        cn_say_headline(&G, G.winner, NAMES, out, sizeof out);
        CHECK(!strcmp(out, "You win"), "game %u: %s", k, out);
        int loser = (G.winner + 1) % 3;
        cn_say_headline(&G, loser, NAMES, out, sizeof out);
        char want[64];
        snprintf(want, sizeof want, "%s wins", NAMES[G.winner]);
        CHECK(!strcmp(out, want), "game %u: %s", k, out);
        cn_say_outcome(&G, NAMES, out, sizeof out);
        size_t n = strlen(out), w = strlen(want);
        CHECK(n > w && !strcmp(out + n - w, want), "game %u: the outcome ends with the winner: %s", k, out);
        cn_say_subline(&G, NAMES, out, sizeof out);
        CHECK(!strcmp(out, ""), "no bid line once over");
        for (int m = 0; m <= G.hist_n; m++) {
            int r = cn_say_caption(&G, m, NAMES, out, sizeof out);
            CHECK(r > 0 && !has_dash(out) && out[r - 1] != '.', "game %u move %d: %s", k, m, out);
        }
    }
    /* the top bid leaves only the call */
    seed_wide(seed, 9);
    cn_new(&G, seed, 2);
    cn_apply(&G, 0, bid(10, 6));
    cn_say_headline(&G, 1, NAMES, out, sizeof out);
    CHECK(!strcmp(out, "Your turn: call it"), "%s", out);
    cn_say_table(&G, out, sizeof out);
    CHECK(!strcmp(out, "10 dice on the table"), "%s", out);
    cn_apply(&G, 1, call_move());
    cn_say_reveal_count(&G, out, sizeof out);
    char want[32];
    snprintf(want, sizeof want, "There were %s", (const char *[]){ "0", "one", "two", "three", "four", "five", "six",
                                                     "seven", "eight", "nine", "ten" }[G.call_count]);
    CHECK(!strcmp(out, want), "%s, want %s", out, want);
}

static void test_lobby_errors(void)
{
    TEST("the lobby and the errors");
    cn_say_lobby_caption(CN_SAY_INVITE, "Alex", out, sizeof out);
    CHECK(!strcmp(out, "Alex wants a game of Chui Niu. Tap to join"), "%s", out);
    cn_say_lobby_caption(CN_SAY_JOINED, "Bo", out, sizeof out);
    CHECK(!strcmp(out, "Bo joined"), "%s", out);
    cn_say_lobby_caption(CN_SAY_LEFT, "Cy", out, sizeof out);
    CHECK(!strcmp(out, "Cy left"), "%s", out);
    cn_say_lobby_row(NAMES, 1, 0, out, sizeof out);
    CHECK(!strcmp(out, "2. Bo"), "%s", out);
    cn_say_lobby_row(NAMES, 1, 1, out, sizeof out);
    CHECK(!strcmp(out, "2. Bo (You)"), "%s", out);
    cn_say_error(CN_EFORMAT, out, sizeof out);
    CHECK(!strcmp(out, "That game came from a newer version of the app"), "%s", out);
    int dam[] = { CN_ECHECK, CN_ESHORT, CN_EGAME, CN_ETEXT };
    for (int i = 0; i < 4; i++) {
        cn_say_error(dam[i], out, sizeof out);
        CHECK(!strcmp(out, "This game link is damaged"), "%d: %s", dam[i], out);
    }
    cn_say_error(CN_EMAGIC, out, sizeof out);
    CHECK(!strcmp(out, "Can't read that"), "%s", out);
    cn_say_rules_title(out, sizeof out);
    CHECK(!strcmp(out, "How to play Chui Niu"), "%s", out);
    for (int i = 0; i < CN_RULES_N; i++) CHECK(cn_say_rule(i, out, sizeof out) > 20, "rule %d", i);
    CHECK(cn_say_rule(CN_RULES_N, out, sizeof out) == -1, "no rule 7");
}

int main(void)
{
    test_table();
    test_things();
    test_captions();
    test_screen_end();
    test_lobby_errors();
    return report("cn_say_test");
}
