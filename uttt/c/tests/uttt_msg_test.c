/* The message: its bytes, its seats and its words.
 *
 *     make -C uttt/c run        (and asan)
 *
 * Every ply of GAMES random games goes through the whole wire - bytes, text,
 * back - and every seat verdict has a row in a truth table. Each named
 * assertion below was watched go red with the rule it guards broken. */
#include "../src/uttt_msg.h"
#include "../src/uttt_say.h"
#include "../src/uttt_draw.h"
#include "../src/uttt_code.h"
#include "../../../shared/c/b32.h"
#include "../../../shared/c/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define OK(c, what) do { checks++; if (!(c)) { fails++; \
    printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, what); } } while (0)

static uint64_t rs = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void)
{
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (uint32_t)(rs >> 11);
}

static void tag_of(const char *who, int32_t seed, uint8_t t[UTM_TAG_LEN])
{
    utm_tag(seed, (const uint8_t *)who, (int)strlen(who), t);
}

static int same_msg(const UtmMsg *a, const UtmMsg *b)
{
    if (a->seed != b->seed || a->sealed != b->sealed) return 0;
    if (memcmp(a->o, b->o, UTM_TAG_LEN)) return 0;
    if (a->sealed && memcmp(a->x, b->x, UTM_TAG_LEN)) return 0;
    if (a->game.n_plies != b->game.n_plies) return 0;
    if (memcmp(a->game.move, b->game.move, a->game.n_plies)) return 0;
    return a->game.over == b->game.over && a->game.turn == b->game.turn;
}

/* ------------------------------------------------------------- base32 */
static void test_b32(void)
{
    /* RFC 4648 section 10, unpadded */
    static const char *in[]  = { "", "f", "fo", "foo", "foob", "fooba", "foobar" };
    static const char *out[] = { "", "MY", "MZXQ", "MZXW6", "MZXW6YQ", "MZXW6YTB", "MZXW6YTBOI" };
    for (int i = 0; i < 7; i++) {
        char t[32];
        int n = b32_encode((const unsigned char *)in[i], (int)strlen(in[i]), t, sizeof t);
        OK(n == (int)strlen(out[i]) && !strcmp(t, out[i]), "b32: RFC 4648 vectors");
        OK(n == B32_LEN((int)strlen(in[i])), "b32: B32_LEN is the encoded length");
        unsigned char back[32];
        int m = b32_decode(t, back, sizeof back);
        OK(m == (int)strlen(in[i]) && !memcmp(back, in[i], (size_t)m), "b32: decodes back");
    }
    unsigned char b[16];
    OK(b32_decode("mzxw6ytboi", b, sizeof b) == 6 && !memcmp(b, "foobar", 6),
       "b32: lower case reads the same");
    OK(b32_decode("MZX.W6/YTBOI", b, sizeof b) == 6 && !memcmp(b, "foobar", 6),
       "b32: stray characters are skipped");
    OK(b32_decode("MZXW6-YTBOI", b, sizeof b) == 3, "b32: a dash ends the code");
    OK(b32_decode("MZXW6YTBOI", b, 5) == -1, "b32: decode refuses a short buffer");
    char t[4];
    OK(b32_encode((const unsigned char *)"foobar", 6, t, sizeof t) == -1,
       "b32: encode refuses a short buffer");

    /* long inputs: the bit accumulator must not overflow */
    for (int trial = 0; trial < 200; trial++) {
        unsigned char r[120], back[120];
        char s[200];
        int n = (int)(rnd() % 120);
        for (int i = 0; i < n; i++) r[i] = (unsigned char)rnd();
        int w = b32_encode(r, n, s, sizeof s);
        OK(w == B32_LEN(n), "b32: random length");
        OK(b32_decode(s, back, sizeof back) == n && !memcmp(r, back, (size_t)n),
           "b32: random bytes round-trip");
    }
}

