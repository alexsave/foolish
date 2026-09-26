/* The words, in every language the game carries.
 *
 *     make -C uttt/c run        (and asan)
 *
 * The tables are split one file per language (i18n/keys.h), and a split
 * table turns a missing key from a compile error into a silent English word
 * on a phone nobody here reads. This is what stands in for the compiler:
 *
 *   - every language fills every key, and only the keys that may be empty
 *     are empty;
 *   - every translation keeps English's placeholders and invents none;
 *   - every key fits its width (keys.h), and every caption each language can
 *     compose fits one transcript line (UTTT_CAPTION_MAX);
 *   - no em dash, no caption or label ending in a full stop, and the two
 *     yellow phrases appear word for word in the rules they are drawn in;
 *   - every sentence answers at every ply from every seat, in every language;
 *   - the phone's preference list resolves the way the sister product's does.
 *
 * Each assertion here was watched go red with the rule it guards broken. */
#include "../src/uttt.h"
#include "../src/uttt_say.h"
#include "../src/uttt_draw.h"
#include "../src/uttt_msg.h"
#include "../src/uttt_lang.h"
#include <stdio.h>
#include <string.h>

static int fails, checks;
#define OK(c, ...) do { checks++; if (!(c)) { fails++; \
    printf("  FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t rs = 0x2545f4914f6cdd1dull;
static uint32_t rnd(void)
{
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (uint32_t)(rs >> 11);
}

static int has_em_dash(const char *s) { return strstr(s, "\xe2\x80\x94") != 0; }

/* Ends in a full stop: a Latin one or the ideographic one. */
static int ends_in_stop(const char *s)
{
    size_t n = strlen(s);
    if (n && s[n - 1] == '.') return 1;
    return n >= 3 && !strcmp(s + n - 3, "\xe3\x80\x82");
}

/* THE PLACEHOLDERS A STRING NAMES, as a bit set; -1 for one nobody fills. */
static const char *const PH[] = { "who", "mark", "line", "moves", "n", "board", "cell", "state" };
enum { PH_MOVES = 1 << 3, PH_N = 1 << 4 };
static int placeholders(const char *s)
{
    int set = 0;
    for (const char *p = strchr(s, '{'); p; p = strchr(p + 1, '{')) {
        const char *e = strchr(p, '}');
        if (!e) return -1;
        int hit = -1;
        for (int i = 0; i < (int)(sizeof PH / sizeof *PH); i++)
            if ((size_t)(e - p - 1) == strlen(PH[i]) && !strncmp(p + 1, PH[i], strlen(PH[i]))) hit = i;
        if (hit < 0) return -1;
        set |= 1 << hit;
    }
    return set;
}

/* The halves round a drawn mark carry their gap as a space (keys.h), so they
 * are the only keys whose edges may be white. */
static int edges_may_be_space(int k)
{
    return k == UT_K_HEAD_WAITING_PRE || k == UT_K_HEAD_WAITING_POST ||
           k == UT_K_HEAD_WINS_PRE || k == UT_K_HEAD_WINS_POST ||
           k == UT_K_WATCH_TO_PLAY || k == UT_K_WATCH_TOOK;
}

static int is_rule(int k) { return k >= UT_K_RULES_TITLE && k <= UT_K_RULES_TINT; }

/* ---------------------------------------------------------- the tables */
static void test_tables(void)
{
    int holes = 0, empties = 0, lost = 0, wide = 0, dashes = 0, stops = 0, edges = 0;
    for (int l = 0; l < uttt_lang_count(); l++) {
        const char *code = uttt_lang_code(l);
        OK(code[0], "language %d has a code in the shared registry", l);
        for (int k = 0; k < UT_K_COUNT; k++) {
            const char *s = uttt_text_in(l, k), *en = uttt_text_in(0, k);
            const char *name = uttt_key_name(k);
            if (!s) { holes++; OK(0, "%s is missing %s", code, name); continue; }
            if (!s[0] && !uttt_key_may_be_empty(k)) { empties++; OK(0, "%s leaves %s empty", code, name); }
            int want = placeholders(en), got = placeholders(s);
            /* {n} may stand in for {moves} where the grammar wants a case the
             * MOVES_ forms are not in (keys.h) */
            if (got >= 0 && (want & PH_MOVES) && (got & PH_N) && !(got & PH_MOVES))
                got = (got & ~PH_N) | PH_MOVES;
            if ((want & PH_MOVES) && (got & PH_N) && (got & PH_MOVES)) got = -1;
            if (got != want) { lost++; OK(0, "%s %s does not name English's placeholders: \"%s\"", code, name, s); }
            int max = uttt_key_max(k), cols = uttt_text_cols(s);
            if (max && cols > max) { wide++; OK(0, "%s %s is %d columns, over its %d: \"%s\"", code, name, cols, max, s); }
            if (has_em_dash(s)) { dashes++; OK(0, "%s %s has an em dash", code, name); }
            if (!is_rule(k) && ends_in_stop(s)) { stops++; OK(0, "%s %s ends in a full stop: \"%s\"", code, name, s); }
            size_t n = strlen(s);
            if (n && !edges_may_be_space(k) && (s[0] == ' ' || s[n - 1] == ' ')) {
                edges++; OK(0, "%s %s starts or ends with a space: \"%s\"", code, name, s);
            }
        }
        /* a pair round a mark says something */
        OK(uttt_text_in(l, UT_K_HEAD_WAITING_PRE) && uttt_text_in(l, UT_K_HEAD_WAITING_POST) &&
           (uttt_text_in(l, UT_K_HEAD_WAITING_PRE)[0] || uttt_text_in(l, UT_K_HEAD_WAITING_POST)[0]),
           "%s: waiting on a mark says something beside it", code);
        OK(uttt_text_in(l, UT_K_HEAD_WINS_PRE) && uttt_text_in(l, UT_K_HEAD_WINS_POST) &&
           (uttt_text_in(l, UT_K_HEAD_WINS_PRE)[0] || uttt_text_in(l, UT_K_HEAD_WINS_POST)[0]),
           "%s: a mark's win says something beside it", code);
        /* the yellow phrases are drawn round words that are there */
        const char *r6 = uttt_text_in(l, UT_K_RULE_6), *r7 = uttt_text_in(l, UT_K_RULE_7);
        const char *ol = uttt_text_in(l, UT_K_RULES_OUTLINE), *ti = uttt_text_in(l, UT_K_RULES_TINT);
        OK(r6 && ol && ol[0] && strstr(r6, ol), "%s: the yellow outline's words are in rule 6", code);
        OK(r7 && ti && ti[0] && strstr(r7, ti), "%s: the yellow tint's words are in rule 7", code);
    }
    OK(holes == 0, "every language fills every key");
    OK(empties == 0, "only the halves round a mark may be empty");
    OK(lost == 0, "every translation keeps English's placeholders");
    OK(wide == 0, "every key fits its width in every language");
    OK(dashes == 0, "no em dash in any language");
    OK(stops == 0, "no caption or label ends in a full stop, in any language");
    OK(edges == 0, "no stray space at the edge of a string");
    OK(uttt_key_max(UT_K_BUBBLE_WINS) > 0 && uttt_key_max(UT_K_DOOR_AGAIN) > 0,
       "the bubble's words and the doors have a width");
}

/* ---------------------------------------------------- what it composes */
static void test_captions(int l)
{
    char s[256];
    const char *code = uttt_lang_code(l);
    int longest = 0, bad = 0, unfilled = 0;
    char worst[256] = "";
    for (int plies = 0; plies <= 81; plies++)
        for (int over = 0; over <= 3; over++)
            for (int turn = UTTT_X; turn <= UTTT_O; turn++)
                for (int block = -1; block <= 9; block++)
                    for (int line = -1; line < 8; line++) {
                        int k = uttt_caption(over, turn, block, line, plies, NULL, s, sizeof s);
                        if (k < 0 || k != (int)strlen(s)) { bad++; continue; }
                        int c = uttt_text_cols(s);
                        if (c > longest) { longest = c; strcpy(worst, s); }
                        if (strchr(s, '{')) unfilled++;
                        if (ends_in_stop(s)) bad++;
                    }
    printf("  %s: longest caption %d columns, \"%s\"\n", code, longest, worst);
    OK(bad == 0, "%s: every caption answers, without a full stop", code);
    OK(unfilled == 0, "%s: no caption leaves a placeholder unfilled", code);
    OK(longest <= UTTT_CAPTION_MAX, "%s: every caption fits one transcript line (%d > %d: \"%s\")",
       code, longest, UTTT_CAPTION_MAX, worst);
    OK(uttt_caption(UTTT_X, UTTT_O, 0, 6, 25, "$ALEX", s, sizeof s) > 0 && strstr(s, "$ALEX"),
       "%s: the winner's name reaches the caption", code);
    OK(uttt_caption(0, UTTT_X, -1, -1, 0, "$ALEX", s, sizeof s) > 0 && strstr(s, "$ALEX"),
       "%s: the invitation names its sender", code);
}

static void test_sentences(int l)
{
    char s[256];
    const char *code = uttt_lang_code(l);
    int missing = 0, dashes = 0, unfilled = 0, stops = 0, wide = 0;
    UtttGame g;
    for (int game = 0; game < 40; game++) {
        uttt_init(&g);
        for (;;) {
            for (int seat = 0; seat <= UTM_SEAT_OPEN; seat++)
                for (int k = 0; k < UTTT_SAY_COUNT; k++) {
                    int n = uttt_say(k, &g, seat, s, sizeof s);
                    if (n < 0) { missing++; continue; }
                    if (has_em_dash(s)) dashes++;
                    if (strchr(s, '{')) unfilled++;
                    if (n && ends_in_stop(s)) stops++;
                }
            /* the bubble's place line wraps onto at most two lines of ~95
             * points (UtttBubble.swift) */
            if (uttt_say(UTTT_SAY_BUBBLE_PLACE, &g, UTM_SEAT_X, s, sizeof s) >= 0 && uttt_text_cols(s) > 20) wide++;
            for (int mv = 0; mv < 81; mv++)
                if (uttt_say_cell(&g, mv, s, sizeof s) < 0 || strchr(s, '{')) missing++;
            if (g.over) break;
            uint8_t list[81];
            int n = uttt_legal(&g, list);
            uttt_play(&g, list[rnd() % (unsigned)n]);
        }
    }
    OK(missing == 0, "%s: every sentence answers at every ply from every seat", code);
    OK(dashes == 0, "%s: no em dash in anything said", code);
    OK(unfilled == 0, "%s: no sentence leaves a placeholder unfilled", code);
    OK(stops == 0, "%s: nothing said ends in a full stop", code);
    OK(wide == 0, "%s: the bubble's move count fits its two lines", code);
    OK(uttt_rules_count() == 8, "%s: eight rules", code);
    for (int i = 0; i < uttt_rules_count(); i++)
        OK(uttt_rules_line(i)[0], "%s: rule %d has words", code, i + 1);
    OK(uttt_rules_title()[0], "%s: the rules have a title", code);
    /* the sheet draws the outline round rule 6's phrase and the tint behind
     * rule 7's, found by the kernel in this language's words */
    for (int i = 0; i < uttt_rules_count(); i++) {
        int at = -1, len = -1, k = uttt_rules_yellow(i, &at, &len);
        const char *line = uttt_rules_line(i);
        int want = i == 5 ? UTTT_RULES_OUTLINE : i == 6 ? UTTT_RULES_TINT : UTTT_RULES_PLAIN;
        const char *phrase = i == 5 ? uttt_text(UT_K_RULES_OUTLINE) : i == 6 ? uttt_text(UT_K_RULES_TINT) : "";
        OK(k == want && len == (int)strlen(phrase) && at >= 0 && at + len <= (int)strlen(line) &&
           !strncmp(line + at, phrase, (size_t)len),
           "%s: rule %d marks %s", code, i + 1, want == UTTT_RULES_PLAIN ? "nothing" : phrase);
    }
}

/* The kernel speaks the language it was set to, and English where a table
 * has nothing. */
static void test_switch(void)
{
    char en[128], ru[128];
    UtttGame g;
    uttt_init(&g);
    uttt_lang_set(0);
    uttt_say(UTTT_SAY_DOOR_AGAIN, &g, UTM_SEAT_X, en, sizeof en);
    int ruL = uttt_lang_prefer("ru-RU");
    uttt_say(UTTT_SAY_DOOR_AGAIN, &g, UTM_SEAT_X, ru, sizeof ru);
    OK(!strcmp(uttt_lang_code(ruL), "ru") && strcmp(en, ru) && !strcmp(ru, uttt_text_in(ruL, UT_K_DOOR_AGAIN)),
       "switch: the door speaks the language set");
    OK(!strcmp(uttt_rules_line(0), uttt_text_in(ruL, UT_K_RULE_1)), "switch: so do the rules");
    uttt_lang_set(99);
    OK(uttt_lang() == 0, "switch: an unknown language is English");
    OK(uttt_text(UT_K_COUNT)[0] == '\0' && uttt_text(-1)[0] == '\0', "switch: an unknown key is empty, never NULL");
}

static void test_prefer(void)
{
    static const struct { const char *tags, *want; } T[] = {
        { "de-CH,fr-CH,en-US", "de" },  /* the first one carried wins          */
        { "ca-ES,es-ES", "es" },        /* Catalan is not carried; Spanish is  */
        { "nb-NO", "no" }, { "nn-NO", "no" },   /* nobody's phone says "no"    */
        { "in-ID", "id" }, { "iw-IL", "he" },   /* the pre-1989 codes          */
        { "zh-Hant-TW", "zh" }, { "pt-BR", "pt" }, { "PT_br", "pt" },
        { "xx,yy-ZZ", "en" }, { "", "en" }, { "en-GB", "en" },
        { " ja-JP , ko-KR", "ja" },
        { "ido,it", "it" },             /* a longer subtag is not its prefix   */
    };
    for (unsigned i = 0; i < sizeof T / sizeof *T; i++) {
        int l = uttt_lang_prefer(T[i].tags);
        OK(!strcmp(uttt_lang_code(l), T[i].want), "prefer: \"%s\" is %s, not %s", T[i].tags, T[i].want, uttt_lang_code(l));
    }
    uttt_lang_prefer(NULL);
    OK(uttt_lang() == 0, "prefer: no list is English");
}

static int lang_of(const char *code)
{
    for (int l = 0; l < uttt_lang_count(); l++)
        if (!strcmp(uttt_lang_code(l), code)) return l;
    return -1;
}

static void test_plural(void)
{
    enum { ONE, FEW, MANY, OTHER };
    int ru = lang_of("ru"), pl = lang_of("pl"), cs = lang_of("cs"), ro = lang_of("ro"),
        ar = lang_of("ar"), en = lang_of("en"), fr = lang_of("fr"), ja = lang_of("ja");
    OK(uttt_plural(ru, 1) == ONE && uttt_plural(ru, 21) == ONE && uttt_plural(ru, 11) == MANY &&
       uttt_plural(ru, 22) == FEW && uttt_plural(ru, 12) == MANY && uttt_plural(ru, 25) == MANY,
       "plural: Russian 21 ход, 22 хода, 25 ходов, 11 ходов");
    OK(uttt_plural(pl, 1) == ONE && uttt_plural(pl, 21) == MANY && uttt_plural(pl, 22) == FEW &&
       uttt_plural(pl, 14) == MANY, "plural: Polish 21 ruchów, 22 ruchy");
    OK(uttt_plural(cs, 3) == FEW && uttt_plural(cs, 22) == OTHER, "plural: Czech 3 tahy, 22 tahů");
    OK(uttt_plural(ro, 19) == FEW && uttt_plural(ro, 20) == OTHER && uttt_plural(ro, 101) == FEW,
       "plural: Romanian 19 mutări, 20 de mutări");
    OK(uttt_plural(ar, 5) == FEW && uttt_plural(ar, 58) == MANY && uttt_plural(ar, 100) == OTHER,
       "plural: Arabic 5 نقلات, 58 نقلة");
    OK(uttt_plural(en, 1) == ONE && uttt_plural(en, 0) == OTHER && uttt_plural(fr, 0) == ONE &&
       uttt_plural(ja, 1) == OTHER, "plural: English, French, Japanese");
    char s[64];
    uttt_lang_set(ru);
    UtttGame g;
    uttt_init(&g);
    static const uint8_t diag[] = { 79, 63, 5, 45, 8, 76, 42, 61, 70, 71, 78, 55, 15, 58, 36, 1, 11, 24, 4, 40, 39, 31, 80, 35, 0 };
    for (unsigned i = 0; i < sizeof diag && !g.over; i++) uttt_play(&g, diag[i]);
    OK(g.n_plies == 25 && uttt_say(UTTT_SAY_BUBBLE_PLACE, &g, UTM_SEAT_X, s, sizeof s) > 0 && !strcmp(s, "25 ходов"),
       "plural: the Russian bubble counts 25 ходов (\"%s\")", s);
    uttt_lang_set(0);
}

static void test_cols(void)
{
    OK(uttt_text_cols("X to play") == 9, "cols: ASCII is one a character");
    OK(uttt_text_cols("Ходит X") == 7, "cols: Cyrillic is one a character, not two bytes");
    OK(uttt_text_cols("Xの番") == 5, "cols: kana is two");
    OK(uttt_text_cols("한 판") == 5, "cols: Hangul is two");
    OK(uttt_text_cols("\xe0\xb8\x81\xe0\xb8\xb4") == 1, "cols: a Thai vowel above sets no width");
    OK(uttt_text_cols("") == 0 && uttt_text_cols(NULL) == 0, "cols: nothing is nothing");
    char s[16];
    const char *kv[] = { "a", "xyz", 0 };
    OK(uttt_fill(s, sizeof s, "<{a}{b}>", kv) == 8 && !strcmp(s, "<xyz{b}>"), "fill: a placeholder nobody fills stays written");
    OK(uttt_fill(s, 3, "{a}", kv) == -1, "fill: a short buffer is refused");
}

#ifdef UTTT_LANG_ENGLISH_ONLY
/* THE REPLAY PAGE'S BUILD (UTTT_LANG_ENGLISH_ONLY, uttt/c/Makefile): one
 * table, and every other language reads as English rather than as nothing. */
static void test_english_only(void)
{
    UtttGame g;
    char s[64];
    uttt_init(&g);
    int ru = uttt_lang_prefer("ru-RU");
    OK(!strcmp(uttt_lang_code(ru), "ru") && uttt_text_in(ru, UT_K_DOOR_AGAIN) == NULL,
       "english only: Russian is a language with no table");
    OK(uttt_say(UTTT_SAY_DOOR_AGAIN, &g, UTM_SEAT_X, s, sizeof s) > 0 && !strcmp(s, "Again"),
       "english only: a language with no table falls back to English");
    OK(uttt_caption(0, UTTT_X, -1, -1, 0, NULL, s, sizeof s) > 0 && !strcmp(s, "New game?"),
       "english only: so does the caption");
}
#endif

int main(void)
{
#ifdef UTTT_LANG_ENGLISH_ONLY
    test_english_only();
    printf("uttt_lang (english only): %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
#endif
    test_tables();
    for (int l = 0; l < uttt_lang_count(); l++) {
        uttt_lang_set(l);
        test_captions(l);
        test_sentences(l);
    }
    uttt_lang_set(0);
    test_switch();
    test_prefer();
    test_plural();
    test_cols();
    printf("uttt_lang: %d languages, %d keys, %d checks, %d failed\n",
           uttt_lang_count(), UT_K_COUNT, checks, fails);
    return fails ? 1 : 0;
}
