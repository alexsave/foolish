/* The big game's message: its bytes, its board check, its seats and its words.
 *
 *     make -C uttt/c run        (./build/uttt_big_msg_test [games]; and asan)
 *
 * A depth-5 random game runs to about 42,000 plies, so the per-ply round trip
 * covers the first PLY_CAP plies of each of `games` games (every ply through
 * bytes, text, the header alone and the board), and ONE game is played to the
 * end with every 50th ply through the same round trip and through the kit's
 * whole picture path without the JPEG (bd_frame, bd_paint, bd_sample,
 * bd_unframe). The JPEG chain is tests/uttt_big_chain.c (macOS, make
 * big-chain).
 *
 * MUTATION CHECKS: each named assertion below was watched go red with the
 * rule it guards broken in a copy of uttt_big_msg.c (or the format-2 encoder,
 * for the golden), and the file restored with cp and proved by cmp:
 *
 *   M1  board check's polynomial 0xEDB88320 -> 0xEDB88321
 *         "crc: the standard check value", "crc: the kit's bd_crc32 on random boards"
 *   M2  encode writes n_plies one high
 *         "wire: every ply round-trips (prefix)", "picture: every 50th ply ..."
 *   M3  peek accepts format 2 as well as 3
 *         "peek: format 2 is refused as a format"
 *   M4  peek drops the reserved-flag test
 *         "decode: a reserved flag is refused"
 *   M5  the wire check skips the last header byte (body - 1)
 *         "decode: every flipped bit is refused", "peek: ... by the header alone"
 *   M6  decode skips the board CRC comparison
 *         "decode: one changed cell is refused (the board check)"
 *   M7  decode skips the n_plies-against-marks comparison
 *         "decode: a ply count the marks do not give is refused"
 *   M8  head_ok drops the X == O rule
 *         "decode: X and O the same person"
 *   M9  head_ok drops the zero-seed rule
 *         "decode: a zero seed"
 *   M10 peek accepts a trailing byte (n > want not refused)
 *         "decode: a trailing byte is refused"
 *   M11 play does not seal on the joining move
 *         "seat: taking the seat is the first move"
 *   M12 undo of the joining move keeps the seal
 *         "undo: taking back the joining move unseals"
 *   M13 can_replace allows the same square
 *         "replace: the same square is not a change of mind"
 *   M14 prefer: one roster equal plies returns tapped
 *         "prefer: one roster, equal plies, a change of mind: mine"
 *   M15 join key salt "uttt.join.big|" -> "uttt.join.1|"
 *         "prefer: two joiners by the join key"
 *   M16 caption passes block -1 in play
 *         "caption: in play"
 *   M17 roster adapter n_plies = parity dropped (always 0)
 *         "roster: the sender witness reads the parity"
 *   M18 door ignores over
 *         "door: Again once the game is over"
 *   M19 text_is accepts format 2
 *         "text_is: a format-2 text is not big"
 *   M20 again does not copy the look
 *         "again: on the same napkin"
 *   M21 uttt_msg.c's encoder writes the look byte flipped (look ^ 1)
 *         "golden: the 9 x 9 bytes are the shipped format"
 *   M22 utb_head_encode writes a header head_ok refuses
 *         "head encode: refuses what peek refuses"
 *   M23 decode carries on with an empty board when adopt refuses
 *         "decode: an invitation's link over a board with marks is refused by adopt"
 *   (M6 first went unnoticed: the changed cell also broke the counts, so
 *   adopt refused it too; the test now moves a mark, a board adopt accepts,
 *   which only the board check refuses. M11 first crashed the test on a
 *   negative length rather than failing an assertion; it now stops cleanly.)
 *
 * The bridge's rules are mutation-checked through ios/uttt_api_smoke.c's
 * big-game section (B1-B9 in the commit that adds it).
 */
#include "../src/uttt_big_msg.h"
#include "../src/uttt_say.h"
#include "../../../shared/c/b32.h"
#include "../../../shared/c/sha256.h"
#include "../../../shared/c/wire_check/wire_check.h"
#include "../../../shared/swift/BubbleDataKit/Sources/CBubbleData/include/bubble_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails, checks;
#define OK(c, what) do { checks++; if (!(c)) { fails++; \
    printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, what); } } while (0)

