/* The words (T8): the table's own rules (every key there, every placeholder
 * known, widths, no dashes, no final full stop, the game's name in one key,
 * none of the branded game's words), then the captions and screen lines of
 * hand-built positions, word for word, then every caption of random games
 * against the 36-column line and the guards. */
#include "tb_check.h"
#include "../src/tb_say.h"
#include "../src/tb_plan.h"
#include <ctype.h>

static const char *NAMES[8] = { "Alex", "Bo", "Cy", "Dee", "Eve", "Fin", "Gus", "Hal" };

static int has_ci(const char *s, const char *w)
{
    size_t n = strlen(w);
    for (; *s; s++) {
        size_t i = 0;
        while (i < n && s[i] && tolower((unsigned char)s[i]) == tolower((unsigned char)w[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

/* THE GUARD: no sentence the game can say names the branded game or reads
 * like its card (DECISIONS.md T1, T4). */
static void guard(const char *s, const char *what)
{
    static const char *banned[] = { "yahtzee", "kniffel", "generala", "yacht", "straight", "chance", 0 };
    for (int i = 0; banned[i]; i++) CHECK(!has_ci(s, banned[i]), "%s says \"%s\": %s", what, banned[i], s);
    CHECK(!strstr(s, "\xE2\x80\x94") && !strstr(s, "\xE2\x80\x93"), "%s has a dash: %s", what, s);
}

static void s_table(void)
{
    TEST("say table");
    static const char *known[] = { "who", "next", "a", "b", "n", "cat", "phrase", "dice", "count", "face", "total", "game", 0 };
    for (int k = 0; k < TB_K_COUNT; k++) {
        const char *t = tb_text(k), *name = tb_key_name(k);
        CHECK(t[0] || tb_key_may_be_empty(k), "%s is empty", name);
        guard(t, name);
        size_t n = strlen(t);
        CHECK(n == 0 || t[n - 1] != '.' || k == TB_K_CAP_JOIN, "%s ends in a full stop", name);
        CHECK(k == TB_K_GAME_NAME || !has_ci(t, "tallybones"), "%s spells the game's name out: %s", name, t);
        char filled[512];
        int w = tb_fill(filled, sizeof filled, t, 0);
        CHECK(w >= 0, "%s fills", name);
        if (tb_key_max(k)) CHECK(tb_text_cols(filled) <= tb_key_max(k), "%s is %d columns, room %d", name,
                                 tb_text_cols(filled), tb_key_max(k));
        for (const char *p = t; (p = strchr(p, '{')); p++) {
            const char *e = strchr(p, '}');
            int ok = 0;
            for (int i = 0; e && known[i]; i++)
                if ((size_t)(e - p - 1) == strlen(known[i]) && !strncmp(p + 1, known[i], strlen(known[i]))) ok = 1;
            CHECK(ok, "%s has an unknown placeholder at %s", name, p);
        }
    }
    CHECK(!strcmp(tb_text(TB_K_GAME_NAME), "Tallybones"), "GAME_NAME");
    char out[64];
    CHECK(tb_say_cat(TB_C_TALLYBONES, out, sizeof out) > 0 && !strcmp(out, "Tallybones"), "the category is the name: %s", out);
}

/* A two- or three-seat game on Alex's roll with these dice. */
static void position(TbGame *g, int n, const uint8_t dice[5])
{
    uint8_t seed[32];
    seed_wide(seed, 11);
    tb_new(g, seed, n, 0);
    memcpy(g->dice, dice, 5);
}

static void said_draft(const TbGame *g, TbMove m, int full, const char *want)
{
    TbGame d;
    char out[256];
    CHECK(tb_draft(&d, g, m), "draft %d/%d", m.kind, m.arg);
    int n = tb_say_move(g, m, NAMES, full, out, sizeof out);
    CHECK(n > 0 && !strcmp(out, want), "said \"%s\", want \"%s\"", out, want);
    guard(out, "a caption");
}

static void s_captions(void)
{
    TEST("say captions");
    TbGame g;
    static const uint8_t keep[5] = { 3, 5, 1, 3, 6 };
    position(&g, 2, keep);
    said_draft(&g, mv(TB_M_KEEP, 0, 0x0B), 0, "Alex keeps 3, 3, 5 and rerolls two");
    said_draft(&g, mv(TB_M_KEEP, 0, 0x0B), 1, "Alex keeps 3, 3, 5 and rerolls two");
    said_draft(&g, mv(TB_M_KEEP, 0, 0x10), 0, "Alex keeps 6 and rerolls four");
    said_draft(&g, mv(TB_M_KEEP, 0, 0x1E), 0, "Alex keeps 1, 3, 5, 6 and rerolls one");
    said_draft(&g, mv(TB_M_KEEP, 0, 0), 0, "Alex rerolls all five");
    static const uint8_t house[5] = { 2, 3, 2, 3, 3 };
    position(&g, 2, house);
    said_draft(&g, mv(TB_M_SCORE, 0, TB_C_FULL_HOUSE), 0, "Alex rolled a full house, 25 points");
    said_draft(&g, mv(TB_M_SCORE, 0, TB_C_FULL_HOUSE), 1, "Alex rolled a full house, 25 points. Bo to roll");
    said_draft(&g, mv(TB_M_SCORE, 0, TB_C_THREES), 0, "Alex scored 9 in Threes. Bo to roll");
    said_draft(&g, mv(TB_M_SCORE, 0, TB_C_LONG_RUN), 0, "Alex took a zero on Long Run");
    said_draft(&g, mv(TB_M_SCORE, 0, TB_C_ANY), 0, "Alex took 13 on Any. Bo to roll");
    said_draft(&g, mv(TB_M_SCORE, 0, TB_C_THREE_ALIKE), 1, "Alex rolled three alike, 13 points. Bo to roll");
    static const uint8_t five[5] = { 4, 4, 4, 4, 4 };
    position(&g, 2, five);
    said_draft(&g, mv(TB_M_SCORE, 0, TB_C_TALLYBONES), 0, "Alex rolled Tallybones! 50 points");
    said_draft(&g, mv(TB_M_SCORE, 0, TB_C_TALLYBONES), 1, "Alex rolled Tallybones! 50 points. Bo to roll");
    /* the bonus */
    static const uint8_t sixes[5] = { 6, 6, 6, 6, 2 };
    position(&g, 2, sixes);
    g.score[0][TB_C_FIVES] = 25; g.score[0][TB_C_FOURS] = 16; g.filled[0] = 0x18;
    said_draft(&g, mv(TB_M_SCORE, 0, TB_C_SIXES), 1, "Alex scored 24 in Sixes. Alex made the 35 point bonus. Bo to roll");
    said_draft(&g, mv(TB_M_SCORE, 0, TB_C_SIXES), 0, "Alex scored 24 in Sixes");
    /* a leave */
    position(&g, 3, sixes);
    said_draft(&g, mv(TB_M_LEAVE, 0, 0), 1, "Alex left the game. Bo to roll");
    said_draft(&g, mv(TB_M_LEAVE, 2, 0), 1, "Cy left the game");
    /* the start */
    char out[256];
    CHECK(tb_say_caption(&g, 0, NAMES, out, sizeof out) > 0 && !strcmp(out, "Tallybones is on. Alex to roll"),
          "the start: %s", out);
    CHECK(tb_say_lobby_caption(TB_SAY_INVITE, "Alex", out, sizeof out) > 0
          && !strcmp(out, "Alex wants a game of Tallybones. Tap to join"), "invite: %s", out);
    CHECK(tb_say_lobby_caption(TB_SAY_JOINED, "Bo", out, sizeof out) > 0 && !strcmp(out, "Bo joined"), "%s", out);
    CHECK(tb_say_seat(0, 2, out, sizeof out) > 0 && !strcmp(out, "Player 3"), "a seat with no name: %s", out);
}

/* The last turn of a game: every category filled but Any, Bo to score it. */
static void last_turn(TbGame *g, int n, const int totals[3])
{
    static const uint8_t d[5] = { 1, 1, 1, 1, 2 };     /* Any scores 6 */
    position(g, n, d);
    for (int s = 0; s < n; s++) {
        g->filled[s] = TB_FULL_CARD;
        g->score[s][TB_C_THREE_ALIKE] = (uint8_t)(totals[s] / 2);
        g->score[s][TB_C_FOUR_ALIKE] = (uint8_t)(totals[s] - totals[s] / 2);
    }
    g->filled[n - 1] &= (uint16_t)~(1u << TB_C_ANY);
    g->turn = (uint8_t)(n - 1);
}

static void s_end(void)
{
    TEST("say end");
    TbGame g;
    const int win[3] = { 241, 100, 0 };
    last_turn(&g, 2, win);
    said_draft(&g, mv(TB_M_SCORE, 1, TB_C_ANY), 0, "Alex wins with 241");
    said_draft(&g, mv(TB_M_SCORE, 1, TB_C_ANY), 1, "Bo took 6 on Any. Alex wins with 241");
    const int tie[3] = { 200, 194, 0 };
    last_turn(&g, 2, tie);
    said_draft(&g, mv(TB_M_SCORE, 1, TB_C_ANY), 0, "Alex and Bo tie at 200");
    const int three[3] = { 200, 200, 194 };
    last_turn(&g, 3, three);
    said_draft(&g, mv(TB_M_SCORE, 2, TB_C_ANY), 0, "Alex, Bo and Cy tie at 200");
    TbGame d;
    char out[128];
    tb_draft(&d, &g, mv(TB_M_SCORE, 2, TB_C_ANY));
    CHECK(tb_say_headline(&d, 0, NAMES, out, sizeof out) > 0 && !strcmp(out, "Alex, Bo and Cy tie"), "%s", out);
    last_turn(&g, 2, win);
    tb_draft(&d, &g, mv(TB_M_SCORE, 1, TB_C_ANY));
    CHECK(tb_say_headline(&d, 0, NAMES, out, sizeof out) > 0 && !strcmp(out, "You win"), "my own staged win: %s", out);
    CHECK(tb_say_headline(&d, 1, NAMES, out, sizeof out) > 0 && !strcmp(out, "Alex wins"), "Bo's view of it: %s", out);
}

static void s_screen(void)
{
    TEST("say screen");
    TbGame g, d;
    static const uint8_t dice[5] = { 1, 2, 3, 4, 6 };
    position(&g, 2, dice);
    char out[128];
    CHECK(tb_say_headline(&g, 0, NAMES, out, sizeof out) > 0 && !strcmp(out, "Your roll"), "%s", out);
    CHECK(tb_say_subline(&g, 0, NAMES, out, sizeof out) > 0 && !strcmp(out, "Tap dice to keep. Two rerolls left"), "%s", out);
    CHECK(tb_say_headline(&g, 1, NAMES, out, sizeof out) > 0 && !strcmp(out, "Waiting on Alex"), "%s", out);
    CHECK(tb_say_subline(&g, 1, NAMES, out, sizeof out) > 0 && !strcmp(out, "Alex is on roll 1 of 3"), "%s", out);
    tb_draft(&d, &g, mv(TB_M_KEEP, 0, 3));
    CHECK(tb_say_headline(&d, 0, NAMES, out, sizeof out) > 0 && !strcmp(out, "Send it to reroll"), "%s", out);
    CHECK(tb_say_subline(&d, 0, NAMES, out, sizeof out) > 0 && !strcmp(out, "The new dice come when it is sent"), "%s", out);
    CHECK(tb_say_spoken_die(&d, 0, out, sizeof out) > 0 && !strcmp(out, "Die 1, one, kept"), "%s", out);
    CHECK(tb_say_spoken_die(&d, 4, out, sizeof out) > 0 && !strcmp(out, "Die 5, rolling"), "%s", out);
    CHECK(tb_say_spoken_die(&g, 4, out, sizeof out) > 0 && !strcmp(out, "Die 5, six"), "%s", out);
    tb_draft(&d, &g, mv(TB_M_SCORE, 0, TB_C_SHORT_RUN));
    CHECK(tb_say_subline(&d, 0, NAMES, out, sizeof out) > 0 && !strcmp(out, "Short Run for 30"), "%s", out);
    g.roll = 3;
    CHECK(tb_say_subline(&g, 0, NAMES, out, sizeof out) > 0 && !strcmp(out, "Pick a category"), "%s", out);
    for (int r = 0; r < 16; r++) CHECK(tb_say_row_label(r, out, sizeof out) > 0, "row %d has a label", r);
    CHECK(tb_say_row_label(15, out, sizeof out) > 0 && !strcmp(out, "Total"), "%s", out);
    CHECK(tb_say_row_label(13, out, sizeof out) > 0 && !strcmp(out, "Tallybones"), "%s", out);
}

/* Every caption and summary of random games, and every staged keep's caption:
 * one line, the guards, and a keep names only the dice it keeps (T11). */
static void s_games(void)
{
    TEST("say games");
    int longest = 0;
    for (uint32_t k = 0; k < 140; k++) {
        uint8_t seed[32];
        seed_wide(seed, 500 + k);
        TbGame g;
        int n = 2 + (int)(k % 7);
        tb_new(&g, seed, n, 0);
        char out[512];
        while (!g.over) {
            if (g.roll < 3 && rnd(2)) {
                int mask = (int)rnd(31);
                TbGame d;
                CHECK(tb_draft(&d, &g, mv(TB_M_KEEP, g.turn, mask)), "a keep");
                CHECK(tb_say_move(&g, mv(TB_M_KEEP, g.turn, mask), NAMES, 1, out, sizeof out) > 0,
                      "a staged keep says something");
                int digits[7] = { 0 }, want[7] = { 0 };
                for (const char *p = out; *p; p++) if (*p >= '1' && *p <= '6') digits[*p - '0']++;
                for (int i = 0; i < 5; i++) if (mask >> i & 1) want[g.dice[i]]++;
                CHECK(!memcmp(digits, want, sizeof want), "a staged keep names the kept dice and no other: %s", out);
            }
            if (!bot_step(&g, 200)) break;
            CHECK(tb_say_caption(&g, g.hist_n, NAMES, out, sizeof out) > 0, "a caption");
            /* the first clause always goes; a second only while the line fits */
            CHECK(tb_text_cols(out) <= TB_CAPTION_MAX || !strstr(out, ". "), "one line: %s", out);
            if (tb_text_cols(out) > longest) longest = tb_text_cols(out);
            guard(out, "a caption");
            CHECK(tb_say_summary(&g, g.hist_n, NAMES, out, sizeof out) > 0, "a summary");
            guard(out, "a summary");
            if (!g.over) CHECK(tb_say_headline(&g, g.turn, NAMES, out, sizeof out) > 0, "a headline");
        }
    }
    printf("  the longest caption: %d columns\n", longest);
}

int main(void)
{
    s_table();
    s_captions();
    s_end();
    s_screen();
    s_games();
    return report("tb_say_test");
}
