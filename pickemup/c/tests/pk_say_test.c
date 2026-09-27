/* The words (RULES_AND_KERNEL.md section 6): every key filled, only known
 * placeholders, within its width, no dash, no final full stop, none of the
 * protected product's words, the game's name in one key; and the sentences
 * the kernel composes from them, by golden and over random games.
 *
 *     make -C pickemup/c run
 */
#include "pk_check.h"
#include "../src/pk_say.h"
#include "../src/pk_internal.h"

/* A tape of one hand-built bubble's events: apply and seal through the sink,
 * then caption the tape (pk_say_caption_of), since a hand-built position has
 * no seed history to replay. */
static PkEvent TAPE[1024];
static int TAPE_N;
static void tape_fn(const PkEvent *e, void *ctx) { (void)ctx; if (TAPE_N < 1024) TAPE[TAPE_N++] = *e; }
static PkSink SINK;
static void tape_start(void)
{
    memset(&SINK, 0, sizeof SINK);
    SINK.fn = tape_fn;
    SINK.viewer = PK_VIEW_ALL;
    SINK.from = -1;
    SINK.to = 0xFFFF;
    TAPE_N = 0;
}
static void A(PkGame *g, int seat, PkAct a) { pk__apply(g, seat, a, &SINK); }
static void S(PkGame *g) { pk__seal(g, &SINK); }

extern const char *const PK_STRINGS_EN[PK_K_COUNT];

static const char *const KNOWN[] = { "who", "target", "next", "a", "b", "card", "suits", "rank",
                                     "n", "state", "game", 0 };

static int known(const char *p, int len)
{
    for (int i = 0; KNOWN[i]; i++)
        if ((int)strlen(KNOWN[i]) == len && !strncmp(KNOWN[i], p, (size_t)len)) return 1;
    return 0;
}

static int has_word_ci(const char *s, const char *w)
{
    int n = (int)strlen(w);
    for (const char *p = s; *p; p++) {
        int i = 0;
        while (i < n && p[i] && (p[i] | 32) == (w[i] | 32)) i++;
        if (i < n) continue;
        int before = p == s || !((p[-1] | 32) >= 'a' && (p[-1] | 32) <= 'z');
        int after = !((p[n] | 32) >= 'a' && (p[n] | 32) <= 'z');
        if (before && after) return 1;
    }
    return 0;
}

static void t_table(void)
{
    TEST("6 the table");
    for (int k = 0; k < PK_K_COUNT; k++) {
        const char *s = PK_STRINGS_EN[k], *name = pk_key_name(k);
        CHECK(s != 0, "%s: a hole", name);
        if (!s) continue;
        CHECK(s[0] || pk_key_may_be_empty(k), "%s: empty", name);
        CHECK(!pk_key_max(k) || pk_text_cols(s) <= pk_key_max(k), "%s: %d columns, limit %d",
              name, pk_text_cols(s), pk_key_max(k));
        CHECK(!strstr(s, "\xe2\x80\x94") && !strstr(s, "\xe2\x80\x93"), "%s: a dash", name);
        int n = (int)strlen(s);
        CHECK(k == PK_K_CAP_JOIN || !n || s[n - 1] != '.', "%s: ends in a full stop", name);
        CHECK(!has_word_ci(s, "uno") && !has_word_ci(s, "mattel"), "%s: the protected mark", name);
        CHECK(k == PK_K_GAME_NAME || !strstr(s, pk_text(PK_K_GAME_NAME)),
              "%s: says the game's name itself instead of {game}", name);
        for (const char *p = s; *p; p++) {
            if (*p == '}') CHECK(0, "%s: a stray }", name);
            if (*p != '{') continue;
            const char *e = p + 1;
            while (*e && *e != '}') e++;
            CHECK(*e == '}' && known(p + 1, (int)(e - p - 1)), "%s: unknown placeholder at \"%s\"", name, p);
            if (*e) p = e;
        }
        if (!strncmp(name, "CAP_", 4))
            CHECK(!has_word_ci(s, "you") && !has_word_ci(s, "your"), "%s: a caption says you", name);
    }
    CHECK(!strcmp(pk_text(PK_K_CALL_WORD), "Last card!") && !strcmp(pk_text(PK_K_CAUGHT_WORD), "Caught you!"),
          "the call-out and the catch are our own words (D2)");
    CHECK(PK_RULES_N == 8, "eight rules");
}

