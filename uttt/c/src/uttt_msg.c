#include "uttt_msg.h"
#include "uttt_code.h"
#include "../../../shared/c/sha256.h"
#include "../../../shared/c/b32.h"
#include <string.h>

static void put32(uint8_t *p, int32_t v)
{
    uint32_t u = (uint32_t)v;
    p[0] = (uint8_t)(u >> 24); p[1] = (uint8_t)(u >> 16);
    p[2] = (uint8_t)(u >> 8);  p[3] = (uint8_t)u;
}

static int32_t get32(const uint8_t *p)
{
    return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                     ((uint32_t)p[2] << 8)  |  (uint32_t)p[3]);
}

void utm_tag(int32_t seed, const uint8_t *id, int id_len, uint8_t out[UTM_TAG_LEN])
{
    static const char salt[] = "uttt.seat.1|";
    uint8_t s[4], d[SHA256_DIGEST_LEN];
    put32(s, seed);
    Sha256 c;
    sha256_init(&c);
    sha256_update(&c, salt, sizeof salt - 1);
    sha256_update(&c, s, 4);
    if (id && id_len > 0) sha256_update(&c, id, (size_t)id_len);
    sha256_final(&c, d);
    memcpy(out, d, UTM_TAG_LEN);
}

int32_t utm_seed_at(int64_t unix_seconds)
{
    int32_t s = (int32_t)(uint32_t)(uint64_t)unix_seconds;
    return s ? s : 1;
}

void utm_open(UtmMsg *m, int32_t seed, const uint8_t me[UTM_TAG_LEN])
{
    memset(m, 0, sizeof *m);
    m->seed = seed ? seed : 1;
    memcpy(m->o, me, UTM_TAG_LEN);
    uttt_init(&m->game);
}

/* The roster and the game have to tell the same story: the joiner's first
 * move IS the seal, so an open invitation has no plies and a sealed game has
 * at least one. And nobody plays themselves. */
static int roster_ok(const UtmMsg *m)
{
    if (!m->sealed) return m->game.n_plies == 0;
    if (m->game.n_plies == 0) return 0;
    return memcmp(m->x, m->o, UTM_TAG_LEN) != 0;
}

static void check_of(const uint8_t *head, int hn, const uint8_t *code, int cn,
                     uint8_t out[UTM_CHECK_LEN])
{
    uint8_t d[SHA256_DIGEST_LEN];
    Sha256 c;
    sha256_init(&c);
    sha256_update(&c, head, (size_t)hn);
    sha256_update(&c, code, (size_t)cn);
    sha256_final(&c, d);
    memcpy(out, d, UTM_CHECK_LEN);
}

int utm_encode(const UtmMsg *m, uint8_t *out, int cap)
{
    if (!roster_ok(m)) return UTM_EROSTER;
    uint8_t buf[UTM_MAX_BYTES];
    int n = 0;
    buf[n++] = UTM_MAGIC;
    buf[n++] = UTM_FORMAT;
    put32(buf + n, m->seed); n += 4;
    buf[n++] = m->sealed ? UTM_FLAG_SEALED : 0;
    memcpy(buf + n, m->o, UTM_TAG_LEN); n += UTM_TAG_LEN;
    if (m->sealed) { memcpy(buf + n, m->x, UTM_TAG_LEN); n += UTM_TAG_LEN; }
    int head = n;
    n += UTM_CHECK_LEN;
    int cn = uttt_encode(&m->game, buf + n, UTM_MAX_CODE);
    if (cn <= 0) return UTM_EGAME;
    check_of(buf, head, buf + n, cn, buf + head);
    n += cn;
    if (n > cap) return UTM_ECAP;
    memcpy(out, buf, (size_t)n);
    return n;
}

