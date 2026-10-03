#include "uttt_big_msg.h"
#include "uttt_say.h"
#include "../../../shared/c/sha256.h"
#include "../../../shared/c/wire_check/wire_check.h"
#include "../../../shared/c/b32.h"
#include <string.h>

/* ------------------------------------------------------------ the check */

/* CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320, ~0 in and out) - the
 * kit's bd_crc32 over one span. It runs over 59,049 cells on every encode and
 * every decode, so it is slice-by-8: eight tables of 256, eight bytes a step.
 * Profiled (sample) before it was: a byte-table loop was three quarters of a
 * round trip at 200 us a pass, one dependent table load a byte. The tables
 * are built on first use from the polynomial the kit's bit loop applies, and
 * the test holds the two to each other and to the standard check value. */
static uint32_t crc_table[8][256];
static int      crc_ready;

static void crc_build(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        crc_table[0][i] = c;
    }
    for (int t = 1; t < 8; t++)
        for (int i = 0; i < 256; i++) {
            uint32_t c = crc_table[t - 1][i];
            crc_table[t][i] = (c >> 8) ^ crc_table[0][c & 0xFFu];
        }
    crc_ready = 1;
}

uint32_t utb_board_check(const uint8_t *cells, int n)
{
    if (!crc_ready) crc_build();
    uint32_t c = 0xFFFFFFFFu;
    if (!cells) return 0;
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        const uint8_t *p = cells + i;
        uint32_t lo = c ^ ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
        c = crc_table[7][lo & 0xFFu] ^ crc_table[6][(lo >> 8) & 0xFFu]
          ^ crc_table[5][(lo >> 16) & 0xFFu] ^ crc_table[4][lo >> 24]
          ^ crc_table[3][p[4]] ^ crc_table[2][p[5]] ^ crc_table[1][p[6]] ^ crc_table[0][p[7]];
    }
    for (; i < n; i++)
        c = (c >> 8) ^ crc_table[0][(c ^ cells[i]) & 0xFFu];
    return ~c;
}

/* ------------------------------------------------------------ the bytes */

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint32_t get16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int is(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, UTM_TAG_LEN) == 0; }

void utb_msg_open(UtbMsg *m, int32_t seed, uint8_t look, const uint8_t me[UTM_TAG_LEN])
{
    memset(m, 0, sizeof *m);
    m->seed = seed ? seed : 1;
    m->look = look;
    memcpy(m->o, me, UTM_TAG_LEN);
    utb_init(&m->game, UTB_DEPTH);
}

int utb_msg_again(UtbMsg *next, const UtbMsg *finished, int32_t seed, const uint8_t me[UTM_TAG_LEN])
{
    if (utb_msg_door(finished) != UTM_DOOR_AGAIN) return 0;
    /* read before the write: `next` may be `finished` (the bridge's one slot) */
    uint8_t look = finished->look, tag[UTM_TAG_LEN];
    memcpy(tag, me, UTM_TAG_LEN);
    utb_msg_open(next, seed, look, tag);
    return 1;
}

/* THE HEADER'S OWN STORY, told before any board is read: the seed is a seed,
 * the roster agrees with the ply count (the joiner's first move IS the seal,
 * as in format 2) and nobody plays themselves; the last move is there exactly
 * when a move is, and on the board. */
static int head_ok(const UtbHead *h)
{
    if (h->seed == 0) return UTM_EROSTER;
    if (!h->sealed && h->n_plies != 0) return UTM_EROSTER;
    if (h->sealed && (h->n_plies == 0 || is(h->x, h->o))) return UTM_EROSTER;
    if (h->n_plies < 0 || h->n_plies > UTB_CELLS) return UTB_EBOARD;
    if ((h->n_plies == 0) != (h->last == UTB_NONE)) return UTB_EBOARD;
    if (h->last != UTB_NONE && (h->last < 0 || h->last >= UTB_CELLS)) return UTB_EBOARD;
    return UTM_EOK;
}

static UtbHead head_of(const UtbMsg *m)
{
    UtbHead h;
    memset(&h, 0, sizeof h);
    h.seed = m->seed;
    h.look = m->look;
    h.sealed = m->sealed;
    memcpy(h.o, m->o, UTM_TAG_LEN);
    if (m->sealed) memcpy(h.x, m->x, UTM_TAG_LEN);
    h.n_plies = m->game.n_plies;
    h.last = m->game.last;
    h.board_check = utb_board_check(m->game.cell, UTB_CELLS);
    return h;
}