static const char *const NAMES[PK_MAX_SEATS] = { "Ana", "Bo", "Cy", "Di", "Ed", "Flo", "Gus", "Hal" };

static void tape_is(int seats, const char *want, const char *what)
{
    char out[256];
    int n = pk_say_caption_of(TAPE, TAPE_N, seats, NAMES, out, sizeof out);
    CHECK(n >= 0 && !strcmp(out, want), "%s: \"%s\", want \"%s\"", what, n >= 0 ? out : "(error)", want);
}

static void cap_is(const PkGame *g, int bubble, const char *want, const char *what)
{
    char out[256];
    int n = pk_say_caption(g, bubble, NAMES, out, sizeof out);
    CHECK(n >= 0 && !strcmp(out, want), "%s: \"%s\", want \"%s\"", what, n >= 0 ? out : "(error)", want);
}

/* Play a scripted bubble on a real game: `seat` draws `draws` times, then
 * plays the first playable card (wilds get suit 2) or passes. */
static void turn(PkGame *g, int draws)
{
    int s = g->turn;
    for (int i = 0; i < draws; i++) pk_apply(g, s, DRAW);
    for (int p = 0; p < g->hand_n[s]; p++)
        if (pk_can_play(g, s, p)) {
            uint8_t c = g->hand[s][p];
            pk_apply(g, s, pk_is_wild(c) && g->hand_n[s] > 1 ? PLAYW(p, 2) : PLAY(p));
            pk_seal(g);
            return;
        }
    if (!pk_is_legal(g, s, PASS)) pk_apply(g, s, DRAW);
    pk_apply(g, s, PASS);
    pk_seal(g);
}