int utm_decode(const uint8_t *in, int n, UtmMsg *out)
{
    if (!in || n < 2) return UTM_ESHORT;
    if (in[0] != UTM_MAGIC) return UTM_EMAGIC;
    if (in[1] != UTM_FORMAT) return UTM_EFORMAT;
    if (n < UTM_HEAD_OPEN) return UTM_ESHORT;
    uint8_t flags = in[6];
    if (flags & ~UTM_FLAGS_KNOWN) return UTM_EFLAGS;
    int sealed = (flags & UTM_FLAG_SEALED) != 0;
    int head = sealed ? UTM_HEAD_SEALED : UTM_HEAD_OPEN;
    /* the check and at least one byte of game */
    if (n < head + UTM_CHECK_LEN + 1) return UTM_ESHORT;

    const uint8_t *code = in + head + UTM_CHECK_LEN;
    int cn = n - head - UTM_CHECK_LEN;
    uint8_t want[UTM_CHECK_LEN];
    check_of(in, head, code, cn, want);
    if (memcmp(want, in + head, UTM_CHECK_LEN) != 0) return UTM_ECHECK;

    UtmMsg m;
    memset(&m, 0, sizeof m);
    m.seed = get32(in + 2);
    m.sealed = (uint8_t)sealed;
    memcpy(m.o, in + 7, UTM_TAG_LEN);
    if (sealed) memcpy(m.x, in + 7 + UTM_TAG_LEN, UTM_TAG_LEN);
    if (m.seed == 0) return UTM_EROSTER;
    if (!uttt_decode(&m.game, code, (size_t)cn)) return UTM_EGAME;
    if (!roster_ok(&m)) return UTM_EROSTER;
    *out = m;
    return UTM_EOK;
}

int utm_text_encode(const UtmMsg *m, char *out, int cap)
{
    uint8_t b[UTM_MAX_BYTES];
    int n = utm_encode(m, b, sizeof b);
    if (n < 0) return n;
    if (cap < 4) return UTM_ECAP;
    memcpy(out, "?m=", 3);
    int w = b32_encode(b, n, out + 3, cap - 3);
    if (w < 0) return UTM_ECAP;
    return 3 + w;
}

int utm_text_decode(const char *text, UtmMsg *out)
{
    if (!text) return UTM_ETEXT;
    /* The value of `m` in the query: after "?m=" or "&m=", up to the next
     * '&' or '#'. Copied out first, because the base32 reader skips what it
     * does not know and would otherwise read straight on into the next
     * parameter. */
    const char *q = strchr(text, '?');
    const char *v = 0;
    while (q) {
        if (q[1] == 'm' && q[2] == '=') { v = q + 3; break; }
        q = strchr(q + 1, '&');
    }
    if (!v) return UTM_ETEXT;
    char span[UTM_MAX_TEXT];
    int k = 0;
    while (v[k] && v[k] != '&' && v[k] != '#') {
        if (k >= (int)sizeof span - 1) return UTM_ETEXT;
        span[k] = v[k];
        k++;
    }
    span[k] = 0;
    uint8_t b[UTM_MAX_BYTES];
    int n = b32_decode(span, b, sizeof b);
    if (n <= 0) return UTM_ETEXT;
    return utm_decode(b, n, out);
}

/* ------------------------------------------------------------ the seats */

static int is(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, UTM_TAG_LEN) == 0;
}

int utm_seat(const UtmMsg *m, const uint8_t me[UTM_TAG_LEN])
{
    if (!m->sealed) return is(me, m->o) ? UTM_SEAT_WAITING : UTM_SEAT_OPEN;
    if (is(me, m->x)) return UTM_SEAT_X;
    if (is(me, m->o)) return UTM_SEAT_O;
    return UTM_SEAT_SPECTATOR;
}

const char *utm_seat_why(const UtmMsg *m, const uint8_t me[UTM_TAG_LEN])
{
    if (!m->sealed)
        return is(me, m->o) ? "waiting: open invitation, my tag is O's (I made it)"
                            : "open: somebody's invitation, my tag is not O's, X is mine to take";
    if (is(me, m->x)) return "X: sealed, my tag is X's";
    if (is(me, m->o)) return "O: sealed, my tag is O's";
    return "spectator: sealed, my tag is neither O's nor X's";
}

int utm_seat_mark(int seat)
{
    switch (seat) {
    case UTM_SEAT_X: case UTM_SEAT_OPEN: return UTTT_X;
    case UTM_SEAT_O:                     return UTTT_O;
    default:                             return 0;
    }
}

/* ----------------------------------------------- the seat, resolved */

/* The seat a mark sits in: sealed, X or O; unsealed, O is the waiting
 * creator and X is the open seat. */
