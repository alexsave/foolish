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
#include <math.h>
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

/* ------------------------------------------------ the seat, resolved */

/* THE OWNER'S CASE (1.0(8), 2026-09-25): Messages rotated this device's
 * participant id, so neither sealed tag is mine any more. Every witness of
 * utm_resolve, one row each, and the records that feed witness (a). */
static void test_resolve(void)
{
    const int32_t seed = 1790284811;
    uint8_t a[UTM_TAG_LEN], b[UTM_TAG_LEN], c[UTM_TAG_LEN], a2[UTM_TAG_LEN], b2[UTM_TAG_LEN];
    tag_of("alex", seed, a);
    tag_of("vera", seed, b);
    tag_of("cleo", seed, c);
    tag_of("alex-rotated", seed, a2);
    tag_of("vera-rotated", seed, b2);

    UtmMsg inv, g;
    utm_open(&inv, seed, a);
    g = inv;
    OK(utm_play(&g, b, 40), "resolve: vera joins");        /* 1 ply: O to move */
    int by = -9;

    /* (a) the record, whatever the tag says */
    OK(utm_resolve(&g, UTM_SEAT_O, UTM_SEAT_SPECTATOR, 0, UTM_SENT_UNKNOWN, &by) == UTM_SEAT_O
       && by == UTM_BY_RECORD, "resolve: an O record seats a rotated creator in a group");
    OK(utm_resolve(&g, UTM_SEAT_X, UTM_SEAT_SPECTATOR, 0, UTM_SENT_UNKNOWN, &by) == UTM_SEAT_X
       && by == UTM_BY_RECORD, "resolve: an X record seats a rotated joiner");
    OK(utm_resolve(&g, UTM_SEAT_O, UTM_SEAT_SPECTATOR, 1, 1, &by) == UTM_SEAT_O
       && by == UTM_BY_RECORD, "resolve: the record outranks the sender");
    OK(utm_resolve(&inv, UTM_SEAT_O, UTM_SEAT_OPEN, 0, UTM_SENT_UNKNOWN, &by) == UTM_SEAT_WAITING
       && by == UTM_BY_RECORD, "resolve: an O record on my own invitation is waiting");
    OK(utm_resolve(&inv, UTM_SEAT_X, UTM_SEAT_OPEN, 0, UTM_SENT_UNKNOWN, &by) == UTM_SEAT_OPEN
       && by == UTM_BY_NONE, "resolve: an X record cannot seat anybody on an open invitation");

    /* (b) the tag, while the id stands */
    OK(utm_resolve(&g, 0, utm_seat(&g, a), 1, 0, &by) == UTM_SEAT_O && by == UTM_BY_TAG,
       "resolve: the tag seats O, and outranks the sender");
    OK(utm_resolve(&g, 0, utm_seat(&g, b), 0, UTM_SENT_UNKNOWN, &by) == UTM_SEAT_X && by == UTM_BY_TAG,
       "resolve: the tag seats X");
    OK(utm_resolve(&inv, 0, utm_seat(&inv, a), 0, UTM_SENT_UNKNOWN, &by) == UTM_SEAT_WAITING
       && by == UTM_BY_TAG, "resolve: the tag says my invitation is waiting");

    /* (c) the sender, in a DM, with both tags unknown */
    OK(utm_seat(&g, a2) == UTM_SEAT_SPECTATOR && utm_seat(&g, b2) == UTM_SEAT_SPECTATOR,
       "resolve: after the rotation neither tag is either of us");
    OK(g.game.turn == UTTT_O && utm_resolve(&g, 0, utm_seat(&g, a2), 1, 0, &by) == UTM_SEAT_O
       && by == UTM_BY_SENDER,
       "resolve: THE OWNER'S CASE - a DM, the bubble from the other side, O to move: I am O");
    OK(utm_resolve(&g, 0, utm_seat(&g, b2), 1, 1, &by) == UTM_SEAT_X && by == UTM_BY_SENDER,
       "resolve: a DM, my own bubble, X moved last: I am X");
    UtmMsg g2 = g;
    OK(utm_play(&g2, a, 36), "resolve: alex answers");     /* 2 plies: X to move */
    OK(utm_resolve(&g2, 0, UTM_SEAT_SPECTATOR, 1, 1, &by) == UTM_SEAT_O && by == UTM_BY_SENDER,
       "resolve: my own bubble, O moved last: I am O");
    OK(utm_resolve(&g2, 0, UTM_SEAT_SPECTATOR, 1, 0, &by) == UTM_SEAT_X && by == UTM_BY_SENDER,
       "resolve: their bubble, O moved last: I am X");
    OK(utm_resolve(&inv, 0, utm_seat(&inv, a2), 1, 1, &by) == UTM_SEAT_WAITING && by == UTM_BY_SENDER,
       "resolve: my own invitation, sent by me, is waiting");
    OK(utm_resolve(&inv, 0, utm_seat(&inv, b2), 1, 0, &by) == UTM_SEAT_OPEN && by == UTM_BY_SENDER,
       "resolve: their invitation in a DM is mine to take");

    /* (d) nothing */
    OK(utm_resolve(&g, 0, utm_seat(&g, c), 0, 0, &by) == UTM_SEAT_SPECTATOR && by == UTM_BY_NONE,
       "resolve: a group chat never infers - cleo watches");
    OK(utm_resolve(&g, 0, UTM_SEAT_SPECTATOR, 0, 1, &by) == UTM_SEAT_SPECTATOR && by == UTM_BY_NONE,
       "resolve: not even from my own bubble in a group");
    OK(utm_resolve(&g, 0, UTM_SEAT_SPECTATOR, 1, UTM_SENT_UNKNOWN, &by) == UTM_SEAT_SPECTATOR
       && by == UTM_BY_NONE, "resolve: a DM with no sender to read watches");
    OK(utm_resolve(&inv, 0, UTM_SEAT_OPEN, 0, UTM_SENT_UNKNOWN, &by) == UTM_SEAT_OPEN
       && by == UTM_BY_NONE, "resolve: nobody's witness on an invitation leaves it open");

    /* THE RECORDS */
    static uint8_t r[UTM_REC_BYTES];
    int n = 0;
    OK(utm_rec_find(r, n, &g) == 0, "rec: empty finds nothing");
    n = utm_rec_put(r, n, &inv, UTM_SEAT_WAITING);
    OK(n == UTM_REC_LEN && utm_rec_find(r, n, &inv) == UTM_SEAT_O && utm_rec_find(r, n, &g) == UTM_SEAT_O,
       "rec: the creator's record is the game's, before and after the join");
    OK(utm_rec_put(r, n, &inv, UTM_SEAT_X) == n, "rec: no X record on an open invitation");
    OK(utm_rec_put(r, n, &g, UTM_SEAT_SPECTATOR) == n, "rec: watching is not recorded");
    UtmMsg other = inv;
    other.seed++;
    OK(utm_rec_find(r, n, &other) == 0, "rec: another game finds nothing");

    /* two joiners in a group: vera's X record is her fork's alone */
    UtmMsg gc = inv;
    OK(utm_play(&gc, c, 41), "rec: cleo joins the same invitation");
    static uint8_t rv[UTM_REC_BYTES];
    int nv = utm_rec_put(rv, 0, &g, UTM_SEAT_X);
    OK(utm_rec_find(rv, nv, &g) == UTM_SEAT_X && utm_rec_find(rv, nv, &g2) == UTM_SEAT_X,
       "rec: the joiner's record holds for every later bubble of her fork");
    OK(utm_rec_find(rv, nv, &gc) == 0, "rec: and seats nobody in the other fork");
    OK(utm_rec_find(rv, nv, &inv) == 0, "rec: nor on the invitation");

    /* one record per game: a new seat replaces the old, and forget drops it */
    n = utm_rec_put(r, n, &g, UTM_SEAT_X);
    OK(n == UTM_REC_LEN && utm_rec_find(r, n, &g) == UTM_SEAT_X, "rec: a new seat replaces the old");
    n = utm_rec_forget(r, n, &g);
    OK(n == 0 && utm_rec_find(r, n, &g) == 0, "rec: forgotten");

    /* bounded, newest first; a ragged tail is ignored */
    UtmMsg k = inv;
    for (int i = 0; i < UTM_REC_MAX + 5; i++) {
        k.seed = seed + i;
        n = utm_rec_put(r, n, &k, UTM_SEAT_O);
    }
    k.seed = seed + UTM_REC_MAX + 4;
    OK(n == UTM_REC_BYTES && utm_rec_find(r, n, &k) == UTM_SEAT_O, "rec: full, and the newest is there");
    k.seed = seed + 5;
    OK(utm_rec_find(r, n, &k) == UTM_SEAT_O, "rec: the oldest kept is there");
    k.seed = seed + 4;
    OK(utm_rec_find(r, n, &k) == 0, "rec: past the bound the oldest fell off");
    k.seed = seed + UTM_REC_MAX + 4;
    OK(utm_rec_find(r, UTM_REC_LEN + 3, &k) == UTM_SEAT_O && utm_rec_find(r, 3, &k) == 0,
       "rec: a ragged tail is ignored");
    k.seed = seed + 5;
    n = utm_rec_put(r, n, &k, UTM_SEAT_O);
    OK(n == UTM_REC_BYTES && r[UTM_REC_LEN - 1] == UTM_SEAT_O && utm_rec_find(r, UTM_REC_LEN, &k) == UTM_SEAT_O,
       "rec: re-recording a game moves it to the front, not a second copy");

    /* THE KEY IS FROZEN: a changed salt orphans every record on every phone. */
    static const uint8_t golden[UTM_REC_LEN - 1] = { 0x50, 0x04, 0xa4, 0x97, 0xe2, 0xef, 0xae, 0x39 };
    n = utm_rec_put(r, 0, &inv, UTM_SEAT_O);
    OK(!memcmp(r, golden, UTM_REC_LEN - 1), "rec: the key format is frozen");
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

    /* A CHANGE OF MIND (utm_can_replace): a different square that is legal
     * where my draft was played; anything else is a tap that does nothing. */
    {
        int x = j.game.move[0];
        UtmMsg before = j;
        OK(!utm_can_replace(&j, b, x), "replace: the same square is not a change of mind");
        OK(utm_can_replace(&j, b, (x + 1) % 81), "replace: another square of the joining move");
        OK(!utm_can_replace(&j, a, (x + 1) % 81) && !utm_can_replace(&j, c, (x + 1) % 81),
           "replace: only my own draft");
        OK(!utm_can_replace(&j, b, -1) && !utm_can_replace(&j, b, 81), "replace: off the board");
        OK(!memcmp(&before, &j, sizeof j), "replace: asking changes nothing");
        UtmMsg o = j;
        OK(utm_play(&o, a, 36), "replace: O's draft");
        uint8_t lg[81]; int nl = uttt_legal(&j.game, lg), every = 1;
        for (int mv = 0; mv < 81; mv++) {
            int legal = 0;
            for (int k = 0; k < nl; k++) legal |= lg[k] == mv;
            if (utm_can_replace(&o, a, mv) != (legal && mv != 36)) every = 0;
        }
        OK(every, "replace: exactly the legal squares of the draft's position, less the draft's own");
        OK(!utm_can_replace(&inv, a, 40) && !utm_can_replace(&inv, b, 40), "replace: nothing on an invitation");
    }

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

    /* the rectangle VoiceOver puts an element on is the one hit reads back */
    int round = 1;
    for (int mv = 0; mv < 81; mv++) {
        float r[4];
        if (!uttt_cell_rect(mv, r) || r[2] <= 0 || r[3] <= 0 ||
            uttt_hit(r[0] + r[2] * .5f, r[1] + r[3] * .5f) != mv ||
            uttt_hit(r[0] + r[2] * .05f, r[1] + r[3] * .95f) != mv) round = 0;
    }
    float r[4];
    OK(round, "hit: every square's rectangle hits that square, centre and corner");
    OK(!uttt_cell_rect(-1, r) && !uttt_cell_rect(81, r), "hit: no rectangle off the board");
}