static void t_captions(void)
{
    TEST("6.2 captions");
    uint8_t seed[32];
    seed_of(seed, 21);
    PkGame g;
    pk_new(&g, seed, 3);
    cap_is(&g, 0, "Cards dealt. Bo goes first", "the deal");

    /* the goldens are read back from the game, so they follow the deal */
    char want[256], card[64];
    for (int b = 0; b < 30 && !g.over; b++) {
        int s = g.turn;
        int drew = b % 3 == 2 ? 2 : 0;
        int hand_before = g.hand_n[s];
        turn(&g, drew);
        char out[256];
        CHECK(pk_say_caption(&g, g.bubbles, NAMES, out, sizeof out) > 0, "bubble %d captions", g.bubbles);
        CHECK(!strchr(out, '{'), "bubble %d: \"%s\" has a hole", g.bubbles, out);
        uint8_t top = g.stack[g.stack_n - 1];
        int played = g.hand_n[s] < hand_before + drew;
        if (played && pk_is_number(top) && !g.over) {
            pk_say_card(top, card, sizeof card);
            if (drew) {
                const char *kv[] = { "who", NAMES[s], "n", "2", "card", card, 0 };
                pk_fill(want, sizeof want, pk_text(PK_K_CAP_DREW_AND_PLAYED), kv);
            } else {
                const char *kv[] = { "who", NAMES[s], "card", card, 0 };
                pk_fill(want, sizeof want, pk_text(PK_K_CAP_PLAYED), kv);
            }
            CHECK(!strncmp(out, want, strlen(want)), "bubble %d: \"%s\" starts \"%s\"", g.bubbles, out, want);
        }
    }

    /* hand-built positions: the catch, the say, the pass, the +2, the end */
    exposed3(&g);
    PkGame h = g;
    tape_start(); A(&h, 1, PLAY(0)); S(&h);
    tape_is(h.n, "Bo played 7 of circles. Cy to play", "a plain play, then the next turn");
    h = g;
    tape_start(); A(&h, 1, CALL(0)); A(&h, 1, PLAY(0)); S(&h);
    tape_is(h.n, "Bo caught Ana. Ana draws two", "a hit comes first");
    h = g;
    tape_start(); A(&h, 0, SAY); S(&h);
    tape_is(h.n, "Ana: Last card!", "a standalone say names nobody next");
    h = g;
    tape_start(); A(&h, 2, CALL(1)); S(&h);
    tape_is(h.n, "Cy called Bo wrong and draws one", "a miss");
    h = g;
    tape_start(); A(&h, 1, DRAW); A(&h, 1, PASS); S(&h);
    tape_is(h.n, "Bo drew 1 and passed. Cy to play", "draw and pass");

    /* a say and a turn in one bubble: the joint after "!" is a space */
    table(&h, 2, num(0, 5, 0), 0);
    give(&h, 0, num(1, 1, 0)); give(&h, 1, num(1, 2, 0)); give(&h, 1, num(1, 3, 0));
    h.exposed = 1;
    tape_start(); A(&h, 0, SAY); A(&h, 0, DRAW); A(&h, 0, PASS); S(&h);
    {
        char said[256];
        const char *al[PK_MAX_SEATS] = { "Al", "Bo" };
        pk_say_caption_of(TAPE, TAPE_N, h.n, al, said, sizeof said);
        CHECK(!strcmp(said, "Al: Last card! Al drew 1 and passed"), "say, then the turn: \"%s\"", said);
    }

    table(&h, 2, num(0, 5, 0), 0);
    give(&h, 0, plus2_(0, 0)); give(&h, 0, num(1, 1, 0)); give(&h, 0, num(1, 2, 0));
    tape_start(); A(&h, 0, PLAY(0)); S(&h);
    tape_is(h.n, "Ana played +2. Bo draws two", "a +2, and nothing that does not fit");

    table(&h, 2, num(0, 5, 0), 0);
    give(&h, 0, rev_(0, 0)); give(&h, 0, num(1, 1, 0)); give(&h, 0, num(1, 2, 0));
    tape_start(); A(&h, 0, PLAY(0)); S(&h);
    tape_is(h.n, "Ana reversed and goes again", "a two-player reverse");

    table(&h, 2, num(0, 5, 0), 0);
    give(&h, 0, wild_(0));
    give(&h, 1, num(1, 1, 0));
    tape_start(); A(&h, 0, PLAYW(0, PK_NO_SUIT)); S(&h);
    tape_is(h.n, "Ana went out on a wild and wins", "a wild win");

    table(&h, 3, num(0, 5, 0), 0);
    give(&h, 0, num(1, 1, 0)); give(&h, 1, num(2, 1, 0)); give(&h, 2, num(3, 1, 0)); give(&h, 2, num(3, 2, 0));
    h.deck_n = 0;
    for (int i = 0; i < 3; i++) { tape_start(); A(&h, h.turn, PASS); S(&h); }
    tape_is(h.n, "Nobody can move. Ana wins with the fewest cards", "a stuck table");

    table(&h, 3, num(0, 5, 0), 0);
    give(&h, 0, wild4_(1)); give(&h, 0, num(1, 1, 0)); give(&h, 0, num(1, 2, 0));
    const char *longn[PK_MAX_SEATS] = { "Alexandra", "Bartholomew" };
    tape_start(); A(&h, 0, PLAYW(0, 3)); S(&h);
    char out[256];
    pk_say_caption_of(TAPE, TAPE_N, h.n, longn, out, sizeof out);
    CHECK(!strcmp(out, "Alexandra played wild +4, now diamonds. Bartholomew draws four"),
          "a first clause is kept whole however long: \"%s\"", out);
    CHECK(pk_say_caption_of(TAPE, TAPE_N, h.n, 0, out, sizeof out) > 0 && !strncmp(out, "Player 1 played", 15),
          "no names: the seat fallback (\"%s\")", out);
    CHECK(pk_say_caption_of(TAPE, TAPE_N, h.n, NAMES, out, 10) == -1,
          "a caption that does not fit the buffer says so");
}