static int seat_of_mark(const UtmMsg *m, int mark)
{
    if (m->sealed) return mark == UTTT_X ? UTM_SEAT_X : UTM_SEAT_O;
    return mark == UTTT_X ? UTM_SEAT_OPEN : UTM_SEAT_WAITING;
}

int utm_resolve(const UtmMsg *m, int record, int tag_seat, int is_dm, int i_sent, int *by)
{
    int b = UTM_BY_NONE, seat;
    if (record == UTM_SEAT_O || (record == UTM_SEAT_X && m->sealed)) {
        b = UTM_BY_RECORD;
        seat = seat_of_mark(m, record == UTM_SEAT_X ? UTTT_X : UTTT_O);
    } else if (tag_seat != UTM_SEAT_SPECTATOR && tag_seat != UTM_SEAT_OPEN) {
        b = UTM_BY_TAG;
        seat = tag_seat;
    } else if (is_dm && (i_sent == 0 || i_sent == 1)) {
        /* X plays the odd plies; an invitation (no plies) is O's doing. */
        int last = m->game.n_plies % 2 ? UTTT_X : UTTT_O;
        int other = last == UTTT_X ? UTTT_O : UTTT_X;
        b = UTM_BY_SENDER;
        seat = seat_of_mark(m, i_sent ? last : other);
    } else {
        seat = m->sealed ? UTM_SEAT_SPECTATOR : UTM_SEAT_OPEN;
    }
    if (by) *by = b;
    return seat;
}

/* The record key: the game, and for X the fork. */
static void rec_key(const UtmMsg *m, int x, uint8_t out[UTM_REC_LEN - 1])
{
    static const char salt[] = "uttt.rec.1|";
    uint8_t s[4], d[SHA256_DIGEST_LEN];
    put32(s, m->seed);
    Sha256 c;
    sha256_init(&c);
    sha256_update(&c, salt, sizeof salt - 1);
    sha256_update(&c, s, 4);
    sha256_update(&c, m->o, UTM_TAG_LEN);
    if (x) sha256_update(&c, m->x, UTM_TAG_LEN);
    sha256_final(&c, d);
    memcpy(out, d, UTM_REC_LEN - 1);
}

static int rec_n(int n)
{
    if (n < 0) return 0;
    if (n > UTM_REC_BYTES) n = UTM_REC_BYTES;
    return n - n % UTM_REC_LEN;
}

int utm_rec_find(const uint8_t *recs, int n, const UtmMsg *m)
{
    uint8_t ko[UTM_REC_LEN - 1], kx[UTM_REC_LEN - 1];
    rec_key(m, 0, ko);
    rec_key(m, 1, kx);
    n = recs ? rec_n(n) : 0;
    for (int i = 0; i < n; i += UTM_REC_LEN) {
        const uint8_t *r = recs + i;
        int seat = r[UTM_REC_LEN - 1];
        if (seat == UTM_SEAT_O && !memcmp(r, ko, UTM_REC_LEN - 1)) return UTM_SEAT_O;
        if (seat == UTM_SEAT_X && m->sealed && !memcmp(r, kx, UTM_REC_LEN - 1)) return UTM_SEAT_X;
    }
    return 0;
}

int utm_rec_forget(uint8_t *recs, int n, const UtmMsg *m)
{
    uint8_t ko[UTM_REC_LEN - 1], kx[UTM_REC_LEN - 1];
    rec_key(m, 0, ko);
    rec_key(m, 1, kx);
    n = rec_n(n);
    int w = 0;
    for (int i = 0; i < n; i += UTM_REC_LEN) {
        const uint8_t *r = recs + i;
        int seat = r[UTM_REC_LEN - 1];
        int mine = (seat == UTM_SEAT_O && !memcmp(r, ko, UTM_REC_LEN - 1)) ||
                   (seat == UTM_SEAT_X && m->sealed && !memcmp(r, kx, UTM_REC_LEN - 1));
        if (mine) continue;
        if (w != i) memmove(recs + w, r, UTM_REC_LEN);
        w += UTM_REC_LEN;
    }
    return w;
}