static uint64_t rs = 0x2545f4914f6cdd1dull;
static uint32_t rnd(void)
{
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (uint32_t)(rs >> 11);
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void tag_of(const char *who, int32_t seed, uint8_t t[UTM_TAG_LEN])
{
    utm_tag(seed, (const uint8_t *)who, (int)strlen(who), t);
}

/* Big messages are 66 KB each: the tests keep theirs in static storage. */
static UtbMsg M, BACK, T1, T2, T3;
static int32_t LEGAL[UTB_CELLS];

static int same_msg(const UtbMsg *a, const UtbMsg *b)
{
    if (a->seed != b->seed || a->look != b->look || a->sealed != b->sealed) return 0;
    if (memcmp(a->o, b->o, UTM_TAG_LEN) || memcmp(a->x, b->x, UTM_TAG_LEN)) return 0;
    const UtbGame *g = &a->game, *h = &b->game;
    if (g->depth != h->depth || g->turn != h->turn || g->over != h->over) return 0;
    if (g->last != h->last || g->n_plies != h->n_plies) return 0;
    if (memcmp(g->cell, h->cell, UTB_CELLS)) return 0;
    if (memcmp(g->node, h->node, (size_t)utb_nodes(UTB_DEPTH))) return 0;
    return utb_region(g) == utb_region(h);
}

static int same_head(const UtbHead *h, const UtbMsg *m)
{
    return h->seed == m->seed && h->look == m->look && h->sealed == m->sealed
        && !memcmp(h->o, m->o, UTM_TAG_LEN) && (!m->sealed || !memcmp(h->x, m->x, UTM_TAG_LEN))
        && h->n_plies == m->game.n_plies && h->last == m->game.last
        && h->board_check == utb_board_check(m->game.cell, UTB_CELLS);
}

/* One random legal move for whoever is on move. */
static int random_move(const UtbGame *g)
{
    int n = utb_legal(g, LEGAL, UTB_CELLS);
    return n > 0 ? LEGAL[rnd() % (unsigned)n] : -1;
}

/* encode -> text -> peek -> decode with the sender's cells, and back. */
static int round_trip(const UtbMsg *m)
{
    char t[UTB_MAX_TEXT];
    UtbHead h;
    if (utb_msg_text_encode(m, t, sizeof t) <= 0) return 0;
    if (utb_msg_text_peek(t, &h) != UTM_EOK || !same_head(&h, m)) return 0;
    if (!utb_msg_text_is(t)) return 0;
    if (utb_msg_text_decode(t, m->game.cell, &BACK) != UTM_EOK) return 0;
    return same_msg(m, &BACK);
}

/* --------------------------------------------- the kit's picture, no JPEG */

static uint8_t FRAMED[UTB_SIDE * (UTB_SIDE + 1)], READ[UTB_SIDE * (UTB_SIDE + 1)], SYM[UTB_CELLS];
static uint8_t RGBA[UTB_SIDE * (UTB_SIDE + 1) * 4];

static int picture_trip(const UtbMsg *m)
{
    char t[UTB_MAX_TEXT];
    BdReading rd;
    if (utb_msg_text_encode(m, t, sizeof t) <= 0) return 0;
    if (bd_frame(m->game.cell, UTB_CELLS, BD_KIND_SYMBOLS, 0, UTB_SIDE, FRAMED) != BD_EOK) return 0;
    if (bd_paint(FRAMED, UTB_SIDE, 1, RGBA) != BD_EOK) return 0;
    if (bd_sample(RGBA, UTB_SIDE, UTB_SIDE + 1, UTB_SIDE, READ, &rd) != BD_EOK) return 0;
    if (bd_unframe(READ, UTB_SIDE, SYM, UTB_CELLS, NULL, NULL) != UTB_CELLS) return 0;
    if (utb_msg_text_decode(t, SYM, &BACK) != UTM_EOK) return 0;
    return same_msg(m, &BACK);
}

/* --------------------------------------------------------- 1 and 8 */

#define PLY_CAP 1000

/* THE FINISHED GAME, kept for the door and the caption tests. */
static UtbMsg FIN;
static uint8_t FIN_A[UTM_TAG_LEN], FIN_B[UTM_TAG_LEN];

static void test_games(int games)
{
    double t0 = now();
    long plies = 0;
    int bad = 0, refused = 0;
    for (int gi = 0; gi < games; gi++) {
        int32_t seed = (int32_t)(rnd() | 1);
        uint8_t a[UTM_TAG_LEN], b[UTM_TAG_LEN];
        tag_of("creator", seed, a);
        tag_of("joiner", seed, b);
        utb_msg_open(&M, seed, (uint8_t)rnd(), a);
        if (!round_trip(&M)) bad++;
        for (int p = 0; p < PLY_CAP && !M.game.over; p++) {
            const uint8_t *who = M.game.n_plies % 2 == 0 ? b : a;
            const uint8_t *other = who == b ? a : b;
            int mv = random_move(&M.game);
            if (utb_msg_play(&M, other, mv)) refused++;
            if (!utb_msg_play(&M, who, mv)) { bad++; break; }
            if (!round_trip(&M)) bad++;
            plies++;
        }
    }
    double t1 = now();
    OK(bad == 0, "wire: every ply round-trips (prefix)");
    OK(refused == 0, "play: out of turn is refused");
    printf("  wire: %d games, first %d plies each, %ld plies through bytes, text, header and board"
           " in %.2f s (%.0f us a ply)\n", games, PLY_CAP, plies, t1 - t0, 1e6 * (t1 - t0) / (double)(plies ? plies : 1));

    /* ONE GAME TO THE END, every 50th ply through the wire and the picture */
    int32_t seed = 1790000001;
    tag_of("creator", seed, FIN_A);
    tag_of("joiner", seed, FIN_B);
    utb_msg_open(&FIN, seed, 42, FIN_A);
    int wire_bad = 0, pic_bad = 0, checked = 0;
    while (!FIN.game.over) {
        const uint8_t *who = FIN.game.n_plies % 2 == 0 ? FIN_B : FIN_A;
        if (!utb_msg_play(&FIN, who, random_move(&FIN.game))) { wire_bad++; break; }
        if (FIN.game.n_plies % 50 == 0 || FIN.game.over) {
            checked++;
            if (!round_trip(&FIN)) wire_bad++;
            if (!picture_trip(&FIN)) pic_bad++;
        }
    }
    double t2 = now();
    OK(wire_bad == 0, "wire: every 50th ply of a whole game round-trips");
    OK(pic_bad == 0, "picture: every 50th ply through frame, paint, sample, unframe and decode");
    OK(FIN.game.over != 0, "a random game ends");
    printf("  whole game: %d plies, over %d, %d positions through wire and picture in %.2f s\n",
           FIN.game.n_plies, FIN.game.over, checked, t2 - t1);
}

/* ------------------------------------------------------------ 2 refusals */

/* Bytes with a correct check over whatever header the test wants: the only
 * way to reach decode's roster and board rules, since encode refuses them. */
static int forge(uint8_t *out, int fmt, int32_t seed, uint8_t look, int flags,
                 const uint8_t *o, const uint8_t *x, int n_plies, int last, uint32_t crc)
{
    int n = 0;
    out[n++] = UTM_MAGIC; out[n++] = (uint8_t)fmt;
    uint32_t u = (uint32_t)seed;
    out[n++] = (uint8_t)(u >> 24); out[n++] = (uint8_t)(u >> 16);
    out[n++] = (uint8_t)(u >> 8);  out[n++] = (uint8_t)u;
    out[n++] = (uint8_t)flags;
    out[n++] = look;
    memcpy(out + n, o, UTM_TAG_LEN); n += UTM_TAG_LEN;
    if (flags & UTM_FLAG_SEALED) { memcpy(out + n, x, UTM_TAG_LEN); n += UTM_TAG_LEN; }
    out[n++] = (uint8_t)(n_plies >> 8); out[n++] = (uint8_t)n_plies;
    unsigned l = last < 0 ? UTB_LAST_NONE : (unsigned)last;
    out[n++] = (uint8_t)(l >> 8); out[n++] = (uint8_t)l;
    out[n++] = (uint8_t)(crc >> 24); out[n++] = (uint8_t)(crc >> 16);
    out[n++] = (uint8_t)(crc >> 8);  out[n++] = (uint8_t)crc;
    uint8_t d[32];
    Sha256 c;
    sha256_init(&c);
    sha256_update(&c, out, (size_t)n);
    sha256_final(&c, d);
    memcpy(out + n, d, UTM_CHECK_LEN);
    return n + UTM_CHECK_LEN;
}

static uint8_t CELLS[UTB_CELLS];

static void test_refusals(void)
{
    const int32_t seed = 1234;
    uint8_t a[UTM_TAG_LEN], b[UTM_TAG_LEN];
    tag_of("creator", seed, a);
    tag_of("joiner", seed, b);
    utb_msg_open(&M, seed, 88, a);
    OK(utb_msg_play(&M, b, 30000) && utb_msg_play(&M, a, random_move(&M.game))
       && utb_msg_play(&M, b, random_move(&M.game)), "refusal fixture plays");
    const uint8_t *cells = M.game.cell;
    uint32_t crc = utb_board_check(cells, UTB_CELLS);
    uint8_t buf[UTB_MAX_BYTES + 4], t[UTB_MAX_BYTES + 4];
    int n = utb_msg_encode(&M, buf, sizeof buf);
    OK(n == UTB_MAX_BYTES && buf[0] == UTM_MAGIC && buf[1] == UTB_FORMAT && buf[7] == 88,
       "encode: format 3 under the magic, the look at byte 7, the sealed length");
    if (n <= 0) return;                       /* the rest reads these bytes */
    OK(buf[26] == 0 && buf[27] == 3 && ((buf[28] << 8) | buf[29]) == M.game.last
       && (uint32_t)((buf[30] << 24) | (buf[31] << 16) | (buf[32] << 8) | buf[33]) == crc,
       "encode: the tail is n_plies, last and the board check, big-endian");
    OK(utb_msg_decode(buf, n, cells, &BACK) == UTM_EOK && same_msg(&M, &BACK), "decode: the fixture reads");
    {
        UtbHead hh;
        uint8_t hb[UTB_MAX_BYTES];
        OK(utb_msg_peek(buf, n, &hh) == UTM_EOK && utb_head_encode(&hh, hb, sizeof hb) == n && !memcmp(hb, buf, (size_t)n),
           "head encode: a header's bytes are the message's");
        hh.sealed = 0;
        OK(utb_head_encode(&hh, hb, sizeof hb) == UTM_EROSTER, "head encode: refuses what peek refuses");
    }

    OK(utb_msg_decode(buf, 1, cells, &BACK) == UTM_ESHORT, "decode: one byte is short");
    memcpy(t, buf, (size_t)n); t[0] ^= 1;
    OK(utb_msg_decode(t, n, cells, &BACK) == UTM_EMAGIC, "decode: wrong magic");
    memcpy(t, buf, (size_t)n); t[1] = 1;
    OK(utb_msg_decode(t, n, cells, &BACK) == UTM_EFORMAT, "decode: format 1 is refused as a format");
    memcpy(t, buf, (size_t)n); t[1] = 2;
    OK(utb_msg_decode(t, n, cells, &BACK) == UTM_EFORMAT, "peek: format 2 is refused as a format");
    memcpy(t, buf, (size_t)n); t[1] = 4;
    OK(utb_msg_decode(t, n, cells, &BACK) == UTM_EFORMAT, "decode: format 4 is refused as a format");
    memcpy(t, buf, (size_t)n); t[6] |= 0x80;
    OK(utb_msg_decode(t, n, cells, &BACK) == UTM_EFLAGS, "decode: a reserved flag is refused");
    memcpy(t, buf, (size_t)n); t[n] = 0;
    OK(utb_msg_decode(t, n + 1, cells, &BACK) == UTM_ECHECK, "decode: a trailing byte is refused");
    OK(utb_msg_decode(buf, n, NULL, &BACK) == UTB_EBOARD, "decode: no board is refused");

    /* EVERY single-bit change past the format is refused, by the board or the
     * check, and by the header alone wherever the header can tell */
    int misread = 0, peeked = 0;
    for (int i = 2; i < n; i++)
        for (int bit = 0; bit < 8; bit++) {
            memcpy(t, buf, (size_t)n);
            t[i] ^= (uint8_t)(1u << bit);
            if (utb_msg_decode(t, n, cells, &BACK) == UTM_EOK) misread++;
            if (utb_msg_peek(t, n, NULL) == UTM_EOK) peeked++;
        }
    OK(misread == 0, "decode: every flipped bit is refused");
    OK(peeked == 0, "peek: every flipped bit is refused by the header alone");
    int cut = 0;
    for (int k = 0; k < n; k++)
        if (utb_msg_decode(buf, k, cells, &BACK) == UTM_EOK || utb_msg_peek(buf, k, NULL) == UTM_EOK) cut++;
    OK(cut == 0, "decode: every truncation is refused");
    {   /* and the same through the text: every cut of the link */
        char txt[UTB_MAX_TEXT], part[UTB_MAX_TEXT];
        int tn = utb_msg_text_encode(&M, txt, sizeof txt);
        int tcut = 0;
        for (int k = 0; k < tn; k++) {
            memcpy(part, txt, (size_t)k); part[k] = 0;
            if (utb_msg_text_decode(part, cells, &BACK) == UTM_EOK) tcut++;
        }
        OK(tn > 3 && tcut == 0, "text: every truncation of the link is refused");
    }

    /* the roster and the board must tell one story */
    int fn;
    fn = forge(t, UTB_FORMAT, seed, 88, 1, a, b, M.game.n_plies, M.game.last, crc);
    OK(utb_msg_decode(t, fn, cells, &BACK) == UTM_EOK && same_msg(&M, &BACK), "decode: the forge itself is sound");
    fn = forge(t, UTB_FORMAT, seed, 88, 0, a, b, M.game.n_plies, M.game.last, crc);
    OK(utb_msg_decode(t, fn, cells, &BACK) == UTM_EROSTER, "decode: an open seat with moves on the board");
    {
        UtbGame e;
        utb_init(&e, UTB_DEPTH);
        uint32_t ecrc = utb_board_check(e.cell, UTB_CELLS);
        fn = forge(t, UTB_FORMAT, seed, 88, 1, a, b, 0, -1, ecrc);
        OK(utb_msg_decode(t, fn, e.cell, &BACK) == UTM_EROSTER, "decode: a sealed roster with no first move");
        fn = forge(t, UTB_FORMAT, seed, 88, 0, a, b, 0, -1, ecrc);
        OK(utb_msg_decode(t, fn, e.cell, &BACK) == UTM_EOK && !BACK.sealed && BACK.game.n_plies == 0,
           "decode: a forged invitation reads");
        fn = forge(t, UTB_FORMAT, 0, 88, 0, a, b, 0, -1, ecrc);
        OK(utb_msg_decode(t, fn, e.cell, &BACK) == UTM_EROSTER, "decode: a zero seed");
    }
    fn = forge(t, UTB_FORMAT, seed, 88, 1, a, a, M.game.n_plies, M.game.last, crc);
    OK(utb_msg_decode(t, fn, cells, &BACK) == UTM_EROSTER, "decode: X and O the same person");

    /* the board: one changed cell, a board no play reaches, a wrong count */
    /* A MISREAD THAT IS STILL A LEGAL BOARD: O's first mark moved to another
     * empty square - the same counts, the same last move, no line - is what
     * only the board check can refuse */
    memcpy(CELLS, cells, UTB_CELLS);
    {
        int from = -1, to = -1;
        for (int i = 0; i < UTB_CELLS; i++) if (CELLS[i] == UTTT_O && i != M.game.last) { from = i; break; }
        for (int i = UTB_CELLS - 1; i >= 0; i--) if (!CELLS[i]) { to = i; break; }
        CELLS[from] = 0;
        CELLS[to] = UTTT_O;
        OK(from >= 0 && utb_adopt(&T3.game, UTB_DEPTH, CELLS, M.game.last) && T3.game.n_plies == M.game.n_plies,
           "decode fixture: the moved mark is a board adopt accepts");
    }
    OK(utb_msg_decode(buf, n, CELLS, &BACK) == UTB_EBOARD, "decode: one changed cell is refused (the board check)");
    memcpy(CELLS, cells, UTB_CELLS);
    for (int i = 0; i < UTB_CELLS; i++) if (!CELLS[i] && i != 30000) { CELLS[i] = UTTT_O; break; }
    fn = forge(t, UTB_FORMAT, seed, 88, 1, a, b, M.game.n_plies, M.game.last,
               utb_board_check(CELLS, UTB_CELLS));
    OK(utb_msg_decode(t, fn, CELLS, &BACK) == UTB_EBOARD,
       "decode: a board with two O for one X is refused by adopt, though its check matches");
    fn = forge(t, UTB_FORMAT, seed, 88, 0, a, b, 0, -1, utb_board_check(CELLS, UTB_CELLS));
    OK(utb_msg_decode(t, fn, CELLS, &BACK) == UTB_EBOARD,
       "decode: an invitation's link over a board with marks is refused by adopt");
    memcpy(CELLS, cells, UTB_CELLS);
    CELLS[M.game.last == 0 ? 1 : 0] = 3;
    fn = forge(t, UTB_FORMAT, seed, 88, 1, a, b, M.game.n_plies, M.game.last,
               utb_board_check(CELLS, UTB_CELLS));
    OK(utb_msg_decode(t, fn, CELLS, &BACK) == UTB_EBOARD, "decode: a cell value of 3 is refused by adopt");
    {   /* the last move's cell must hold the mark that just moved */
        int wrong = -1;
        for (int i = 0; i < UTB_CELLS; i++) if (cells[i] == UTTT_O) { wrong = i; break; }
        fn = forge(t, UTB_FORMAT, seed, 88, 1, a, b, M.game.n_plies, wrong, crc);
        OK(wrong >= 0 && utb_msg_decode(t, fn, cells, &BACK) == UTB_EBOARD,
           "decode: a last move on the other side's mark is refused by adopt");
        int empty = M.game.last == 0 ? 1 : 0;
        while (cells[empty]) empty++;
        fn = forge(t, UTB_FORMAT, seed, 88, 1, a, b, M.game.n_plies, empty, crc);
        OK(utb_msg_decode(t, fn, cells, &BACK) == UTB_EBOARD, "decode: a last move on an empty cell is refused");
    }
    fn = forge(t, UTB_FORMAT, seed, 88, 1, a, b, M.game.n_plies + 2, M.game.last, crc);
    OK(utb_msg_decode(t, fn, cells, &BACK) == UTB_EBOARD, "decode: a ply count the marks do not give is refused");
    fn = forge(t, UTB_FORMAT, seed, 88, 1, a, b, M.game.n_plies, -1, crc);
    OK(utb_msg_decode(t, fn, cells, &BACK) == UTB_EBOARD, "decode: no last move on a played board");
    fn = forge(t, UTB_FORMAT, seed, 88, 1, a, b, M.game.n_plies, UTB_CELLS, crc);
    OK(utb_msg_decode(t, fn, cells, &BACK) == UTB_EBOARD, "decode: a last move off the board");

    /* encode refuses what decode would */
    T1 = M; T1.sealed = 0;
    OK(utb_msg_encode(&T1, buf, sizeof buf) == UTM_EROSTER, "encode: will not write what decode refuses");
    OK(utb_msg_encode(&M, buf, 10) == UTM_ECAP, "encode: a short buffer");

    /* THE TEXT: found among other parameters, and in lower case */
    char txt[UTB_MAX_TEXT], url[UTB_MAX_TEXT + 64];
    int tn = utb_msg_text_encode(&M, txt, sizeof txt);
    OK(tn > 0 && tn < UTB_MAX_TEXT, "text: the link fits UTB_MAX_TEXT");
    snprintf(url, sizeof url, "https://example.invalid/x?q=1&m=%s&z=9#frag", txt + 3);
    OK(utb_msg_text_decode(url, cells, &BACK) == UTM_EOK && same_msg(&M, &BACK),
       "text: m= is found among other parameters and stops at &");
    snprintf(url, sizeof url, "?m=%s", txt + 3);
    for (char *p = url; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    OK(utb_msg_text_decode(url, cells, &BACK) == UTM_EOK && same_msg(&M, &BACK),
       "text: case-folded on the way through still reads");
    OK(utb_msg_text_decode("?v=1&s=2", cells, &BACK) == UTM_ETEXT && utb_msg_text_decode(NULL, cells, &BACK) == UTM_ETEXT,
       "text: no m=, or NULL, is refused");

    /* THE TWO FORMATS SIDE BY SIDE: each reader refuses the other's as a
     * format, and the router tells them apart by the format byte alone */
    UtmMsg small, sback;
    utm_open(&small, seed, 88, a);
    OK(utm_play(&small, b, 40), "the 9 x 9 fixture plays");
    char stxt[UTM_MAX_TEXT];
    OK(utm_text_encode(&small, stxt, sizeof stxt) > 0, "the 9 x 9 fixture has a link");
    OK(!utb_msg_text_is(stxt), "text_is: a format-2 text is not big");
    OK(utb_msg_text_is(txt), "text_is: a format-3 text is big");
    OK(utb_msg_text_peek(stxt, NULL) == UTM_EFORMAT, "peek: a format-2 text is refused as a format");
    OK(utm_text_decode(txt, &sback) == UTM_EFORMAT, "THE SHIPPED READER refuses a format-3 text as a format");
    n = utb_msg_encode(&M, buf, sizeof buf);
    OK(utm_decode(buf, n, &sback) == UTM_EFORMAT, "THE SHIPPED READER refuses format-3 bytes as a format");
}

/* --------------------------------------------------- 3 seats and resolve */

static void test_seats(void)
{
    const int32_t seed = 1726990000;
    uint8_t a[UTM_TAG_LEN], b[UTM_TAG_LEN], c[UTM_TAG_LEN];
    tag_of("alex", seed, a);
    tag_of("vera", seed, b);
    tag_of("cleo", seed, c);
    utb_msg_open(&M, seed, 7, a);

    OK(utb_msg_seat(&M, a) == UTM_SEAT_WAITING, "seat: my own invitation is waiting");
    OK(utb_msg_seat(&M, b) == UTM_SEAT_OPEN, "seat: somebody's invitation is open");
    OK(!utb_msg_can_move(&M, a) && !utb_msg_play(&M, a, 0), "seat: the creator waits, and cannot move");
    OK(utb_msg_can_move(&M, b), "seat: the open seat may move");
    OK(!utb_msg_play(&M, b, -1) && !utb_msg_play(&M, b, UTB_CELLS) && !M.sealed,
       "play: a move off the board takes nothing");

    /* the records: O at open, through the adapter */
    static uint8_t rec[UTM_REC_BYTES];
    UtmMsg r = utb_msg_roster(&M);
    int rn = utm_rec_put(rec, 0, &r, UTM_SEAT_WAITING);
    OK(utm_rec_find(rec, rn, &r) == UTM_SEAT_O, "records: the creator's O is written and found through the adapter");

    T1 = M;                                    /* the invitation, kept */
    OK(utb_msg_play(&M, b, 40000), "seat: vera joins");
    OK(M.sealed && !memcmp(M.x, b, UTM_TAG_LEN) && M.game.n_plies == 1,
       "seat: taking the seat is the first move");
    OK(utb_msg_seat(&M, b) == UTM_SEAT_X && utb_msg_seat(&M, a) == UTM_SEAT_O
       && utb_msg_seat(&M, c) == UTM_SEAT_SPECTATOR, "seat: X, O and a spectator once sealed");
    OK(utb_msg_can_move(&M, a) && !utb_msg_can_move(&M, b) && !utb_msg_can_move(&M, c),
       "seat: O is on move, X and the spectator are not");
    r = utb_msg_roster(&M);
    OK(utm_rec_find(rec, rn, &r) == UTM_SEAT_O, "records: the creator's O holds after the join");
    static uint8_t recx[UTM_REC_BYTES];
    int xn = utm_rec_put(recx, 0, &r, UTM_SEAT_X);
    UtmMsg ri = utb_msg_roster(&T1);
    OK(utm_rec_find(recx, xn, &r) == UTM_SEAT_X && utm_rec_find(recx, xn, &ri) == 0,
       "records: the joiner's X is found on the sealed game, not on the invitation");

    /* the sender witness reads the parity (X made ply 1: odd) */
    int by = -1;
    OK(utm_resolve(&r, 0, UTM_SEAT_SPECTATOR, 1, 1, &by) == UTM_SEAT_X && by == UTM_BY_SENDER,
       "roster: the sender witness reads the parity");
    OK(utm_resolve(&r, 0, UTM_SEAT_SPECTATOR, 1, 0, &by) == UTM_SEAT_O, "roster: and the other side of it");
    {
        UtbHead h;
        char t[UTB_MAX_TEXT];
        utb_msg_text_encode(&M, t, sizeof t);
        utb_msg_text_peek(t, &h);
        UtmMsg hr = utb_head_roster(&h);
        OK(utm_same_game(&hr, &r) && hr.sealed && hr.game.n_plies == 1 && !memcmp(hr.x, b, UTM_TAG_LEN),
           "roster: the header's roster is the message's");
    }

    /* undo: only my own move, and the joining move unseals */
    OK(!utb_msg_undo(&M, a) && !utb_msg_undo(&M, c), "undo: only my own move comes back");
    T2 = M;
    OK(utb_msg_undo(&T2, b) && !T2.sealed && T2.game.n_plies == 0, "undo: taking back the joining move unseals");
    OK(same_msg(&T2, &T1), "undo: and what is left is the invitation");

    /* can_replace: a different free square where the draft was played */
    int other = 40001;
    OK(utb_msg_can_replace(&M, b, other), "replace: another square on the empty board");
    OK(!utb_msg_can_replace(&M, b, 40000), "replace: the same square is not a change of mind");
    OK(!utb_msg_can_replace(&M, a, other), "replace: not mine to take back");
    OK(!utb_msg_can_replace(&M, b, -1) && !utb_msg_can_replace(&M, b, UTB_CELLS), "replace: off the board");
    T2 = M;
    OK(utb_msg_play(&T2, a, random_move(&T2.game)), "replace: alex answers");
    {
        /* O's draft is in the block X's move sent it to; a square outside it is no change */
        int ok_in = 0, out_refused = 1, region = utb_region(&M.game);
        int first = utb_node_first(&M.game, region);
        /* the region's nine squares, and 600 squares anywhere else */
        for (int k = 0; k < 9 + 600; k++) {
            int mv = k < 9 ? first + k : (int)(rnd() % UTB_CELLS);
            if (mv == T2.game.last || (k >= 9 && mv >= first && mv < first + 9)) continue;
            int can = utb_msg_can_replace(&T2, a, mv);
            int legal_before = utb_legal_at(&M.game, mv);
            if (can != legal_before) out_refused = 0;
            if (can) ok_in++;
        }
        OK(out_refused && ok_in == 8 && region > 0,
           "replace: exactly the other free squares of the draft's region");
    }
    T3 = T2;
    (void)utb_msg_can_replace(&T2, a, 40002);
    OK(same_msg(&T2, &T3), "replace: pure");

    /* AFTER AN ADOPT the move before is unknown: nothing comes back */
    {
        char t[UTB_MAX_TEXT];
        utb_msg_text_encode(&T2, t, sizeof t);
        OK(utb_msg_text_decode(t, T2.game.cell, &T3) == UTM_EOK, "adopt: the answer reads");
        OK(!utb_msg_undo(&T3, a) && T3.game.n_plies == 2, "undo: refused after an adopt (the move before is unknown)");
        int any = 0;
        int first = utb_node_first(&T1.game, utb_region(&M.game));
        for (int mv = first; mv < first + 9; mv++) any |= utb_msg_can_replace(&T3, a, mv);
        OK(!any, "replace: refused after an adopt");
    }

    /* the door */
    OK(utb_msg_door(&M) == UTM_DOOR_NONE, "door: none while the game runs");
    OK(utb_msg_door(&FIN) == UTM_DOOR_AGAIN, "door: Again once the game is over");
    OK(!utb_msg_again(&T3, &M, 1790000500, b), "again: refused on a live game");
    OK(utb_msg_again(&T3, &FIN, 1790000500, FIN_B) && T3.look == FIN.look && T3.seed == 1790000500
       && !T3.sealed && !memcmp(T3.o, FIN_B, UTM_TAG_LEN) && T3.game.n_plies == 0,
       "again: on the same napkin");
    T2 = FIN;
    OK(utb_msg_again(&T2, &T2, 1790000501, FIN_A) && T2.look == FIN.look && T2.game.n_plies == 0
       && !memcmp(T2.o, FIN_A, UTM_TAG_LEN), "again: the finished game's own slot may take the rematch");
}

/* ----------------------------------------------------------- 4 prefer */

static void head_of_msg(const UtbMsg *m, UtbHead *h)
{
    char t[UTB_MAX_TEXT];
    utb_msg_text_encode(m, t, sizeof t);
    if (utb_msg_text_peek(t, h) != UTM_EOK) memset(h, 0, sizeof *h);
}

static void test_prefer(void)
{
    const int32_t seed = 1726991111;
    uint8_t a[UTM_TAG_LEN], b[UTM_TAG_LEN], c[UTM_TAG_LEN];
    tag_of("alex", seed, a);
    tag_of("vera", seed, b);
    tag_of("cleo", seed, c);
    UtbHead inv, vb, vb2, vb2r, cb, other;
    utb_msg_open(&M, seed, 1, a);
    head_of_msg(&M, &inv);
    T1 = M;
    utb_msg_play(&T1, b, 100);
    head_of_msg(&T1, &vb);
    T2 = T1;
    utb_msg_play(&T2, a, random_move(&T2.game));
    head_of_msg(&T2, &vb2);
    T3 = T1;
    {   /* the same ply, a different square: a change of mind */
        int alt = -1;
        for (int mv = 0; mv < UTB_CELLS; mv++)
            if (mv != T2.game.last && utb_legal_at(&T1.game, mv)) { alt = mv; break; }
        utb_msg_play(&T3, a, alt);
    }
    head_of_msg(&T3, &vb2r);
    T3 = M;
    utb_msg_play(&T3, c, 100);
    head_of_msg(&T3, &cb);
    utb_msg_open(&T3, seed + 1, 1, a);
    head_of_msg(&T3, &other);

    OK(utb_head_prefer(&vb, &other) > 0 && utb_head_prefer(&other, &vb) > 0,
       "prefer: different games: the tapped one");
    OK(utb_head_prefer(&vb, &inv) < 0 && utb_head_prefer(&inv, &vb) > 0,
       "prefer: sealed beats its own invitation");
    OK(utb_head_prefer(&vb2, &vb) < 0 && utb_head_prefer(&vb, &vb2) > 0, "prefer: more plies wins");
    OK(utb_head_prefer(&vb2r, &vb2) < 0 && utb_head_prefer(&vb2, &vb2r) < 0,
       "prefer: one roster, equal plies, a change of mind: mine");
    OK(utb_head_prefer(&vb2, &vb2) == 0 && utb_head_prefer(&inv, &inv) == 0, "prefer: identical is 0");
    OK(utb_head_same_game(&vb, &cb) && utb_head_same_game(&inv, &vb2) && !utb_head_same_game(&vb, &other),
       "same game: seed and creator");

    /* TWO JOINERS: the lower SHA-256("uttt.join.big|" seed X) wins, both ways round */
    uint8_t kv[32], kc[32], s[4];
    uint32_t u = (uint32_t)seed;
    s[0] = (uint8_t)(u >> 24); s[1] = (uint8_t)(u >> 16); s[2] = (uint8_t)(u >> 8); s[3] = (uint8_t)u;
    Sha256 h;
    sha256_init(&h); sha256_update(&h, "uttt.join.big|", 14); sha256_update(&h, s, 4);
    sha256_update(&h, b, UTM_TAG_LEN); sha256_final(&h, kv);
    sha256_init(&h); sha256_update(&h, "uttt.join.big|", 14); sha256_update(&h, s, 4);
    sha256_update(&h, c, UTM_TAG_LEN); sha256_final(&h, kc);
    int v_wins = memcmp(kv, kc, 32) < 0;
    int pv = utb_head_prefer(&vb, &cb), pc = utb_head_prefer(&cb, &vb);
    OK(pv != 0 && pc != 0 && (pv < 0) == v_wins && (pc < 0) == !v_wins,
       "prefer: two joiners by the join key");
}

/* ---------------------------------------------------------- 5 caption */

static void test_caption(void)
{
    char got[96], want[96];
    const int32_t seed = 1726992222;
    uint8_t a[UTM_TAG_LEN], b[UTM_TAG_LEN];
    tag_of("alex", seed, a);
    tag_of("vera", seed, b);
    utb_msg_open(&M, seed, 1, a);
    int n = utb_msg_caption(&M, NULL, got, sizeof got);
    uttt_caption(0, UTTT_X, -1, -1, 0, NULL, want, sizeof want);
    OK(n > 0 && !strcmp(got, want) && !strcmp(got, "New game?"), "caption: the invitation");
    utb_msg_caption(&M, "$who", got, sizeof got);
    uttt_caption(0, UTTT_X, -1, -1, 0, "$who", want, sizeof want);
    OK(!strcmp(got, want), "caption: the invitation, named");
    utb_msg_play(&M, b, 5);
    n = utb_msg_caption(&M, NULL, got, sizeof got);
    uttt_caption(0, UTTT_O, 9, -1, 1, NULL, want, sizeof want);
    OK(n > 0 && n <= UTTT_CAPTION_MAX && !strcmp(got, want) && !strcmp(got, "O to play"), "caption: in play");
    n = utb_msg_caption(&FIN, NULL, got, sizeof got);
    uttt_caption(FIN.game.over, FIN.game.turn, 9, -1, FIN.game.n_plies, NULL, want, sizeof want);
    OK(n > 0 && n <= UTTT_CAPTION_MAX && !strcmp(got, want)
       && strstr(got, FIN.game.over == UTTT_DRAW ? "Drawn in " : " won in "),
       "caption: the finished game, in the 9 x 9's words");
    printf("  caption of the whole game: \"%s\"\n", got);
    /* A DRAW, and the longest win there could be: the parts alone decide */
    T1 = FIN;
    T1.game.over = UTTT_DRAW;
    T1.game.n_plies = UTB_CELLS;
    n = utb_msg_caption(&T1, NULL, got, sizeof got);
    uttt_caption(UTTT_DRAW, T1.game.turn, 9, -1, UTB_CELLS, NULL, want, sizeof want);
    OK(n > 0 && n <= UTTT_CAPTION_MAX && !strcmp(got, want) && !strcmp(got, "Drawn in 59049 moves"),
       "caption: a draw");
    T1.game.over = UTTT_O;
    n = utb_msg_caption(&T1, NULL, got, sizeof got);
    uttt_caption(UTTT_O, T1.game.turn, 9, -1, UTB_CELLS, NULL, want, sizeof want);
    OK(n > 0 && n <= UTTT_CAPTION_MAX && !strcmp(got, want) && !strcmp(got, "O won in 59049 moves"),
       "caption: a win, and the longest fits one line");
    T1.game.over = UTTT_X;
    utb_msg_caption(&T1, "$who", got, sizeof got);
    uttt_caption(UTTT_X, T1.game.turn, 9, -1, UTB_CELLS, "$who", want, sizeof want);
    OK(!strcmp(got, want) && !strcmp(got, "$who won in 59049 moves"), "caption: a win, named by its sender");
}

/* ------------------------------------------------------------- 6 crc */

static void test_crc(void)
{
    const uint8_t *nine = (const uint8_t *)"123456789";
    OK(utb_board_check(nine, 9) == 0xCBF43926u && bd_crc32(0, nine, 9) == 0xCBF43926u,
       "crc: the standard check value");
    int agree = 1;
    for (int k = 0; k < 20; k++) {
        for (int i = 0; i < UTB_CELLS; i++) CELLS[i] = (uint8_t)(rnd() % 3);
        if (utb_board_check(CELLS, UTB_CELLS) != bd_crc32(0, CELLS, UTB_CELLS)) agree = 0;
        int cut = (int)(rnd() % UTB_CELLS);
        if (utb_board_check(CELLS, cut) != bd_crc32(0, CELLS, cut)) agree = 0;
    }
    OK(agree, "crc: the kit's bd_crc32 on random boards");
    OK(utb_board_check(NULL, 0) == 0 && bd_crc32(0, NULL, 0) == 0, "crc: nothing is 0, as the kit's");
}

/* ------------------------------------------------- 7 the 9 x 9 golden */

/* THE SHIPPED FORMAT, PINNED: a fixed format-2 message, its bytes and its
 * text. Computed once from uttt_msg.c as it shipped (1.0(13)) and pasted
 * here; any change to how the 9 x 9 writes a bubble shows as this failing,
 * which is what makes "format 3 sits beside it untouched" checkable. */
static void test_golden(void)
{
    static const char HEX[] =
        "b70266f0d580016a5cf6f0753e83dbf81c12f8053463485962a7778ea8beda1002";
    static const char TEXT[] = "?m=W4BGN4GVQAAWUXHW6B2T5A637AOBF6AFGRRUQWLCU53Y5KF63IIAE";
    const int32_t seed = 1727059328;
    uint8_t o[UTM_TAG_LEN], x[UTM_TAG_LEN];
    tag_of("golden-o", seed, o);
    tag_of("golden-x", seed, x);
    UtmMsg m;
    utm_open(&m, seed, 0x6a, o);
    static const int mv[] = { 40, 36, 4, 41, 49, 37, 13, 38 };
    int played = 1;
    for (unsigned i = 0; i < sizeof mv / sizeof *mv; i++)
        played &= utm_play(&m, i % 2 ? o : x, mv[i]);
    OK(played, "golden: the fixed game plays");
    uint8_t b[UTM_MAX_BYTES];
    int n = utm_encode(&m, b, sizeof b);
    char hex[2 * UTM_MAX_BYTES + 1];
    for (int i = 0; i < n; i++) snprintf(hex + 2 * i, 3, "%02x", b[i]);
    if (n <= 0) hex[0] = 0;
    char t[UTM_MAX_TEXT];
    utm_text_encode(&m, t, sizeof t);
    OK(!strcmp(hex, HEX), "golden: the 9 x 9 bytes are the shipped format");
    OK(!strcmp(t, TEXT), "golden: the 9 x 9 text is the shipped format");
    if (strcmp(hex, HEX) || strcmp(t, TEXT)) printf("  golden now: %s\n  golden now: %s\n", hex, t);
}

int main(int argc, char **argv)
{
    int games = argc > 1 ? atoi(argv[1]) : 20;
    if (games < 1) games = 1;
    double t0 = now();
    test_crc();
    test_golden();
    test_games(games);
    test_refusals();
    test_seats();
    test_prefer();
    test_caption();
    printf("uttt_big_msg_test: %d checks, %d failed, %.2f s\n", checks, fails, now() - t0);
    return fails ? 1 : 0;
}