static void t_screen(void)
{
    TEST("6.3 screen lines");
    PkGame g;
    exposed3(&g);
    char out[256];
    pk_say_headline(&g, 1, NAMES, out, sizeof out);
    CHECK(!strcmp(out, "Your turn"), "turn seat: \"%s\"", out);
    pk_say_headline(&g, 2, NAMES, out, sizeof out);
    CHECK(!strcmp(out, "Waiting on Bo"), "another seat: \"%s\"", out);
    pk_say_subline(&g, 1, NAMES, out, sizeof out);
    CHECK(!strcmp(out, "circles, or a 6"), "what matches: \"%s\"", out);
    pk_say_subline(&g, 2, NAMES, out, sizeof out);
    CHECK(!strcmp(out, "Bo, then you"), "who is before you: \"%s\"", out);
    pk_say_subline(&g, 0, NAMES, out, sizeof out);
    CHECK(!strcmp(out, "One card. Say it before they catch you"), "exposed: \"%s\"", out);
    pk_say_spoken_card(&g, 1, 0, out, sizeof out);
    CHECK(!strcmp(out, "7 of circles, playable"), "VoiceOver on a card: \"%s\"", out);
    pk_say_spoken_card(&g, 1, 1, out, sizeof out);
    CHECK(!strcmp(out, "2 of squares, does not play"), "and one that does not: \"%s\"", out);
    pk_say_spoken_stack(&g, out, sizeof out);
    CHECK(!strcmp(out, "Pile, 6 of circles on top, circles to match"), "the pile: \"%s\"", out);
    pk_say_deck_left(&g, out, sizeof out);
    CHECK(!strcmp(out, "92 left") || strstr(out, " left"), "deck: \"%s\"", out);
    pk_say_dir(&g, out, sizeof out);
    CHECK(!strcmp(out, "clockwise"), "direction at 3: \"%s\"", out);
    PkGame t;
    table(&t, 2, num(0, 5, 0), 0);
    pk_say_dir(&t, out, sizeof out);
    CHECK(!strcmp(out, ""), "no direction word at 2 players (D13)");
    pk_say_spoken_fan(NAMES, 3, out, sizeof out);
    CHECK(!strcmp(out, "Di's cards. Tap to catch them on one"), "a fan: \"%s\"", out);
    pk_say_lobby_caption(PK_SAY_INVITE, "Ana", out, sizeof out);
    CHECK(!strcmp(out, "Ana wants a game of Pick 'Em Up. Tap to join"), "the invite: \"%s\"", out);
    pk_say_rules_title(out, sizeof out);
    CHECK(!strcmp(out, "How to play Pick 'Em Up"), "the rules title: \"%s\"", out);
    for (int i = 0; i < PK_RULES_N; i++) CHECK(pk_say_rule(i, out, sizeof out) > 0, "rule %d", i);
    CHECK(!strcmp(pk_text(PK_K_SEND_HINT), "Send"), "the Send reminder's word, under the arrow (A15)");
    CHECK(pk_itoa(-305, out, sizeof out) == 4 && !strcmp(out, "-305") && pk_itoa(12, out, 2) == -1,
          "numbers without libc");
}

static void t_random_captions(void)
{
    TEST("6.2 captions over random games");
    char out[256];
    int long_ok = 0, checked = 0;
    for (uint32_t k = 0; k < 200; k++) {
        PkGame g;
        uint8_t seed[32];
        seed_of(seed, 60000u + k);
        pk_new(&g, seed, 2 + (int)(k % 7));
        for (int i = 0; i < 5000 && !(g.over && !g.b_open); i++) bot_step(&g);
        for (int b = 0; b <= g.bubbles; b++) {
            int n = pk_say_caption(&g, b, NAMES, out, sizeof out);
            checked++;
            CHECK(n > 0 && !strchr(out, '{'), "game %u bubble %d: \"%s\"", k, b, n > 0 ? out : "");
            /* more than one clause only while the line fits */
            const char *join = strstr(out, ". ");
            int one = !join || strstr(out, " draws two") || strstr(out, " draws four")
                   || !strncmp(out, "Nobody", 6) || !strncmp(out, "The game", 8) || !strncmp(out, "Cards dealt", 11);
            CHECK(one || pk_text_cols(out) <= PK_CAPTION_MAX, "game %u bubble %d: \"%s\" over the line",
                  k, b, out);
            long_ok += !one;
        }
    }
    CHECK(checked > 1000 && long_ok > 100, "exercised: %d captions, %d with two clauses", checked, long_ok);
}

int main(void)
{
    t_table();
    t_captions();
    t_screen();
    t_random_captions();
    return report("pk_say_test");
}