int utb_head_encode(const UtbHead *h, uint8_t *out, int cap)
{
    int r = head_ok(h);
    if (r != UTM_EOK) return r;
    uint8_t buf[UTB_MAX_BYTES];
    int n = 0;
    buf[n++] = UTM_MAGIC;
    buf[n++] = UTB_FORMAT;
    put32(buf + n, (uint32_t)h->seed); n += 4;
    buf[n++] = h->sealed ? UTM_FLAG_SEALED : 0;
    buf[n++] = h->look;
    memcpy(buf + n, h->o, UTM_TAG_LEN); n += UTM_TAG_LEN;
    if (h->sealed) { memcpy(buf + n, h->x, UTM_TAG_LEN); n += UTM_TAG_LEN; }
    put16(buf + n, (uint32_t)h->n_plies); n += 2;
    put16(buf + n, h->last == UTB_NONE ? UTB_LAST_NONE : (uint32_t)h->last); n += 2;
    put32(buf + n, h->board_check); n += 4;
    /* the check is last, over every byte before it (one span, no body) */
    wire_check(buf, (size_t)n, NULL, 0, buf + n, UTM_CHECK_LEN);
    n += UTM_CHECK_LEN;
    if (!out || n > cap) return UTM_ECAP;
    memcpy(out, buf, (size_t)n);
    return n;
}

int utb_msg_encode(const UtbMsg *m, uint8_t *out, int cap)
{
    if (m->game.depth != UTB_DEPTH) return UTB_EBOARD;
    UtbHead h = head_of(m);
    return utb_head_encode(&h, out, cap);
}

int utb_msg_peek(const uint8_t *in, int n, UtbHead *out)
{
    if (!in || n < 2) return UTM_ESHORT;
    if (in[0] != UTM_MAGIC) return UTM_EMAGIC;
    if (in[1] != UTB_FORMAT) return UTM_EFORMAT;
    if (n < UTM_HEAD_OPEN) return UTM_ESHORT;
    uint8_t flags = in[6];
    if (flags & ~UTM_FLAGS_KNOWN) return UTM_EFLAGS;
    int sealed = (flags & UTM_FLAG_SEALED) != 0;
    int body = (sealed ? UTB_HEAD_SEALED : UTB_HEAD_OPEN);
    int want = body + UTM_CHECK_LEN;
    if (n < want) return UTM_ESHORT;
    /* A FIXED LENGTH: anything after the check is a link somebody edited,
     * and a reader that ignored it would vouch for bytes it never checked. */
    if (n > want) return UTM_ECHECK;
    uint8_t check[UTM_CHECK_LEN];
    wire_check(in, (size_t)body, NULL, 0, check, UTM_CHECK_LEN);
    if (memcmp(check, in + body, UTM_CHECK_LEN) != 0) return UTM_ECHECK;

    UtbHead h;
    memset(&h, 0, sizeof h);
    h.seed = (int32_t)get32(in + 2);
    h.look = in[7];
    h.sealed = (uint8_t)sealed;
    memcpy(h.o, in + UTM_TAGS_AT, UTM_TAG_LEN);
    int at = UTM_HEAD_OPEN;
    if (sealed) { memcpy(h.x, in + at, UTM_TAG_LEN); at += UTM_TAG_LEN; }
    h.n_plies = (int32_t)get16(in + at);
    uint32_t last = get16(in + at + 2);
    h.last = last == UTB_LAST_NONE ? UTB_NONE : (int32_t)last;
    h.board_check = get32(in + at + 4);
    int r = head_ok(&h);
    if (r != UTM_EOK) return r;
    if (out) *out = h;
    return UTM_EOK;
}

int utb_msg_decode(const uint8_t *in, int n, const uint8_t *cells, UtbMsg *out)
{
    UtbHead h;
    int r = utb_msg_peek(in, n, &h);
    if (r != UTM_EOK) return r;
    if (!cells) return UTB_EBOARD;
    /* the picture is THIS link's board, or nothing is read */
    if (utb_board_check(cells, UTB_CELLS) != h.board_check) return UTB_EBOARD;
    UtbGame g;
    if (!utb_adopt(&g, UTB_DEPTH, cells, h.last)) return UTB_EBOARD;
    if (g.n_plies != h.n_plies) return UTB_EBOARD;
    out->seed = h.seed;
    out->look = h.look;
    out->sealed = h.sealed;
    memcpy(out->o, h.o, UTM_TAG_LEN);
    memcpy(out->x, h.x, UTM_TAG_LEN);
    out->game = g;
    return UTM_EOK;
}