int utm_rec_put(uint8_t *recs, int n, const UtmMsg *m, int seat)
{
    if (seat == UTM_SEAT_WAITING) seat = UTM_SEAT_O;
    n = rec_n(n);
    if (seat != UTM_SEAT_O && !(seat == UTM_SEAT_X && m->sealed)) return n;
    n = utm_rec_forget(recs, n, m);
    if (n == UTM_REC_BYTES) n -= UTM_REC_LEN;          /* the oldest falls off */
    memmove(recs + UTM_REC_LEN, recs, (size_t)n);
    rec_key(m, seat == UTM_SEAT_X, recs);
    recs[UTM_REC_LEN - 1] = (uint8_t)seat;
    return n + UTM_REC_LEN;
}

int utm_can_move(const UtmMsg *m, const uint8_t me[UTM_TAG_LEN])
{
    if (m->game.over) return 0;
    int mark = utm_seat_mark(utm_seat(m, me));
    return mark && m->game.turn == mark;
}

int utm_play(UtmMsg *m, const uint8_t me[UTM_TAG_LEN], int mv)
{
    if (mv < 0 || mv > 80 || !utm_can_move(m, me)) return 0;
    UtmMsg t = *m;
    if (!t.sealed) {
        memcpy(t.x, me, UTM_TAG_LEN);
        t.sealed = 1;
    }
    if (!uttt_play(&t.game, (uint8_t)mv)) return 0;
    *m = t;
    return 1;
}

int utm_undo(UtmMsg *m, const uint8_t me[UTM_TAG_LEN])
{
    int np = m->game.n_plies;
    if (np == 0) return 0;
    /* X plays the even plies. Only my own move comes back: the other
     * player's is in the thread already and is not mine to take. */
    int last = (np - 1) % 2 == 0 ? UTTT_X : UTTT_O;
    if (utm_seat_mark(utm_seat(m, me)) != last) return 0;
    if (!uttt_undo(&m->game)) return 0;
    if (m->game.n_plies == 0) {
        m->sealed = 0;
        memset(m->x, 0, UTM_TAG_LEN);
    }
    return 1;
}

int utm_door(const UtmMsg *m)
{
    return m->game.over ? UTM_DOOR_AGAIN : UTM_DOOR_NONE;
}

/* ------------------------------------------------------- two messages */

int utm_same_game(const UtmMsg *a, const UtmMsg *b)
{
    return a->seed == b->seed && is(a->o, b->o);
}

void utm_join_key(const UtmMsg *m, uint8_t out[32])
{
    static const char salt[] = "uttt.join.1|";
    uint8_t s[4];
    uint8_t first = m->game.n_plies ? m->game.move[0] : 0xff;
    put32(s, m->seed);
    Sha256 c;
    sha256_init(&c);
    sha256_update(&c, salt, sizeof salt - 1);
    sha256_update(&c, s, 4);
    sha256_update(&c, m->x, UTM_TAG_LEN);
    sha256_update(&c, &first, 1);
    sha256_final(&c, out);
}

static int same_moves(const UtttGame *a, const UtttGame *b)
{
    return a->n_plies == b->n_plies &&
           memcmp(a->move, b->move, a->n_plies) == 0;
}

int utm_prefer(const UtmMsg *mine, const UtmMsg *tapped)
{
    if (!utm_same_game(mine, tapped)) return 1;
    if (mine->sealed != tapped->sealed) return mine->sealed ? -1 : 1;
    int a = mine->game.n_plies, b = tapped->game.n_plies;
    if (a != b) return a > b ? -1 : 1;
    int one_roster = !mine->sealed || is(mine->x, tapped->x);
    if (one_roster) return same_moves(&mine->game, &tapped->game) ? 0 : -1;
    /* Two people took the same seat. */
    uint8_t ka[32], kb[32];
    utm_join_key(mine, ka);
    utm_join_key(tapped, kb);
    int c = memcmp(ka, kb, 32);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

int utm_can_replace(const UtmMsg *m, const uint8_t me[UTM_TAG_LEN], int mv)
{
    if (mv < 0 || mv > 80 || m->game.n_plies == 0) return 0;
    if (mv == m->game.move[m->game.n_plies - 1]) return 0;
    UtmMsg c = *m;
    if (!utm_undo(&c, me)) return 0;
    return utm_play(&c, me, mv);
}
