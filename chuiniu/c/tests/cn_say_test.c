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
    static const char *known[] = { "who", "loser", "bid", "qty", "face", "n", "game", "loss", 0 };
    for (int k = 0; k < CN_K_COUNT; k++) {
        const char *s = cn_text(k);
        CHECK(s[0] || cn_key_may_be_empty(k), "%s is empty", cn_key_name(k));
        CHECK(!has_dash(s), "%s has an em or en dash", cn_key_name(k));
        size_t n = strlen(s);
        CHECK(n == 0 || s[n - 1] != '.', "%s ends in a full stop", cn_key_name(k));
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
    CHECK(!strcmp(out, "Your turn: raise or call Liar"), "%s", out);
    /* the owner's word for the call is Liar: the blood plate says it, and the lines that name the button do */
    CHECK(!strcmp(cn_text(CN_K_BTN_CALL), "Liar"), "the call's button: %s", cn_text(CN_K_BTN_CALL));
    cn_say_subline(&G, NAMES, out, sizeof out);
    CHECK(!strcmp(out, "Bid to beat: four 3s by Alex"), "%s", out);
    cn_say_staged(&G, bid(5, 2), out, sizeof out);
    CHECK(!strcmp(out, "Send to bid five 2s"), "%s", out);
    cn_say_staged(&G, call_move(), out, sizeof out);
    CHECK(!strcmp(out, "Send to call Liar on four 3s"), "%s", out);

    /* THE CALL: its caption names the call only (K8), the outcome the rest */
    CnEvent ev[8];
    int n = cn_plan_move(&G, call_move(), ev, 8);
    cn_say_caption_of(ev, n, NAMES, out, sizeof out);
    CHECK(!strcmp(out, "Bo calls four 3s"), "a call's caption: %s", out);
    cn_say_outcome_of(ev, n, NAMES, out, sizeof out);
    CHECK(!strcmp(out, "Bo calls. Four 3s was true, Bo loses a die"), "true: %s", out);
    /* the clauses a host sets apart, each as the line says it (package S: the loser's in blood) */
    cn_say_outcome_part_of(ev, n, NAMES, CN_SAY_PART_LOSS, out, sizeof out);
    CHECK(!strcmp(out, "Bo loses a die"), "the loser's clause: %s", out);
    cn_say_outcome_part_of(ev, n, NAMES, CN_SAY_PART_WIN, out, sizeof out);
    CHECK(!strcmp(out, ""), "no winner's clause before the end: '%s'", out);

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
    char whole[sizeof out];
    snprintf(whole, sizeof whole, "%s", out);
    cn_say_outcome_part_of(e2, 5, NAMES, CN_SAY_PART_LOSS, out, sizeof out);
    CHECK(!strcmp(out, "Alex loses a die") && strstr(whole, out), "the loser's clause, in the line: %s", out);
    cn_say_outcome_part_of(e2, 5, NAMES, CN_SAY_PART_WIN, out, sizeof out);
    CHECK(!strcmp(out, "Bo wins") && strstr(whole, out), "the winner's clause, in the line: %s", out);
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
    CHECK(!strcmp(out, "Your turn: call Liar"), "%s", out);
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
    CHECK(!strcmp(out, "Alex wants a game of Chui Niu"), "%s", out);
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

/* ---- the bubble's one line (docs_pkgY.md) -------------------------------------- */

/* The glyphs a name is made of: the widest of every width class cn_cap_width
 * knows (W and %, a Latin letter, Cyrillic, CJK, an emoji, the bismillah,
 * cuneiform, a digraph), narrow ones, a space and a letter with its mark. */
static const char *const GLYPHS[] = {
    "W", "%", "M", "m", "w", "i", " ", "\xC5\x92" /* Œ */, "\xD1\xB8" /* Ѹ */, "\xE5\x90\xB9" /* 吹 */,
    "\xF0\x9F\x98\x80" /* 😀 */, "\xEF\xB7\xBD" /* U+FDFD */, "\xF0\x92\x90\xAB" /* U+1242B */,
    "\xC7\x84" /* Ǆ */, "e\xCC\x81" /* é as e and U+0301 */, 0
};

/* Name `len` code points long from glyph `g` on, cycling every `step` glyphs
 * (0: all one glyph), within the wire's caps (CN_NAME_MAX_CHARS code points,
 * CN_NAME_MAX_BYTES bytes). */
static void make_name(char *out, int len, int g, int step)
{
    int ng = 0, bytes = 0, cps = 0;
    while (GLYPHS[ng]) ng++;
    out[0] = 0;
    for (int i = 0; cps < len; i++) {
        const char *u = GLYPHS[(g + (step ? i / step : 0)) % ng];
        int b = (int)strlen(u), c = 0;
        for (const char *p = u; *p; p++) c += (*p & 0xC0) != 0x80;
        if (bytes + b > CN_NAME_MAX_BYTES || cps + c > CN_NAME_MAX_CHARS) break;
        memcpy(out + bytes, u, (size_t)b + 1);
        bytes += b;
        cps += c;
    }
}

/* `line` names `who` whole, or a non-empty start of it (whole characters)
 * then the clip mark; *clipped says which. */
static int names_who(const char *line, const char *who, int *clipped)
{
    *clipped = 0;
    if (strstr(line, who)) return 1;
    const char *mark = cn_text(CN_K_CAP_CLIP);
    const char *m = strstr(line, mark);
    if (!m) return 0;
    for (const char *s = line; s < m; s++) {
        size_t k = (size_t)(m - s);
        /* a start of the name, never ending in a space */
        if (k && !strncmp(s, who, k) && (s == line || s[-1] == ' ') && m[-1] != ' ') { *clipped = 1; return 1; }
    }
    return 0;
}

/* CHECK that also clears `ok` */
#define HOLD(ok, cond, ...) do { int c_ = (cond); CHECK(c_, __VA_ARGS__); (ok) &= c_; } while (0)

static int fill_width(int key, const char *who, const char *bid, char *buf, int cap)
{
    const char *kv[] = { "who", who, "bid", bid, 0 };
    return cn_fill(buf, cap, cn_text(key), kv) < 0 ? 1 << 20 : cn_cap_width(buf);
}

/* One caption against the rule: within the budget; names who; the study's
 * sentence whenever it fits; a name clipped only when no whole-name form
 * fits; and the bid's quantity and face said, in words or in digits. */
static int one_line(const char *line, int len, const int *forms, const char *who, int q, int f, const char *what)
{
    char buf[512], words[64] = "", digits[16] = "";
    int ok = 1, clipped = 0;
    if (q) {
        cn_say_bid(q, f, 0, words, sizeof words);
        if (q == 1) snprintf(digits, sizeof digits, "1 %d", f);
        else snprintf(digits, sizeof digits, "%d %ds", q, f);
    }
    HOLD(ok, len > 0 && cn_cap_width(line) <= CN_CAP_BUDGET, "%s past the budget, %d of %d units: %s", what,
                cn_cap_width(line), CN_CAP_BUDGET, line);
    HOLD(ok, names_who(line, who, &clipped), "%s loses who (%s): %s", what, who, line);
    if (fill_width(forms[0], who, words, buf, sizeof buf) <= CN_CAP_BUDGET)
        HOLD(ok, !strcmp(line, buf), "%s fits the study's sentence %s but says %s", what, buf, line);
    int n = 0;
    while (forms[n + 1] >= 0) n++;
    if (clipped)
        HOLD(ok, fill_width(forms[n], who, q ? digits : "", buf, sizeof buf) > CN_CAP_BUDGET,
                    "%s clipped a name that fits: %s", what, line);
    if (q) {
        size_t L = strlen(line), w = strlen(words), d = strlen(digits);
        HOLD(ok, (L > w && !strcmp(line + L - w, words)) || (L > d && !strcmp(line + L - d, digits)),
                    "%s loses the bid %s: %s", what, words, line);
    }
    return ok;
}

static void test_one_line(void)
{
    TEST("the bubble's caption is one line");
    /* the width table's edges, as cn_say.h says them */
    CHECK(cn_cap_width("W") == 132 && cn_cap_width("%") == 132 && cn_cap_width("i") == 34, "ASCII");
    CHECK(cn_cap_width("e\xCC\x81") == cn_cap_width("e"), "a mark rides free");
    CHECK(cn_cap_width("\xF0\x92\x90\xAB") == 80 * CN_CAP_UNIT, "cuneiform");
    CHECK(cn_cap_width(0) == 0 && cn_cap_width("") == 0, "nothing");

    static const int START[] = { CN_K_CAP_START, CN_K_CAP_START_SHORT, -1 };
    static const int BID[] = { CN_K_CAP_BID, -1 };
    static const int CALL[] = { CN_K_CAP_CALL, -1 };
    static const int INVITE[] = { CN_K_CAP_INVITE, CN_K_CAP_INVITE_SHORT, -1 };
    static const int JOINED[] = { CN_K_CAP_JOINED, -1 };
    static const int LEFT[] = { CN_K_CAP_LEFT, -1 };
    int ng = 0, lines = 0, widest = 0, bad = 0;
    while (GLYPHS[ng]) ng++;
    char name[CN_NAME_MAX_BYTES + 1], line[512], widest_line[512] = "";
    for (int len = 1; len <= CN_NAME_MAX_CHARS + 1; len++)
        for (int g = 0; g < ng; g++)
            for (int step = 0; step <= 3; step++) {
                const char *names[CN_MAX_SEATS];
                /* len past the cap is the fallback name ("Player 6") */
                if (len > CN_NAME_MAX_CHARS) { name[0] = 0; } else make_name(name, len, g, step);
                for (int s = 0; s < CN_MAX_SEATS; s++) names[s] = name;
                char who[64];
                cn_say_seat(names, CN_MAX_SEATS - 1, who, sizeof who);
                CnEvent ev;
                memset(&ev, 0, sizeof ev);
                ev.seat = CN_MAX_SEATS - 1;
                ev.kind = CN_EV_ROUND;
                int n = cn_say_caption_of(&ev, 1, names, line, sizeof line);
                bad += !one_line(line, n, START, who, 0, 0, "the start");
                lines++;
                if (cn_cap_width(line) > widest) { widest = cn_cap_width(line); strcpy(widest_line, line); }
                for (int q = 1; q <= CN_MAX_DICE; q++)
                    for (int f = 2; f <= CN_FACES; f++)
                        for (int k = 0; k < 2; k++) {
                            ev.kind = k ? CN_EV_CALL : CN_EV_BID; ev.q = (uint8_t)q; ev.f = (uint8_t)f; ev.move = 1;
                            n = cn_say_caption_of(&ev, 1, names, line, sizeof line);
                            bad += !one_line(line, n, k ? CALL : BID, who, q, f, k ? "a call" : "a bid");
                            lines++;
                            if (cn_cap_width(line) > widest) { widest = cn_cap_width(line); strcpy(widest_line, line); }
                        }
                const int *lobby[3] = { INVITE, JOINED, LEFT };
                for (int w = 0; w < 3; w++) {
                    n = cn_say_lobby_caption(w, who, line, sizeof line);
                    bad += !one_line(line, n, lobby[w], who, 0, 0, "the lobby");
                    lines++;
                    if (cn_cap_width(line) > widest) { widest = cn_cap_width(line); strcpy(widest_line, line); }
                }
                if (bad > 20) { CHECK(0, "too many failures, stopping"); return; }
            }
    printf("  %d captions, the widest %d of %d units: %s\n", lines, widest, CN_CAP_BUDGET, widest_line);

    /* THE USUAL CASE keeps the study's words */
    const char *const usual[] = { "Alex", "Bo", 0 };
    CnEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = CN_EV_ROUND;
    cn_say_caption_of(&ev, 1, usual, line, sizeof line);
    CHECK(!strcmp(line, "Dice rolled. Alex bids first"), "%s", line);
    ev.kind = CN_EV_BID; ev.seat = 1; ev.q = 12; ev.f = 6; ev.move = 1;
    cn_say_caption_of(&ev, 1, usual, line, sizeof line);
    CHECK(!strcmp(line, "Bo bid twelve 6s"), "%s", line);
    /* THE STEPS, in order: the filler, the digits, the name */
    const char *const long_[] = { "Maximiliana Wolf", "Maximiliana Wolf", 0 };
    ev.kind = CN_EV_ROUND; ev.seat = 0; ev.move = 0;
    cn_say_caption_of(&ev, 1, long_, line, sizeof line);
    CHECK(!strcmp(line, "Maximiliana Wolf bids first"), "the filler goes: %s", line);
    ev.kind = CN_EV_CALL; ev.seat = 0; ev.q = 12; ev.f = 6; ev.move = 1;
    cn_say_caption_of(&ev, 1, long_, line, sizeof line);
    CHECK(!strcmp(line, "Maximiliana Wolf calls twelve 6s"), "the study's words while they fit: %s", line);
    const char *const em[] = { "MMMMMMMMMMMM", 0 };
    cn_say_caption_of(&ev, 1, em, line, sizeof line);
    CHECK(!strcmp(line, "MMMMMMMMMMMM calls 12 6s"), "the digits: %s", line);
    const char *const wide[] = { "WWWWWWWWWWWWWWWW", 0 };
    cn_say_caption_of(&ev, 1, wide, line, sizeof line);
    CHECK(!strcmp(line, "WWWWWWWW\xE2\x80\xA6 calls 12 6s"), "the digits, then the name: %s", line);
    cn_say_lobby_caption(CN_SAY_INVITE, "Maximiliana Wolf", line, sizeof line);
    CHECK(!strcmp(line, "Maximiliana Wolf wants a game"), "%s", line);
    cn_say_lobby_caption(CN_SAY_INVITE, "WWWWWW WWWWWWWWW", line, sizeof line);
    CHECK(!strcmp(line, "WWWWWW\xE2\x80\xA6 wants a game"), "a clip at a space drops it: %s", line);

    /* EVERY CAPTION OF REAL GAMES, the widest names at every seat */
    uint8_t seed[32];
    const char *const ws[CN_MAX_SEATS] = { "WWWWWWWWWWWWWWWW", "%%%%%%%%%%%%%%%%", "MMMMMMMMMMMMMMMM",
                                            "\xEF\xB7\xBD\xEF\xB7\xBD\xEF\xB7\xBD", "", "Alex" };
    for (uint32_t k = 0; k < 60; k++) {
        seed_wide(seed, 900 + k);
        cn_new(&G, seed, 2 + (int)(k % 5));
        while (G.phase != CN_PH_OVER) cn_apply(&G, G.turn, bot_move(&G));
        for (int m = 0; m <= G.hist_n; m++) {
            int r = cn_say_caption(&G, m, ws, line, sizeof line);
            int ok = 1;
            HOLD(ok, r > 0 && cn_cap_width(line) <= CN_CAP_BUDGET, "game %u move %d: %s", k, m, line);
            if (!ok) return;
        }
    }
}

int main(void)
{
    test_table();
    test_things();
    test_captions();
    test_screen_end();
    test_lobby_errors();
    test_one_line();
    return report("cn_say_test");
}