/* ------------------------------------------------------------- the text */

int utb_msg_text_encode(const UtbMsg *m, char *out, int cap)
{
    uint8_t b[UTB_MAX_BYTES];
    int n = utb_msg_encode(m, b, sizeof b);
    if (n < 0) return n;
    if (!out || cap < 4) return UTM_ECAP;
    memcpy(out, "?m=", 3);
    int w = b32_encode(b, n, out + 3, cap - 3);
    if (w < 0) return UTM_ECAP;
    return 3 + w;
}

/* THE READER'S SPAN is sized for the longest link of EITHER format, so a
 * format-2 text is read far enough to be refused as a format (UTM_EFORMAT),
 * never as a text that ran long. */
#define SPAN_BYTES (UTM_MAX_BYTES > UTB_MAX_BYTES ? UTM_MAX_BYTES : UTB_MAX_BYTES)
#define SPAN_TEXT  (UTM_MAX_TEXT > UTB_MAX_TEXT ? UTM_MAX_TEXT : UTB_MAX_TEXT)

/* The value of `m` in the query, as bytes: after "?m=" or "&m=", up to the
 * next '&' or '#' - utm_text_decode's reading, the same span rule. The byte
 * count, or UTM_ETEXT. */
static int text_bytes(const char *text, uint8_t *b, int cap)
{
    if (!text) return UTM_ETEXT;
    const char *q = strchr(text, '?');
    const char *v = 0;
    while (q) {
        if (q[1] == 'm' && q[2] == '=') { v = q + 3; break; }
        q = strchr(q + 1, '&');
    }
    if (!v) return UTM_ETEXT;
    char span[SPAN_TEXT];
    int k = 0;
    while (v[k] && v[k] != '&' && v[k] != '#') {
        if (k >= (int)sizeof span - 1) return UTM_ETEXT;
        span[k] = v[k];
        k++;
    }
    span[k] = 0;
    int n = b32_decode(span, b, cap);
    return n <= 0 ? UTM_ETEXT : n;
}

int utb_msg_text_bytes(const char *text, uint8_t *out, int cap)
{
    return text_bytes(text, out, cap);
}

int utb_msg_text_peek(const char *text, UtbHead *out)
{
    uint8_t b[SPAN_BYTES];
    int n = text_bytes(text, b, sizeof b);
    if (n < 0) return n;
    return utb_msg_peek(b, n, out);
}

int utb_msg_text_decode(const char *text, const uint8_t *cells, UtbMsg *out)
{
    uint8_t b[SPAN_BYTES];
    int n = text_bytes(text, b, sizeof b);
    if (n < 0) return n;
    return utb_msg_decode(b, n, cells, out);
}

int utb_msg_text_is(const char *text)
{
    uint8_t b[SPAN_BYTES];
    int n = text_bytes(text, b, sizeof b);
    return n >= 2 && b[0] == UTM_MAGIC && b[1] == UTB_FORMAT;
}

/* ------------------------------------------------------------ the roster */

/* EVERYTHING THE SHIPPED SEAT FUNCTIONS READ of a UtmMsg is its seed, look,
 * sealed flag, two tags and the PARITY of game.n_plies (utm_resolve's "who
 * moved last"); utm_seat, utm_same_game and utm_rec_* read no game at all.
 * The rest of `game` is a fresh 9 x 9 board and means nothing. */
static UtmMsg roster(int32_t seed, uint8_t look, uint8_t sealed, const uint8_t *o,
                     const uint8_t *x, int32_t n_plies)
{
    UtmMsg r;
    memset(&r, 0, sizeof r);
    r.seed = seed;
    r.look = look;
    r.sealed = sealed;
    memcpy(r.o, o, UTM_TAG_LEN);
    if (sealed) memcpy(r.x, x, UTM_TAG_LEN);
    uttt_init(&r.game);
    r.game.n_plies = (uint8_t)(n_plies % 2);
    return r;
}

UtmMsg utb_msg_roster(const UtbMsg *m)
{
    return roster(m->seed, m->look, m->sealed, m->o, m->x, m->game.n_plies);
}