/* ---------------------------------------------------------- the words */
static int has_em_dash(const char *s) { return strstr(s, "\xe2\x80\x94") != 0; }

static void say(int key, const UtttGame *g, int seat, char *out)
{
    int n = uttt_say(key, g, seat, out, 160);
    if (n < 0) out[0] = 0;
}

/* EVERY CAPTION THE TABLE CAN PRODUCE IS ONE LINE (owner): every side to
 * play into every block, every won line by either side, a draw, at every
 * length a game can have, unnamed as the bubble sends it - at most
 * UTTT_CAPTION_MAX characters, measured off the transcript (uttt_say.h). */
static void test_caption_one_line(void)
{
    char s[160];
    int longest = 0, n = 0, lined = 0;
    char worst[160] = "";
    for (int plies = 0; plies <= 81; plies++)
        for (int over = 0; over <= 3; over++)
            for (int turn = UTTT_X; turn <= UTTT_O; turn++)
                for (int block = -1; block <= 9; block++)
                    for (int line = -1; line < 8; line++) {
                        int k = uttt_caption(over, turn, block, line, plies, NULL, s, sizeof s);
                        OK(k >= 0 && k == (int)strlen(s), "caption: every combination answers");
                        if (k > longest) { longest = k; strcpy(worst, s); }
                        if (strstr(s, " the ") && strstr(s, " won ")) lined++;
                        if (s[k ? k - 1 : 0] == '.') n++;
                    }
    printf("  caption: longest %d characters, \"%s\"\n", longest, worst);
    OK(longest <= UTTT_CAPTION_MAX, "caption: every caption fits one line of the transcript");
    OK(n == 0, "caption: no caption ends in a full stop");
    OK(lined > 0, "caption: a win still names its line where the line fits");
    uttt_caption(UTTT_X, UTTT_O, 0, 3, 21, NULL, s, sizeof s);
    OK(!strcmp(s, "X won down the left in 21 moves"), "caption: the owner's example");
    uttt_caption(0, UTTT_X, 9, -1, 4, NULL, s, sizeof s);
    OK(!strcmp(s, "X to play"), "caption: no suffix when free");
    uttt_caption(0, UTTT_O, 0, -1, 4, NULL, s, sizeof s);
    OK(!strcmp(s, "O to play"), "caption: no board suffix (the tint shows it)");
    uttt_caption(0, UTTT_X, -1, -1, 0, NULL, s, sizeof s);
    OK(!strcmp(s, "New game?"), "caption: the invitation");
}