/* ---------------------------------------------------------- the wire */
static void round_trip(const UtmMsg *m, const char *what)
{
    uint8_t b[UTM_MAX_BYTES];
    char t[UTM_MAX_TEXT];
    UtmMsg back;
    int n = utm_encode(m, b, sizeof b);
    OK(n > 0 && n <= UTM_MAX_BYTES, what);
    OK(utm_decode(b, n, &back) == UTM_EOK && same_msg(m, &back), "wire: bytes round-trip");
    int tn = utm_text_encode(m, t, sizeof t);
    OK(tn > 3 && tn < UTM_MAX_TEXT && !strncmp(t, "?m=", 3), "wire: text is ?m=<base32>");
    memset(&back, 0, sizeof back);
    OK(utm_text_decode(t, &back) == UTM_EOK && same_msg(m, &back), "wire: text round-trip");
}

static int max_text;

static void test_games(int games)
{
    long plies = 0;
    for (int g = 0; g < games; g++) {
        int32_t seed = (int32_t)rnd() | 1;
        uint8_t a[UTM_TAG_LEN], b[UTM_TAG_LEN];
        tag_of("creator", seed, a);
        tag_of("joiner", seed, b);
        UtmMsg m;
        utm_open(&m, seed, a);
        round_trip(&m, "wire: an invitation encodes");
        while (!m.game.over) {
            uint8_t list[81];
            int n = uttt_legal(&m.game, list);
            int mv = list[rnd() % (unsigned)n];
            /* the joiner is X: it plays the even plies */
            const uint8_t *who = m.game.n_plies % 2 == 0 ? b : a;
            const uint8_t *other = who == b ? a : b;
            OK(!utm_play(&m, other, mv), "play: out of turn is refused");
            /* a refused move ends the game here rather than looping on it */
            if (!utm_play(&m, who, mv)) { OK(0, "play: the player on move may play"); break; }
            round_trip(&m, "wire: every ply encodes");
            char t[UTM_MAX_TEXT];
            int tn = utm_text_encode(&m, t, sizeof t);
            if (tn > max_text) max_text = tn;
            plies++;
        }
        OK(m.sealed && !memcmp(m.x, b, UTM_TAG_LEN) && !memcmp(m.o, a, UTM_TAG_LEN),
           "play: the joiner sits in X and the creator in O");
    }
    printf("  wire: %d games, %ld plies through bytes and text; longest link %d chars\n",
           games, plies, max_text);
}

/* Bytes with a correct check over whatever header the test wants - the only
 * way to reach decode's roster rules, since encode refuses to write them. */
static int forge(uint8_t *out, int32_t seed, int sealed, const uint8_t *o,
                 const uint8_t *x, const UtttGame *g)
{
    int n = 0;
    out[n++] = UTM_MAGIC; out[n++] = UTM_FORMAT;
    uint32_t u = (uint32_t)seed;
    out[n++] = (uint8_t)(u >> 24); out[n++] = (uint8_t)(u >> 16);
    out[n++] = (uint8_t)(u >> 8);  out[n++] = (uint8_t)u;
    out[n++] = sealed ? UTM_FLAG_SEALED : 0;
    memcpy(out + n, o, UTM_TAG_LEN); n += UTM_TAG_LEN;
    if (sealed) { memcpy(out + n, x, UTM_TAG_LEN); n += UTM_TAG_LEN; }
    int head = n;
    n += UTM_CHECK_LEN;
    int cn = uttt_encode(g, out + n, UTM_MAX_CODE);
    uint8_t d[32];
    Sha256 c;
    sha256_init(&c);
    sha256_update(&c, out, (size_t)head);
    sha256_update(&c, out + n, (size_t)cn);
    sha256_final(&c, d);
    memcpy(out + head, d, UTM_CHECK_LEN);
    return n + cn;
}