UtmMsg utb_head_roster(const UtbHead *h)
{
    return roster(h->seed, h->look, h->sealed, h->o, h->x, h->n_plies);
}

int utb_msg_seat(const UtbMsg *m, const uint8_t me[UTM_TAG_LEN])
{
    UtmMsg r = utb_msg_roster(m);
    return utm_seat(&r, me);
}

/* utm_can_move's rule, over this board: the game runs, I have a mark, and it
 * is that mark's turn. */
int utb_msg_can_move(const UtbMsg *m, const uint8_t me[UTM_TAG_LEN])
{
    if (m->game.over) return 0;
    int mark = utm_seat_mark(utb_msg_seat(m, me));
    return mark && m->game.turn == mark;
}

int utb_msg_play(UtbMsg *m, const uint8_t me[UTM_TAG_LEN], int mv)
{
    if (mv < 0 || mv >= UTB_CELLS || !utb_msg_can_move(m, me)) return 0;
    /* utb_play changes nothing when it refuses, so the seat is taken only
     * once the move is known to stand: one act, as utm_play */
    if (!utb_play(&m->game, mv)) return 0;
    if (!m->sealed) {
        memcpy(m->x, me, UTM_TAG_LEN);
        m->sealed = 1;
    }
    return 1;
}

int utb_msg_undo(UtbMsg *m, const uint8_t me[UTM_TAG_LEN])
{
    int np = m->game.n_plies;
    if (np == 0) return 0;
    /* X plays the even plies; only my own move comes back (utm_undo) */
    int last = (np - 1) % 2 == 0 ? UTTT_X : UTTT_O;
    if (utm_seat_mark(utb_msg_seat(m, me)) != last) return 0;
    if (!utb_undo(&m->game)) return 0;
    if (m->game.n_plies == 0) {
        m->sealed = 0;
        memset(m->x, 0, UTM_TAG_LEN);
    }
    return 1;
}

int utb_msg_can_replace(const UtbMsg *m, const uint8_t me[UTM_TAG_LEN], int mv)
{
    if (mv < 0 || mv >= UTB_CELLS || m->game.n_plies == 0) return 0;
    if (mv == m->game.last) return 0;
    UtbMsg c = *m;                       /* pure: the question is asked of a copy */
    if (!utb_msg_undo(&c, me)) return 0;
    return utb_msg_play(&c, me, mv);
}

int utb_msg_door(const UtbMsg *m)
{
    return m->game.over ? UTM_DOOR_AGAIN : UTM_DOOR_NONE;
}

/* ------------------------------------------------------- two messages */

int utb_head_same_game(const UtbHead *a, const UtbHead *b)
{
    return a->seed == b->seed && is(a->o, b->o);
}

/* SHA-256("uttt.join.big|" || seed || X's tag). The 9 x 9's key adds the
 * first move; a big bubble does not carry its history, and the joiner's tag
 * is already salted with the seed, so the fork is named by who took it. */
static void join_key(const UtbHead *h, uint8_t out[32])
{
    static const char salt[] = "uttt.join.big|";
    uint8_t s[4];
    put32(s, (uint32_t)h->seed);
    Sha256 c;
    sha256_init(&c);
    sha256_update(&c, salt, sizeof salt - 1);
    sha256_update(&c, s, 4);
    sha256_update(&c, h->x, UTM_TAG_LEN);
    sha256_final(&c, out);
}

int utb_head_prefer(const UtbHead *mine, const UtbHead *tapped)
{
    if (!utb_head_same_game(mine, tapped)) return 1;
    if (mine->sealed != tapped->sealed) return mine->sealed ? -1 : 1;
    if (mine->n_plies != tapped->n_plies) return mine->n_plies > tapped->n_plies ? -1 : 1;
    int one_roster = !mine->sealed || is(mine->x, tapped->x);
    if (one_roster)
        return mine->last == tapped->last && mine->board_check == tapped->board_check ? 0 : -1;
    uint8_t ka[32], kb[32];
    join_key(mine, ka);
    join_key(tapped, kb);
    int c = memcmp(ka, kb, 32);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

int utb_msg_caption(const UtbMsg *m, const char *who, char *out, int cap)
{
    const UtbGame *g = &m->game;
    return uttt_caption(g->over, g->turn, g->n_plies ? 9 : -1, -1, g->n_plies, who, out, cap);
}