static void test_say(void)
{
    char s[160];
    UtttGame g;
    uttt_init(&g);
    say(UTTT_SAY_BUBBLE_HEADLINE, &g, UTM_SEAT_WAITING, s);
    OK(!strcmp(s, ""), "say: an invitation's bubble has no words, its caption asks");
    say(UTTT_SAY_CAPTION, &g, UTM_SEAT_WAITING, s);
    OK(!strcmp(s, "New game?"), "say: the invitation's caption, nobody named");
    OK(uttt_say_by(UTTT_SAY_CAPTION, &g, UTM_SEAT_WAITING, "$ALEX", s, sizeof s) > 0
       && !strcmp(s, "$ALEX wants a game. Tap to take it"),
       "say: the invitation's caption names its sender (UI.html 01)");
    OK(uttt_say_bubble_mark(&g) == 0, "say: an invitation's bubble draws no mark");
    say(UTTT_SAY_BUBBLE_PLACE, &g, UTM_SEAT_WAITING, s);
    OK(!strcmp(s, ""), "say: an invitation has no place line");
    say(UTTT_SAY_HEADLINE_PRE, &g, UTM_SEAT_OPEN, s);
    OK(!strcmp(s, "Your move"), "say: the joiner is on move");
    say(UTTT_SAY_SUBLINE, &g, UTM_SEAT_OPEN, s);
    OK(!strcmp(s, ""), "say: a live game has no line under the headline, the tint says where");

    uttt_play(&g, 41);                  /* centre block, middle-right cell -> block 5 */
    say(UTTT_SAY_CAPTION, &g, UTM_SEAT_X, s);
    OK(!strcmp(s, "O to play"), "say: the caption says whose turn (owner)");
    say(UTTT_SAY_BUBBLE_PLACE, &g, UTM_SEAT_X, s);
    OK(!strcmp(s, ""), "say: a move's bubble has no place line, its caption names it");
    say(UTTT_SAY_BUBBLE_HEADLINE, &g, UTM_SEAT_X, s);
    OK(!strcmp(s, "") && uttt_say_bubble_mark(&g) == 0,
       "say: a move's bubble has no headline and draws no mark");
    OK(uttt_say_by(UTTT_SAY_CAPTION, &g, UTM_SEAT_X, "$ALEX", s, sizeof s) > 0
       && !strcmp(s, "O to play"),
       "say: a move's caption names nobody");
    say(UTTT_SAY_HEADLINE_PRE, &g, UTM_SEAT_X, s);
    OK(!strcmp(s, "Waiting on ") && uttt_say_headline_mark(&g, UTM_SEAT_X) == UTTT_O,
       "say: X waits on a drawn O");
    say(UTTT_SAY_SUBLINE, &g, UTM_SEAT_X, s);
    OK(!strcmp(s, ""), "say: waiting on them has no line under it (owner)");
    say(UTTT_SAY_HEADLINE_PRE, &g, UTM_SEAT_O, s);
    OK(!strcmp(s, "Your move") && uttt_say_headline_mark(&g, UTM_SEAT_O) == 0,
       "say: O is on move, words only");
    say(UTTT_SAY_WATCH_LINE, &g, UTM_SEAT_SPECTATOR, s);
    OK(!strcmp(s, " to play") && uttt_say_watch_mark(&g) == UTTT_O,
       "say: the spectator's line follows a drawn O, never a typed one");
    say(UTTT_SAY_WATCH_SPOKEN, &g, UTM_SEAT_SPECTATOR, s);
    OK(!strcmp(s, "O to play"), "say: VoiceOver hears the spectator's mark spelled");

    say(UTTT_SAY_DOOR_AGAIN, &g, UTM_SEAT_X, s);
    OK(!strcmp(s, "Again"), "say: the end door (UI.html 06)");
    say(UTTT_SAY_HEADLINE_SPOKEN, &g, UTM_SEAT_X, s);
    OK(!strcmp(s, "Waiting on O"), "say: VoiceOver hears the drawn mark spelled");
    say(UTTT_SAY_YOU_ARE_SPOKEN, &g, UTM_SEAT_O, s);
    OK(!strcmp(s, "You are O"), "say: VoiceOver hears which side you are");
    say(UTTT_SAY_YOU_ARE_SPOKEN, &g, UTM_SEAT_SPECTATOR, s);
    OK(!strcmp(s, ""), "say: a spectator is no side");
    OK(uttt_say_cell(&g, 4 * 9 + 4, s, sizeof s) > 0 && !strcmp(s, "Centre board, centre square, empty"),
       "say: an empty square by block and cell");
    {
        int mv = g.move[g.n_plies - 1], ok = uttt_say_cell(&g, mv, s, sizeof s) > 0;
        char want[80];
        snprintf(want, sizeof want, "%s board, %s square, X",
                 uttt_place_name(mv / 9), uttt_place_name(mv % 9));
        want[0] = (char)(want[0] - 'a' + 'A');
        OK(ok && !strcmp(s, want), "say: a marked square names its mark");
    }
    OK(uttt_say_cell(&g, 0, s, sizeof s) > 0 && !strncmp(s, "Top left board, top left square, ", 33),
       "say: the first square");
    OK(uttt_say_cell(&g, 81, s, sizeof s) == -1 && uttt_say_cell(&g, 0, s, 8) == -1,
       "say: a square off the board or a short buffer is refused");

    /* THE END, docs/UI.html 05-07: X on the top-left to bottom-right diagonal */
    static const uint8_t diag[] = { 79, 63, 5, 45, 8, 76, 42, 61, 70, 71, 78, 55, 15, 58, 36, 1, 11, 24, 4, 40, 39, 31, 80, 35, 0 };
    uttt_init(&g);
    for (unsigned i = 0; i < sizeof diag && !g.over; i++) uttt_play(&g, diag[i]);
    OK(g.over == UTTT_X && uttt_won_line(&g) == 6, "say: the end fixture is X on the diagonal");
    say(UTTT_SAY_SUBLINE, &g, UTM_SEAT_O, s);
    OK(!strcmp(s, "Diagonal"), "say: the end subline names the line");
    say(UTTT_SAY_HEADLINE_SPOKEN, &g, UTM_SEAT_O, s);
    OK(!strcmp(s, "X wins"), "say: the loser hears the winner's mark spelled");
    say(UTTT_SAY_HEADLINE_SPOKEN, &g, UTM_SEAT_X, s);
    OK(!strcmp(s, "You win"), "say: the winner hears it plainly");
    say(UTTT_SAY_CAPTION, &g, UTM_SEAT_O, s);
    {
        char want[64];
        snprintf(want, sizeof want, "X won on the diagonal in %d moves", g.n_plies);
        if (strlen(want) > UTTT_CAPTION_MAX) snprintf(want, sizeof want, "X won in %d moves", g.n_plies);
        OK(!strcmp(s, want), "say: the end caption names the line and the length, the line where it fits");
        snprintf(want, sizeof want, "$ALEX won on the diagonal in %d moves", g.n_plies);
        if (strlen(want) > UTTT_CAPTION_MAX) snprintf(want, sizeof want, "$ALEX won in %d moves", g.n_plies);
        OK(uttt_say_by(UTTT_SAY_CAPTION, &g, UTM_SEAT_X, "$ALEX", s, sizeof s) > 0
           && !strcmp(s, want), "say: the end caption names the winner, who sent it (UI.html 05)");
    }
    say(UTTT_SAY_BUBBLE_HEADLINE, &g, UTM_SEAT_O, s);
    OK(!strcmp(s, "wins") && uttt_say_bubble_mark(&g) == UTTT_X,
       "say: the finished bubble draws the winner's mark");
    OK(uttt_say_headline_ink(&g) == 0x25376bffu && uttt_bubble(&g).place_rgba == 0x25376bffu,
       "say: X's win is set in X blue, drawer and bubble");

    /* O WINS (docs/STORE_SHOTS.md's store game): its verdict is O red */
    {
        static const uint8_t ow[] = { 34,67,44,80,76,43,69,62,79,63,4,40,39,31,37,16,70,71,73,15,60,59,47,24,57,
                                      30,33,56,23,46,11,21,32,48,29,20,18,3,35,38,19,10,17,42,14,50,45,5,49,22 };
        UtttGame w; uttt_init(&w);
        for (unsigned i = 0; i < sizeof ow; i++) uttt_play(&w, ow[i]);
        OK(w.over == UTTT_O, "say: the O fixture is O's win");
        OK(uttt_say_headline_ink(&w) == 0xa8321fffu, "say: O's \"You win\" is set in O red");
        OK(uttt_bubble(&w).place_rgba == 0xa8321fffu, "say: O's bubble counts the moves in O red");
        uttt_init(&w);
        OK(uttt_say_headline_ink(&w) == UTTT_INK, "say: a live headline is the page's ink");
    }

    /* play games out and read every key at every ply from every seat */
    int dashes = 0, missing = 0, lines_said = 0, periods = 0;
    for (int game = 0; game < 200; game++) {
        uttt_init(&g);
        for (;;) {
            for (int seat = 0; seat <= UTM_SEAT_OPEN; seat++)
                for (int k = 0; k < UTTT_SAY_COUNT; k++) {
                    int n = uttt_say(k, &g, seat, s, sizeof s);
                    if (n < 0) missing++;
                    else if (has_em_dash(s)) dashes++;
                    else if (n > 0 && s[n - 1] == '.') periods++;
                }
            if (g.over) break;
            uint8_t list[81];
            int n = uttt_legal(&g, list);
            uttt_play(&g, list[rnd() % (unsigned)n]);
        }
        if ((g.over == UTTT_X || g.over == UTTT_O)) {
            /* the subline names the line the winner holds, by its shape */
            static const char *const shape[8] = {
                "Top row", "Middle row", "Bottom row",
                "Left column", "Middle column", "Right column",
                "Diagonal", "Diagonal" };
            int li = uttt_won_line(&g);
            unsigned m = li < 0 ? 0 : uttt_line_mask(li);
            unsigned held = g.bm[g.over - 1];
            say(UTTT_SAY_SUBLINE, &g, UTM_SEAT_X, s);
            OK(li >= 0 && (held & m) == m && !strcmp(s, shape[li]),
               "say: the end subline names the winning line");
            /* rows are said "across", columns "down": subline and caption agree */
            char cap[160];
            say(UTTT_SAY_CAPTION, &g, UTM_SEAT_X, cap);
            OK((li < 3 && strstr(cap, " across the ")) || (li >= 3 && li < 6 && strstr(cap, " down the "))
               || (li >= 6 && strstr(cap, " on the diagonal ")) || !strstr(cap, " the "),
               "say: the caption and the subline name the same line, or the caption none");
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
    OK(periods == 0, "say: no sentence ends in a period (owner)");
    OK(uttt_say(UTTT_SAY_COUNT, &g, 0, s, sizeof s) == -1, "say: an unknown key is refused");
    OK(uttt_say(UTTT_SAY_UNREADABLE_SUBLINE, &g, 0, s, 8) == -1, "say: a short buffer is refused");
}

/* GETTING A BUBBLE INTO THE FIELD: the words and the send hint's fuse. When
 * an insert may go and what its silence means are shared/c/msg_stage's, and
 * tested there (msg_stage_test.c, run by `make run`). */
static void test_insert(void)
{
    OK(UTM_SEND_HINT_MS == 3000, "insert: the send hint waits three seconds, as the sister app's");

    UtttGame g;
    char s[128];
    uttt_init(&g);
    OK(uttt_say(UTTT_SAY_SEND_HINT, &g, UTM_SEAT_WAITING, s, sizeof s) > 0 && !strcmp(s, "Send"),
       "say: the send hint's caption");
    OK(uttt_say(UTTT_SAY_DOOR_SEND, &g, UTM_SEAT_X, s, sizeof s) > 0 && !strcmp(s, "Send a board"),
       "say: the door when no insert was answered");
    OK(uttt_say(UTTT_SAY_DOOR_COPY, &g, UTM_SEAT_X, s, sizeof s) > 0 && !strcmp(s, "Copy code"),
       "say: the end screen's replay door");
    OK(uttt_say(UTTT_SAY_DOOR_COPIED, &g, UTM_SEAT_X, s, sizeof s) > 0 && !strcmp(s, "Copied"),
       "say: and its receipt");
}

/* THE BUBBLE'S BAKE SCALE: the sender's own, between 2 and 3. */
static void test_bubble_scale(void)
{
    OK(uttt_bubble_scale(2.f) == 2.f, "bubble scale: a 2x phone bakes at 2");
    OK(uttt_bubble_scale(3.f) == 3.f, "bubble scale: a 3x phone bakes at 3");
    OK(uttt_bubble_scale(2.5f) == 2.5f, "bubble scale: between the two, its own");
    OK(uttt_bubble_scale(1.f) == 2.f && uttt_bubble_scale(0.f) == 2.f,
       "bubble scale: under 2 (or unknown) bakes at 2");
    OK(uttt_bubble_scale(NAN) == 2.f, "bubble scale: NaN bakes at 2");
    OK(uttt_bubble_scale(4.f) == 3.f, "bubble scale: over 3 bakes at 3");
}

/* A GAME IN THE THREAD OUTLIVES THE BUILD THAT WROTE IT. These bytes were
 * written by the 1.0(6) kernel (d2332dd0): alex's invitation, vera's centre
 * move. Every later build must read them and seat alex as O by the same
 * participant bytes - a changed salt, seed or tag length would lock every
 * creator out of every game already in a thread (1.0(8), 2026-09-25: the
 * owner's own game opened as a spectator, and this pin ruled the kernel
 * out: same bytes in, same seat out). */
static void test_tag_pinned(void)
{
    static const uint8_t alex[16] = {0xa1,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    static const uint8_t vera[16] = {0xb2,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    static const char b6[] = "?m=W4AWRU5B4AARYPV2425R7OXV4TGCHFTAD6Y5SYEIPIJMGJQ";
    static const uint8_t o6[UTM_TAG_LEN] = {0x1c,0x3e,0xba,0xe6,0xbb,0x1f,0xba,0xf5,0xe4};
    UtmMsg m;
    uint8_t a[UTM_TAG_LEN], v[UTM_TAG_LEN];
    OK(utm_text_decode(b6, &m) == UTM_EOK, "pinned: a 1.0(6) game still reads");
    utm_tag(m.seed, alex, 16, a);
    utm_tag(m.seed, vera, 16, v);
    OK(!memcmp(a, o6, UTM_TAG_LEN) && !memcmp(m.o, o6, UTM_TAG_LEN),
       "pinned: the creator's tag is the 1.0(6) tag");
    OK(utm_seat(&m, a) == UTM_SEAT_O && utm_seat(&m, v) == UTM_SEAT_X,
       "pinned: its creator is O and its joiner X");
}

int main(int argc, char **argv)
{
    int games = argc > 1 ? atoi(argv[1]) : 10000;
    test_b32();
    test_games(games);
    test_refusals();
    test_seats();
    test_resolve();
    test_tag_pinned();
    test_prefer();
    test_hit();
    test_caption_one_line();
    test_say();
    test_insert();
    test_bubble_scale();
    printf("uttt_msg: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