static void test_refusals(void)
{
    uint8_t a[UTM_TAG_LEN], b[UTM_TAG_LEN];
    tag_of("creator", 1234, a);
    tag_of("joiner", 1234, b);
    UtmMsg m, back;
    utm_open(&m, 1234, a);
    OK(utm_play(&m, b, 40) && utm_play(&m, a, 36), "refusal fixture plays");
    uint8_t buf[UTM_MAX_BYTES];
    int n = utm_encode(&m, buf, sizeof buf);

    OK(utm_decode(buf, 1, &back) == UTM_ESHORT, "decode: one byte is short");
    uint8_t t[UTM_MAX_BYTES];
    memcpy(t, buf, (size_t)n); t[0] ^= 1;
    OK(utm_decode(t, n, &back) == UTM_EMAGIC, "decode: wrong magic");
    memcpy(t, buf, (size_t)n); t[1] = UTM_FORMAT + 1;
    OK(utm_decode(t, n, &back) == UTM_EFORMAT, "decode: a newer format is refused");
    memcpy(t, buf, (size_t)n); t[6] |= 0x80;
    OK(utm_decode(t, n, &back) == UTM_EFLAGS, "decode: a reserved flag is refused");

    /* EVERY single-byte change past the format is refused, never misread:
     * a mixed-radix code has no redundancy of its own. */
    int misread = 0;
    for (int i = 2; i < n; i++)
        for (int bit = 0; bit < 8; bit++) {
            memcpy(t, buf, (size_t)n);
            t[i] ^= (uint8_t)(1u << bit);
            if (utm_decode(t, n, &back) == UTM_EOK) misread++;
        }
    OK(misread == 0, "decode: every flipped bit is refused");
    int cut = 0;
    for (int k = 1; k < n; k++)
        if (utm_decode(buf, k, &back) == UTM_EOK) cut++;
    OK(cut == 0, "decode: every truncation is refused");

    /* the roster and the game must tell one story */
    UtttGame g0, g1;
    uttt_init(&g0);
    uttt_init(&g1);
    uttt_play(&g1, 40);
    int fn = forge(t, 1234, 0, a, b, &g1);
    OK(utm_decode(t, fn, &back) == UTM_EROSTER, "decode: an open seat with a move on the board");
    fn = forge(t, 1234, 1, a, b, &g0);
    OK(utm_decode(t, fn, &back) == UTM_EROSTER, "decode: a sealed roster with no first move");
    fn = forge(t, 1234, 1, a, a, &g1);
    OK(utm_decode(t, fn, &back) == UTM_EROSTER, "decode: X and O the same person");
    fn = forge(t, 0, 0, a, b, &g0);
    OK(utm_decode(t, fn, &back) == UTM_EROSTER, "decode: a zero seed");
    fn = forge(t, 1234, 1, a, b, &g1);
    OK(utm_decode(t, fn, &back) == UTM_EOK, "decode: the forge itself is sound");

    UtmMsg bad = m;
    bad.sealed = 0;
    OK(utm_encode(&bad, buf, sizeof buf) == UTM_EROSTER, "encode: will not write what decode refuses");
    OK(utm_encode(&m, buf, 10) == UTM_ECAP, "encode: a short buffer");

    OK(utm_text_decode("?v=1&s=2", &back) == UTM_ETEXT, "text: no m= is refused");
    OK(utm_text_decode(0, &back) == UTM_ETEXT, "text: NULL is refused");
    char txt[UTM_MAX_TEXT], url[UTM_MAX_TEXT + 64];
    utm_text_encode(&m, txt, sizeof txt);
    snprintf(url, sizeof url, "https://example.invalid/x?q=1&m=%s&z=9#frag", txt + 3);
    OK(utm_text_decode(url, &back) == UTM_EOK && same_msg(&m, &back),
       "text: m= is found among other parameters and stops at &");
    snprintf(url, sizeof url, "?m=%s", txt + 3);
    for (char *p = url; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    url[1] = 'm';
    OK(utm_text_decode(url, &back) == UTM_EOK && same_msg(&m, &back),
       "text: case-folded on the way through still reads");
}

/* ----------------------------------------------------------- the seats */
static void test_seats(void)
{
    const int32_t seed = 1726990000;
    uint8_t a[UTM_TAG_LEN], b[UTM_TAG_LEN], c[UTM_TAG_LEN], a2[UTM_TAG_LEN], b2[UTM_TAG_LEN];
    tag_of("alex", seed, a);
    tag_of("vera", seed, b);
    tag_of("cleo", seed, c);           /* a third member of a group chat */
    tag_of("alex-reinstalled", seed, a2);
    tag_of("vera-reinstalled", seed, b2);

    uint8_t other[UTM_TAG_LEN];
    tag_of("alex", seed + 1, other);
    OK(memcmp(a, other, UTM_TAG_LEN) != 0, "tag: salted by the seed");
    tag_of("alex", seed, other);
    OK(!memcmp(a, other, UTM_TAG_LEN), "tag: the same device recomputes its own");
    OK(memcmp(a, b, UTM_TAG_LEN) != 0, "tag: two devices differ");

    /* THE FORMAT, pinned: a changed salt or byte order would orphan every
     * seat in every thread, so it has to be a failing test and not a
     * silent re-hash. */
    static const uint8_t golden[UTM_TAG_LEN] = {
        0x73, 0x05, 0x2a, 0xd6, 0x10, 0xaa, 0xfe, 0xd2, 0x95 };
    OK(!memcmp(a, golden, UTM_TAG_LEN), "tag: the format is frozen");

    UtmMsg inv;
    utm_open(&inv, seed, a);
    /* the invitation */
    OK(utm_seat(&inv, a) == UTM_SEAT_WAITING, "seat: the creator waits on their own invitation");
    OK(utm_seat(&inv, b) == UTM_SEAT_OPEN, "seat: anybody else may take it");
    OK(utm_seat(&inv, a2) == UTM_SEAT_OPEN, "seat: a reinstalled creator is anybody else");
    OK(utm_seat_mark(UTM_SEAT_OPEN) == UTTT_X, "seat: the open seat is X");
    OK(utm_seat_mark(UTM_SEAT_WAITING) == 0, "seat: the creator has no mark until somebody joins");
    OK(utm_can_move(&inv, b), "move: the joiner moves first");
    OK(!utm_can_move(&inv, a), "move: the creator cannot open their own game");
    OK(!utm_play(&inv, a, 40), "play: the creator cannot take their own seat");
    OK(utm_seat(&inv, a) == UTM_SEAT_WAITING && !inv.sealed, "play: a refused move changes nothing");

    /* the join IS the first move */
    UtmMsg j = inv;
    OK(utm_play(&j, b, 40), "join: the first move takes the seat");
    OK(j.sealed && !memcmp(j.x, b, UTM_TAG_LEN), "join: the roster seals with the joiner in X");
    OK(utm_seat(&j, b) == UTM_SEAT_X, "seat: the joiner is X");
    OK(utm_seat(&j, a) == UTM_SEAT_O, "seat: the creator is O");
    OK(utm_seat(&j, c) == UTM_SEAT_SPECTATOR, "seat: a third tap is a spectator");
    OK(utm_seat(&j, a2) == UTM_SEAT_SPECTATOR, "seat: a reinstalled creator watches");
    OK(utm_seat(&j, b2) == UTM_SEAT_SPECTATOR, "seat: a reinstalled joiner watches");
    OK(!utm_play(&j, c, 36), "play: a spectator cannot move");
    OK(!utm_can_move(&j, b) && utm_can_move(&j, a), "move: after X, O");

    /* undo: my own move only, and the joining move gives the seat back */
    UtmMsg u = j;
    OK(!utm_undo(&u, a), "undo: not the other player's move");
    OK(!utm_undo(&u, c), "undo: not a spectator");
    OK(utm_undo(&u, b), "undo: the joiner takes back the joining move");
    OK(!u.sealed && u.game.n_plies == 0, "undo: and the seat is open again");
    OK(utm_seat(&u, b) == UTM_SEAT_OPEN && utm_seat(&u, a) == UTM_SEAT_WAITING,
       "undo: back to the invitation");
    u = j;
    OK(utm_play(&u, a, 36) && utm_undo(&u, a) && u.sealed && u.game.n_plies == 1,
       "undo: O's own move comes back and the roster stays sealed");
    OK(!utm_undo(&inv, a) && !utm_undo(&inv, b), "undo: nothing to take back on an invitation");

    /* THE DOOR. No take-back, by the owner's decision: only Again. */
    OK(utm_door(&inv) == UTM_DOOR_NONE, "door: an invitation has none");
    OK(utm_door(&j) == UTM_DOOR_NONE, "door: a live game has none");
    UtmMsg over = inv;
    /* X takes the top-left, centre and bottom-right blocks in 25 plies */
    static const uint8_t win[] = { 79, 63, 5, 45, 8, 76, 42, 61, 70, 71, 78, 55, 15, 58, 36, 1, 11, 24, 4, 40, 39, 31, 80, 35, 0 };
    const uint8_t *who[2] = { b, a };
    int played = 1;
    for (unsigned i = 0; i < sizeof win && !over.game.over; i++)
        played &= utm_play(&over, who[i % 2], win[i]);
    OK(played && over.game.over == UTTT_X, "door: the finished fixture is won by X");
    OK(utm_door(&over) == UTM_DOOR_AGAIN, "door: a finished game offers Again");
}

/* ------------------------------------------------ two bubbles, one game */
static void test_prefer(void)
{
    const int32_t seed = 555;
    uint8_t a[UTM_TAG_LEN], b[UTM_TAG_LEN], c[UTM_TAG_LEN];
    tag_of("alex", seed, a);
    tag_of("vera", seed, b);
    tag_of("cleo", seed, c);
    UtmMsg inv, jb, jc, jb2, other;
    utm_open(&inv, seed, a);
    jb = inv; utm_play(&jb, b, 40);
    jc = inv; utm_play(&jc, c, 40);   /* the same square: only the joiner differs */
    jb2 = jb; utm_play(&jb2, a, 36);
    utm_open(&other, seed + 1, a);

    OK(utm_prefer(&inv, &inv) == 0, "prefer: identical is identical");
    OK(utm_prefer(&jb, &other) > 0, "prefer: a different game is the tapped one");
    OK(utm_prefer(&other, &jb) > 0, "prefer: whichever side it is on");
    OK(utm_prefer(&inv, &jb) > 0 && utm_prefer(&jb, &inv) < 0, "prefer: sealed beats its invitation");
    OK(utm_prefer(&jb2, &jb) < 0 && utm_prefer(&jb, &jb2) > 0, "prefer: more plies wins");

    UtmMsg alt = jb;
    utm_undo(&alt, b);
    utm_play(&alt, b, 41);
    OK(utm_prefer(&alt, &jb) < 0 && utm_prefer(&jb, &alt) < 0,
       "prefer: one roster, equal plies - my own draft, whichever it is");

    /* TWO PEOPLE REPLYING AT ONCE: both devices, holding both bubbles in
     * either order, pick the same one - the lower join key. */
    uint8_t kb[32], kc[32];
    utm_join_key(&jb, kb);
    utm_join_key(&jc, kc);
    int b_wins = memcmp(kb, kc, 32) < 0;
    int p1 = utm_prefer(&jb, &jc), p2 = utm_prefer(&jc, &jb);
    OK(p1 != 0 && p2 != 0 && (p1 < 0) != (p2 < 0), "prefer: two joiners is antisymmetric");
    OK((p1 < 0) == b_wins, "prefer: the lower join key wins");
    /* and the winner stays the winner for the rest of its game */
    UtmMsg jc2 = jc; utm_play(&jc2, a, 36);
    OK(utm_prefer(&jb2, &jc2) == (b_wins ? -1 : 1), "prefer: the key holds at any later ply");
    /* but a fork the creator has already answered beats one they have not */
    OK(utm_prefer(&jc, &jb2) > 0 && utm_prefer(&jb2, &jc) < 0,
       "prefer: an answered join beats an unanswered one");
    /* the loser, holding the winner's bubble, is a spectator of it */
    const UtmMsg *win = b_wins ? &jb : &jc;
    const uint8_t *loser = b_wins ? c : b;
    OK(utm_seat(win, loser) == UTM_SEAT_SPECTATOR, "prefer: the loser never sees themselves seated");
}

/* ---------------------------------------------------------- the taps */
static void test_hit(void)
{
    int all = 1;
    for (int mv = 0; mv < 81; mv++) {
        int b = mv / 9, c = mv % 9;
        float u = ((b % 3) * 3 + (c % 3) + .5f) / 9.f;
        float v = ((b / 3) * 3 + (c / 3) + .5f) / 9.f;
        if (uttt_hit(u, v) != mv) all = 0;
    }
    OK(all, "hit: every cell's centre is that cell");
    OK(uttt_hit(0.f, 0.f) == 0, "hit: the top-left corner");
    OK(uttt_hit(1.f, 1.f) == 80, "hit: the far corner belongs to the last cell");
    OK(uttt_hit(.34f, .01f) == 9, "hit: just right of the first line is block 1");
    OK(uttt_hit(.32f, .01f) == 2, "hit: just left of it is block 0's right column");
    OK(uttt_hit(.01f, .67f) == 54, "hit: the bottom-left block");
    OK(uttt_hit(-.01f, .5f) == -1 && uttt_hit(.5f, 1.01f) == -1, "hit: off the board is nothing");
}

/* ---------------------------------------------------------- the words */
static int has_em_dash(const char *s) { return strstr(s, "\xe2\x80\x94") != 0; }

static void say(int key, const UtttGame *g, int seat, char *out)
{
    int n = uttt_say(key, g, seat, out, 160);
    if (n < 0) out[0] = 0;
}

static void test_say(void)
{
    char s[160];
    UtttGame g;
    uttt_init(&g);
    say(UTTT_SAY_BUBBLE_HEADLINE, &g, UTM_SEAT_WAITING, s);
    OK(!strcmp(s, "A game?"), "say: an empty board asks");
    say(UTTT_SAY_CAPTION, &g, UTM_SEAT_WAITING, s);
    OK(!strcmp(s, "A game. Tap to take it."), "say: the invitation's caption, nobody named");
    OK(uttt_say_by(UTTT_SAY_CAPTION, &g, UTM_SEAT_WAITING, "$ALEX", s, sizeof s) > 0
       && !strcmp(s, "$ALEX wants a game. Tap to take it."),
       "say: the invitation's caption names its sender (UI.html 01)");
    OK(uttt_say_bubble_mark(&g) == 0, "say: an invitation's bubble draws no mark");
    say(UTTT_SAY_BUBBLE_PLACE, &g, UTM_SEAT_WAITING, s);
    OK(!strcmp(s, ""), "say: an invitation has no place line");
    say(UTTT_SAY_HEADLINE_PRE, &g, UTM_SEAT_OPEN, s);
    OK(!strcmp(s, "Your move"), "say: the joiner is on move");
    say(UTTT_SAY_SUBLINE, &g, UTM_SEAT_OPEN, s);
    OK(!strcmp(s, "Anywhere you like."), "say: and may go anywhere");

    uttt_play(&g, 41);                  /* centre block, middle-right cell -> block 5 */
    say(UTTT_SAY_CAPTION, &g, UTM_SEAT_X, s);
    OK(!strcmp(s, "Sent to the middle-right board."), "say: the caption names the destination");
    say(UTTT_SAY_BUBBLE_PLACE, &g, UTM_SEAT_X, s);
    OK(!strcmp(s, "middle right"), "say: the bubble's place line");
    say(UTTT_SAY_BUBBLE_HEADLINE, &g, UTM_SEAT_X, s);
    OK(!strcmp(s, "to play") && uttt_say_bubble_mark(&g) == UTTT_O,
       "say: the bubble names the side to play by its mark, never \"Your move\"");
    OK(uttt_say_by(UTTT_SAY_CAPTION, &g, UTM_SEAT_X, "$ALEX", s, sizeof s) > 0
       && !strcmp(s, "Sent to the middle-right board."),
       "say: a move's caption names nobody");
    say(UTTT_SAY_HEADLINE_PRE, &g, UTM_SEAT_X, s);
    OK(!strcmp(s, "Waiting on ") && uttt_say_headline_mark(&g, UTM_SEAT_X) == UTTT_O,
       "say: X waits on a drawn O");
    say(UTTT_SAY_SUBLINE, &g, UTM_SEAT_X, s);
    OK(!strcmp(s, "Middle right."), "say: where I sent them, capitalised");
    say(UTTT_SAY_HEADLINE_PRE, &g, UTM_SEAT_O, s);
    OK(!strcmp(s, "Your move") && uttt_say_headline_mark(&g, UTM_SEAT_O) == 0,
       "say: O is on move, words only");
    say(UTTT_SAY_WATCH_LINE, &g, UTM_SEAT_SPECTATOR, s);
    OK(!strcmp(s, "O to play"), "say: the spectator's line");

    say(UTTT_SAY_DOOR_AGAIN, &g, UTM_SEAT_X, s);
    OK(!strcmp(s, "Again"), "say: the end door (UI.html 06)");

    /* THE END, docs/UI.html 05-07: X on the top-left to bottom-right diagonal */
    static const uint8_t diag[] = { 79, 63, 5, 45, 8, 76, 42, 61, 70, 71, 78, 55, 15, 58, 36, 1, 11, 24, 4, 40, 39, 31, 80, 35, 0 };
    uttt_init(&g);
    for (unsigned i = 0; i < sizeof diag && !g.over; i++) uttt_play(&g, diag[i]);
    OK(g.over == UTTT_X && uttt_won_line(&g) == 6, "say: the end fixture is X on the diagonal");
    say(UTTT_SAY_SUBLINE, &g, UTM_SEAT_O, s);
    OK(!strcmp(s, "Top left, centre, bottom right."), "say: the end subline is the line, spoken");
    say(UTTT_SAY_CAPTION, &g, UTM_SEAT_O, s);
    {
        char want[64];
        snprintf(want, sizeof want, "X won on the diagonal. %d moves.", g.n_plies);
        OK(!strcmp(s, want), "say: the end caption names the line and the length");
        snprintf(want, sizeof want, "$ALEX won on the diagonal. %d moves.", g.n_plies);
        OK(uttt_say_by(UTTT_SAY_CAPTION, &g, UTM_SEAT_X, "$ALEX", s, sizeof s) > 0
           && !strcmp(s, want), "say: the end caption names the winner, who sent it (UI.html 05)");
    }
    say(UTTT_SAY_BUBBLE_HEADLINE, &g, UTM_SEAT_O, s);
    OK(!strcmp(s, "wins") && uttt_say_bubble_mark(&g) == UTTT_X,
       "say: the finished bubble draws the winner's mark");

    /* play games out and read every key at every ply from every seat */
    int dashes = 0, missing = 0, lines_said = 0;
    for (int game = 0; game < 200; game++) {
        uttt_init(&g);
        for (;;) {
            for (int seat = 0; seat <= UTM_SEAT_OPEN; seat++)
                for (int k = 0; k < UTTT_SAY_COUNT; k++) {
                    int n = uttt_say(k, &g, seat, s, sizeof s);
                    if (n < 0) missing++;
                    else if (has_em_dash(s)) dashes++;
                }
            if (g.over) break;
            uint8_t list[81];
            int n = uttt_legal(&g, list);
            uttt_play(&g, list[rnd() % (unsigned)n]);
        }
        if ((g.over == UTTT_X || g.over == UTTT_O)) {
            /* the spoken line names exactly the three blocks of the line */
            int li = uttt_won_line(&g);
            unsigned m = li < 0 ? 0 : uttt_line_mask(li);
            unsigned held = g.bm[g.over - 1];
            say(UTTT_SAY_SUBLINE, &g, UTM_SEAT_X, s);
            int named = 0;
            for (int blk = 0; blk < 9; blk++) {
                if (!((m >> blk) & 1u)) continue;
                char nm[32];
                snprintf(nm, sizeof nm, "%s", uttt_place_name(blk, 0));
                if (strstr(s, nm) || (nm[0] - 'a' + 'A' == s[0] && strstr(s, nm + 1))) named++;
            }
            OK(li >= 0 && (held & m) == m && named == 3,
               "say: the end subline speaks the winning line");
            lines_said++;
        }
        if (game == 0) {
            say(UTTT_SAY_BUBBLE_PLACE, &g, UTM_SEAT_X, s);
            char want[32];
            snprintf(want, sizeof want, "%d moves", g.n_plies);
            OK(!strcmp(s, want), "say: a finished bubble counts the moves");
        }
    }
    OK(missing == 0, "say: every key answers at every ply from every seat");
    OK(lines_said > 0, "say: some finished game was won on a line");
    OK(dashes == 0, "say: no em dash anywhere");
    OK(uttt_say(UTTT_SAY_COUNT, &g, 0, s, sizeof s) == -1, "say: an unknown key is refused");
    OK(uttt_say(UTTT_SAY_UNREADABLE_SUBLINE, &g, 0, s, 8) == -1, "say: a short buffer is refused");
}

int main(int argc, char **argv)
{
    int games = argc > 1 ? atoi(argv[1]) : 10000;
    test_b32();
    test_games(games);
    test_refusals();
    test_seats();
    test_prefer();
    test_hit();
    test_say();
    printf("uttt_msg: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
