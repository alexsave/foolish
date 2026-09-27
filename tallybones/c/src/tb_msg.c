/* Tallybones - the envelope. See tb_msg.h. */
#include "tb_msg.h"
#include "../../../shared/c/sha256.h"
#include "../../../shared/c/b32.h"
#include <string.h>

/* ---- small things ------------------------------------------------------------ */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void salted(const char *salt, int salt_len, const uint8_t seed[32],
                   const uint8_t *more, int more_len, uint8_t out[SHA256_DIGEST_LEN])
{
    Sha256 c;
    sha256_init(&c);
    sha256_update(&c, salt, (size_t)salt_len);
    sha256_update(&c, seed, 32);
    if (more && more_len > 0) sha256_update(&c, more, (size_t)more_len);
    sha256_final(&c, out);
}

void tb_tag(const uint8_t seed[32], const uint8_t *id, int id_len, uint8_t out[TB_TAG_LEN])
{
    static const char salt[] = "tallybones.seat.1|";
    uint8_t d[SHA256_DIGEST_LEN];
    salted(salt, (int)sizeof salt - 1, seed, id, id_len, d);
    memcpy(out, d, TB_TAG_LEN);
}

void tb_game_id(const uint8_t seed[32], uint8_t out[8])
{
    static const char salt[] = "tallybones.game.1|";
    uint8_t d[SHA256_DIGEST_LEN];
    salted(salt, (int)sizeof salt - 1, seed, 0, 0, d);
    memcpy(out, d, 8);
}

/* Strict UTF-8: shortest forms only, no surrogates, nothing past U+10FFFF,
 * and no control characters. The character count, or -1. */
static int utf8_chars(const uint8_t *s, int len)
{
    int chars = 0;
    for (int i = 0; i < len;) {
        uint32_t b = s[i], cp;
        int k;
        if (b < 0x80)      { cp = b; k = 1; }
        else if (b < 0xC2) return -1;
        else if (b < 0xE0) { cp = b & 0x1F; k = 2; }
        else if (b < 0xF0) { cp = b & 0x0F; k = 3; }
        else if (b < 0xF5) { cp = b & 0x07; k = 4; }
        else return -1;
        if (i + k > len) return -1;
        for (int j = 1; j < k; j++) {
            if ((s[i + j] & 0xC0) != 0x80) return -1;
            cp = (cp << 6) | (s[i + j] & 0x3F);
        }
        if ((k == 3 && cp < 0x800) || (k == 4 && (cp < 0x10000 || cp > 0x10FFFF))) return -1;
        if (cp >= 0xD800 && cp <= 0xDFFF) return -1;
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) return -1;
        i += k;
        chars++;
    }
    return chars;
}

int tb_name_verdict(const uint8_t *name, int len)
{
    if (!name || len <= 0) return TB_NAME_EMPTY;
    if (len > TB_NAME_MAX_BYTES) return TB_NAME_TOO_LONG;
    int chars = utf8_chars(name, len);
    if (chars < 0) return TB_NAME_BAD;
    return chars > TB_NAME_MAX_CHARS ? TB_NAME_TOO_LONG : TB_NAME_OK;
}

static int same_name(const TbSeat *s, const uint8_t *name, int len)
{
    return s->name_len == len && len > 0 && memcmp(s->name, name, (size_t)len) == 0;
}

static int cap_of(const TbMsg *m)
{
    return m->dm ? TB_LOBBY_DM_CAP : TB_LOBBY_GROUP_CAP;
}

static int started(const TbMsg *m)
{
    return m->phase != TB_PHASE_WAITING;
}

/* The roster tells one story: every name good, no tag and no name twice. */
static int roster_ok(const TbMsg *m)
{
    for (int s = 0; s < m->n_seats; s++) {
        const TbSeat *a = &m->seat[s];
        if (tb_name_verdict(a->name, a->name_len) != TB_NAME_OK) return 0;
        for (int t = 0; t < s; t++) {
            if (!memcmp(a->tag, m->seat[t].tag, TB_TAG_LEN)) return 0;
            if (same_name(&m->seat[t], a->name, a->name_len)) return 0;
        }
    }
    return 1;
}

/* A roster's count and its changes agree: joins - leaves is the rows past the
 * creator, and a LEFT flag needs a leave to have happened. A started game
 * keeps the lobby_rev it started from (a start is not a roster change), so
 * this holds for every phase. */
static int lobby_rev_ok(const TbMsg *m)
{
    int grown = m->n_seats - 1, rev = m->lobby_rev;
    if (rev < grown || (rev - grown) % 2) return 0;
    return !m->left || rev - grown >= 2;
}

/* The moves a game says: its resident history, then its pending move. */
static int bubbles_of(const TbGame *g)
{
    return g->hist_n + (g->draft ? 1 : 0);
}

static TbMove move_at(const TbGame *g, int i)
{
    return i < g->hist_n ? g->hist[i] : g->pending;
}

static int same_moves(const TbGame *a, const TbGame *b)
{
    if (bubbles_of(a) != bubbles_of(b)) return 0;
    for (int i = 0; i < bubbles_of(a); i++) {
        TbMove x = move_at(a, i), y = move_at(b, i);
        if (memcmp(&x, &y, sizeof x)) return 0;
    }
    return 1;
}

/* ---- the lobby ------------------------------------------------------------------ */

int tb_msg_new(TbMsg *m, const uint8_t seed[32], int dm, const uint8_t tag[TB_TAG_LEN],
               const uint8_t *name, int name_len)
{
    if (tb_name_verdict(name, name_len) != TB_NAME_OK) return TB_EROSTER;
    memset(m, 0, sizeof *m);
    m->phase = TB_PHASE_WAITING;
    m->dm = (uint8_t)(dm != 0);
    m->n_seats = 1;
    m->starter = TB_SEAT_NONE;
    memcpy(m->seed, seed, 32);
    memcpy(m->seat[0].tag, tag, TB_TAG_LEN);
    m->seat[0].name_len = (uint8_t)name_len;
    memcpy(m->seat[0].name, name, (size_t)name_len);
    return TB_EOK;
}

void tb_msg_lobby(const TbMsg *m, TbLobby *l)
{
    memset(l, 0, sizeof *l);
    l->n_seats = m->n_seats;
    l->dm = m->dm;
    l->started = (uint8_t)started(m);
    l->rev = m->lobby_rev;
    l->newest = (uint8_t)(m->left || started(m) ? TB_SEAT_NONE : m->n_seats - 1);
    for (int s = 0; s < m->n_seats; s++) l->who[s] = (uint16_t)(s + 1);
}

int tb_msg_offered(const TbMsg *m, int seat)
{
    TbLobby l;
    tb_msg_lobby(m, &l);
    return tb_lobby_offered(&l, seat);
}

int tb_msg_seat_of_tag(const TbMsg *m, const uint8_t tag[TB_TAG_LEN])
{
    for (int s = 0; s < m->n_seats; s++)
        if (!memcmp(m->seat[s].tag, tag, TB_TAG_LEN)) return s;
    return -1;
}

int tb_msg_join(TbMsg *m, const uint8_t tag[TB_TAG_LEN], const uint8_t *name, int name_len)
{
    if (tb_name_verdict(name, name_len) != TB_NAME_OK) return TB_EROSTER;
    if (tb_msg_seat_of_tag(m, tag) >= 0) return TB_EROSTER;
    for (int s = 0; s < m->n_seats; s++)
        if (same_name(&m->seat[s], name, name_len)) return TB_EROSTER;
    TbLobby l;
    tb_msg_lobby(m, &l);
    if (tb_lobby_offered(&l, -1) != TB_LOBBY_JOIN) return TB_EREFUSED;
    int s = tb_lobby_join(&l, TB_MAX_SEATS + 1);      /* a handle no row holds */
    if (s < 0) return TB_EREFUSED;
    memcpy(m->seat[s].tag, tag, TB_TAG_LEN);
    m->seat[s].name_len = (uint8_t)name_len;
    memcpy(m->seat[s].name, name, (size_t)name_len);
    m->n_seats = l.n_seats;
    m->lobby_rev = l.rev;
    m->left = 0;
    return s;
}

int tb_msg_leave(TbMsg *m, int seat)
{
    TbLobby l;
    tb_msg_lobby(m, &l);
    if (!tb_lobby_leave(&l, seat)) return TB_EREFUSED;
    for (int s = seat; s + 1 < m->n_seats; s++) m->seat[s] = m->seat[s + 1];
    memset(&m->seat[m->n_seats - 1], 0, sizeof m->seat[0]);
    m->n_seats = l.n_seats;
    m->lobby_rev = l.rev;
    m->left = 1;
    return TB_EOK;
}

int tb_msg_start(TbMsg *m, int seat)
{
    TbLobby l;
    tb_msg_lobby(m, &l);
    static TbGame g;
    if (!tb_lobby_start(&l, seat, m->seed, &g)) return TB_EREFUSED;
    m->game = g;
    m->phase = TB_PHASE_LIVE;
    m->starter = (uint8_t)seat;
    m->left = 0;
    return TB_EOK;
}

int tb_msg_join_start(TbMsg *m, const uint8_t tag[TB_TAG_LEN], const uint8_t *name, int name_len)
{
    TbLobby l;
    tb_msg_lobby(m, &l);
    if (!tb_lobby_can_join_and_start(&l)) return TB_EREFUSED;
    static TbMsg t;
    t = *m;
    int s = tb_msg_join(&t, tag, name, name_len);
    if (s < 0) return s;
    int e = tb_msg_start(&t, s);
    if (e) return e;
    *m = t;
    return s;
}

/* Each person is known by their tag across the two rosters: handles come
 * from the union of both, in order of first appearance. */
int tb_msg_plan_lobby(const TbMsg *before, const TbMsg *after, TbEvent *out, int cap)
{
    const uint8_t *tags[2 * TB_MAX_SEATS];
    int nt = 0;
    TbLobby lb, la;
    tb_msg_lobby(before, &lb);
    tb_msg_lobby(after, &la);
    const TbMsg *ms[2] = { before, after };
    TbLobby *ls[2] = { &lb, &la };
    for (int k = 0; k < 2; k++)
        for (int s = 0; s < ms[k]->n_seats; s++) {
            int h = 0;
            while (h < nt && memcmp(tags[h], ms[k]->seat[s].tag, TB_TAG_LEN)) h++;
            if (h == nt) tags[nt++] = ms[k]->seat[s].tag;
            ls[k]->who[s] = (uint16_t)(h + 1);
        }
    return tb_plan_lobby(&lb, &la, out, cap);
}

/* ---- the bytes ------------------------------------------------------------------- */

static void check_of(const uint8_t *head, int hn, const uint8_t *body, int bn, uint8_t out[TB_CHECK_LEN])
{
    uint8_t d[SHA256_DIGEST_LEN];
    Sha256 c;
    sha256_init(&c);
    sha256_update(&c, head, (size_t)hn);
    if (bn > 0) sha256_update(&c, body, (size_t)bn);
    sha256_final(&c, d);
    memcpy(out, d, TB_CHECK_LEN);
}

/* The same message, by everything that is not derived (the phase within a
 * started game, the dice): what the encoder is asked to write against what
 * it reads back without deriving. */
static int same_msg(const TbMsg *a, const TbMsg *b)
{
    if (started(a) != started(b) || a->dm != b->dm || a->lobby_rev != b->lobby_rev
        || a->n_seats != b->n_seats || memcmp(a->seed, b->seed, 32))
        return 0;
    if (!started(a) && (a->left != b->left || a->starter != b->starter)) return 0;
    for (int s = 0; s < a->n_seats; s++)
        if (memcmp(a->seat[s].tag, b->seat[s].tag, TB_TAG_LEN)
            || !same_name(&a->seat[s], b->seat[s].name, b->seat[s].name_len))
            return 0;
    return !started(a) || (a->starter == b->starter && same_moves(&a->game, &b->game));
}

static int decode(const uint8_t *in, int n, TbMsg *out, int derive);

int tb_msg_encode(const TbMsg *m, uint8_t *out, int cap)
{
    static uint8_t buf[TB_MSG_MAX_BYTES];
    static TbMsg back;
    const int live = started(m);
    const TbGame *g = &m->game;
    if (m->n_seats < (live ? 2 : 1) || m->n_seats > cap_of(m)) return TB_EROSTER;
    if (!roster_ok(m) || !lobby_rev_ok(m)) return TB_EROSTER;
    if (live) {
        if (g->n != m->n_seats || m->starter >= m->n_seats || g->starter != m->starter) return TB_EROSTER;
    } else {
        if (m->starter != TB_SEAT_NONE) return TB_EROSTER;
    }

    int n = 0;
    buf[n++] = TB_MSG_MAGIC;
    buf[n++] = TB_MSG_FORMAT;
    buf[n++] = (uint8_t)(!live ? TB_PHASE_WAITING : g->over ? TB_PHASE_FINISHED : TB_PHASE_LIVE);
    buf[n++] = (uint8_t)((m->dm ? TB_FLAG_DM : 0) | (!live && m->left ? TB_FLAG_LEFT : 0));
    memcpy(buf + n, m->seed, 32); n += 32;
    put16(buf + n, m->lobby_rev); n += 2;
    put16(buf + n, (uint16_t)(live ? bubbles_of(g) : 0)); n += 2;
    put16(buf + n, live ? g->turns : 0); n += 2;
    buf[n++] = m->n_seats;
    buf[n++] = live ? m->starter : TB_SEAT_NONE;
    for (int s = 0; s < m->n_seats; s++) {
        memcpy(buf + n, m->seat[s].tag, TB_TAG_LEN); n += TB_TAG_LEN;
        buf[n++] = m->seat[s].name_len;
        memcpy(buf + n, m->seat[s].name, m->seat[s].name_len); n += m->seat[s].name_len;
    }
    int head = n;
    n += TB_CHECK_LEN;
    int bn = 0;
    if (live) {
        bn = tb_code_encode(g, buf + n, TB_CODE_MAX);
        if (bn < 0) return TB_EGAME;
    }
    check_of(buf, head, buf + n, bn, buf + head);
    n += bn;

    /* WHAT WAS WRITTEN IS READ BACK: a payload this build would refuse, or one
     * that reads as a different game, is never handed to a host. */
    int e = decode(buf, n, &back, 0);
    if (e) return e;
    if (!same_msg(&back, m)) return TB_EGAME;
    if (n > cap) return TB_ECAP;
    memcpy(out, buf, (size_t)n);
    return n;
}

static int decode(const uint8_t *in, int n, TbMsg *out, int derive)
{
    static TbMsg m;
    if (!in || n < 2) return TB_ESHORT;
    if (in[0] != TB_MSG_MAGIC) return TB_EMAGIC;
    if (in[1] != TB_MSG_FORMAT) return TB_EFORMAT;
    if (n < TB_HEAD_LEN) return TB_ESHORT;
    if (n > TB_MSG_MAX_BYTES) return TB_EGAME;

    memset(&m, 0, sizeof m);
    /* THE PHASE HAS ONE JUDGE: any byte but WAITING is read as started, and
     * a started header must then say exactly what its replay says (LIVE or
     * FINISHED, below), so an unknown phase is refused there. */
    int phase = in[2], flags = in[3];
    const int live = phase != TB_PHASE_WAITING;
    if (flags & ~TB_FLAGS_KNOWN) return TB_EFLAGS;
    if ((flags & TB_FLAG_LEFT) && live) return TB_EFLAGS;
    m.phase = (uint8_t)phase;
    m.dm = (flags & TB_FLAG_DM) != 0;
    m.left = (flags & TB_FLAG_LEFT) != 0;
    memcpy(m.seed, in + 4, 32);
    m.lobby_rev = get16(in + 36);
    int bubbles = get16(in + 38), turns = get16(in + 40);
    m.n_seats = in[42];
    m.starter = in[43];
    if (m.n_seats < (live ? 2 : 1) || m.n_seats > cap_of(&m)) return TB_EROSTER;

    int at = TB_HEAD_LEN;
    for (int s = 0; s < m.n_seats; s++) {
        if (n - at < TB_TAG_LEN + 1) return TB_ESHORT;
        memcpy(m.seat[s].tag, in + at, TB_TAG_LEN);
        at += TB_TAG_LEN;
        int len = in[at++];
        if (len < 1 || len > TB_NAME_MAX_BYTES) return TB_EROSTER;
        if (n - at < len) return TB_ESHORT;
        m.seat[s].name_len = (uint8_t)len;
        memcpy(m.seat[s].name, in + at, (size_t)len);
        at += len;
    }
    if (n - at < TB_CHECK_LEN) return TB_ESHORT;
    const uint8_t *body = in + at + TB_CHECK_LEN;
    int bn = n - at - TB_CHECK_LEN;
    uint8_t want[TB_CHECK_LEN];
    check_of(in, at, body, bn, want);
    if (memcmp(want, in + at, TB_CHECK_LEN)) return TB_ECHECK;

    if (!roster_ok(&m) || !lobby_rev_ok(&m)) return TB_EROSTER;
    if (!live) {
        if (m.starter != TB_SEAT_NONE) return TB_EROSTER;
        if (bn != 0 || bubbles != 0 || turns != 0) return TB_EGAME;
    } else {
        if (m.starter >= m.n_seats) return TB_EROSTER;
        if (bn < 1) return TB_EGAME;
        if (!tb_code_decode(&m.game, m.seed, m.n_seats, m.starter, bubbles, body, bn, derive)) return TB_EGAME;
        /* the header says what the replay says, or nothing is believed */
        if (m.game.turns != turns || m.game.hist_n != bubbles
            || (m.game.over ? TB_PHASE_FINISHED : TB_PHASE_LIVE) != phase)
            return TB_EGAME;
    }
    *out = m;
    return TB_EOK;
}

int tb_msg_decode(const uint8_t *in, int n, TbMsg *out)
{
    return decode(in, n, out, 1);
}

int tb_msg_text_encode(const TbMsg *m, char *out, int cap)
{
    static uint8_t b[TB_MSG_MAX_BYTES];
    int n = tb_msg_encode(m, b, (int)sizeof b);
    if (n < 0) return n;
    if (cap < 4) return TB_ECAP;
    memcpy(out, "?m=", 3);
    int w = b32_encode(b, n, out + 3, cap - 3);
    if (w < 0) return TB_ECAP;
    return 3 + w;
}

/* The next `c` at or after `s`, or NULL: strchr, which the freestanding
 * wasm libc does not carry. */
static const char *find(const char *s, char c)
{
    for (; *s; s++) if (*s == c) return s;
    return 0;
}

static int text_decode(const char *text, TbMsg *out, int derive)
{
    static char span[TB_MSG_MAX_TEXT];
    static uint8_t b[TB_MSG_MAX_BYTES];
    if (!text) return TB_ETEXT;
    /* The value of `m` in the query: after "?m=" or "&m=", up to the next '&'
     * or '#'. Copied out first, because the base32 reader skips what it does
     * not know and would otherwise read on into the next parameter. */
    const char *q = find(text, '?'), *v = 0;
    while (q) {
        if (q[1] == 'm' && q[2] == '=') { v = q + 3; break; }
        q = find(q + 1, '&');
    }
    if (!v) return TB_ETEXT;
    int k = 0;
    while (v[k] && v[k] != '&' && v[k] != '#') {
        if (k >= (int)sizeof span - 1) return TB_ETEXT;
        span[k] = v[k];
        k++;
    }
    span[k] = 0;
    int n = b32_decode(span, b, (int)sizeof b);
    if (n <= 0) return TB_ETEXT;
    return decode(b, n, out, derive);
}

int tb_msg_text_decode(const char *text, TbMsg *out)
{
    return text_decode(text, out, 1);
}

int tb_msg_text_peek(const char *text, TbMsg *out)
{
    return text_decode(text, out, 0);
}

/* ---- two messages ---------------------------------------------------------------- */

int tb_msg_same_game(const TbMsg *a, const TbMsg *b)
{
    return memcmp(a->seed, b->seed, 32) == 0;
}

static int digest_of(const TbMsg *m, uint8_t out[SHA256_DIGEST_LEN])
{
    static uint8_t b[TB_MSG_MAX_BYTES];
    int n = tb_msg_encode(m, b, (int)sizeof b);
    if (n < 0) return 0;
    sha256(b, (size_t)n, out);
    return 1;
}

static int cmp_int(int mine, int tapped)      /* the larger wins */
{
    return mine > tapped ? -1 : mine < tapped ? 1 : 0;
}

int tb_msg_prefer(const TbMsg *mine, const TbMsg *tapped)
{
    if (!tb_msg_same_game(mine, tapped)) return 1;                                 /* 1 */
    int r;
    if ((r = cmp_int(started(mine), started(tapped)))) return r;                    /* 2 */
    const int la = started(mine), lb = started(tapped);
    if ((r = cmp_int(la ? mine->game.turns : 0, lb ? tapped->game.turns : 0))) return r;       /* 3 */
    if ((r = cmp_int(la ? bubbles_of(&mine->game) : 0, lb ? bubbles_of(&tapped->game) : 0))) return r; /* 4 */
    if ((r = cmp_int(mine->lobby_rev, tapped->lobby_rev))) return r;                /* 5 */
    if ((r = cmp_int(mine->n_seats, tapped->n_seats))) return r;
    uint8_t da[SHA256_DIGEST_LEN], db[SHA256_DIGEST_LEN];                           /* 6 */
    int oa = digest_of(mine, da), ob = digest_of(tapped, db);
    if (!oa || !ob) return oa ? -1 : ob ? 1 : 0;       /* the one that cannot be written loses */
    int c = memcmp(da, db, SHA256_DIGEST_LEN);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

int tb_common_bubbles(const TbMsg *a, const TbMsg *b)
{
    if (!tb_msg_same_game(a, b) || !started(a) || !started(b)) return 0;
    if (a->game.n != b->game.n || a->game.starter != b->game.starter) return 0;
    int na = bubbles_of(&a->game), nb = bubbles_of(&b->game), most = na < nb ? na : nb, k = 0;
    while (k < most) {
        TbMove x = move_at(&a->game, k), y = move_at(&b->game, k);
        if (memcmp(&x, &y, sizeof x)) break;
        k++;
    }
    return k;
}

/* ---- the seats --------------------------------------------------------------------- */

int tb_msg_sender(const TbMsg *m)
{
    if (!started(m)) return m->left ? -1 : m->n_seats - 1;
    const TbGame *g = &m->game;
    if (bubbles_of(g) > 0) return move_at(g, bubbles_of(g) - 1).seat;
    return m->starter < m->n_seats ? m->starter : -1;
}

int tb_msg_resolve(const TbMsg *m, int record, int tag_seat, int is_dm, int i_sent,
                   const uint8_t *name, int name_len, int *by)
{
    const int n = m->n_seats;
    int b = TB_BY_NONE, seat = -1;
    if (record >= 0 && record < n) {
        b = TB_BY_RECORD;
        seat = record;
    } else if (tag_seat >= 0 && tag_seat < n) {
        b = TB_BY_TAG;
        seat = tag_seat;
    } else if (record != TB_REC_GONE) {
        /* THE INFERENCES, only for a device with no word on this game: a
         * record whose tag has no row says "not me" (D51), and a namesake who
         * took the freed name, or the next joiner's bubble, cannot overrule it */
        int s = -1, snd = tb_msg_sender(m);
        if (i_sent == 1) s = snd;
        else if (i_sent == 0 && is_dm && n == 2 && snd >= 0) s = 1 - snd;
        /* THE LOBBY GATE: a named device gets a lobby seat by its name or not
         * by inference at all */
        if (s >= 0 && !started(m) && name && name_len > 0 && !same_name(&m->seat[s], name, name_len))
            s = -1;
        if (s >= 0) {
            b = TB_BY_SENDER;
            seat = s;
        } else if (name && name_len > 0) {
            for (int t = 0; t < n; t++)
                if (same_name(&m->seat[t], name, name_len)) { b = TB_BY_NAME; seat = t; break; }
        }
    }
    if (by) *by = b;
    return seat;
}

static int rec_n(int n)
{
    if (n < 0) return 0;
    if (n > TB_REC_BYTES) n = TB_REC_BYTES;
    return n - n % TB_REC_LEN;
}

int tb_rec_find(const uint8_t *recs, int n, const TbMsg *m)
{
    uint8_t id[8];
    tb_game_id(m->seed, id);
    n = recs ? rec_n(n) : 0;
    int gone = 0;
    for (int i = 0; i < n; i += TB_REC_LEN)
        if (!memcmp(recs + i, id, 8)) {
            int s = tb_msg_seat_of_tag(m, recs + i + 8);
            if (s >= 0) return s;
            gone = 1;
        }
    return gone ? TB_REC_GONE : -1;
}

int tb_rec_forget(uint8_t *recs, int n, const TbMsg *m)
{
    uint8_t id[8];
    tb_game_id(m->seed, id);
    n = rec_n(n);
    int w = 0;
    for (int i = 0; i < n; i += TB_REC_LEN) {
        if (!memcmp(recs + i, id, 8)) continue;
        if (w != i) memcpy(recs + w, recs + i, TB_REC_LEN);   /* w < i: never overlapping */
        w += TB_REC_LEN;
    }
    return w;
}

int tb_rec_put(uint8_t *recs, int n, const TbMsg *m, int seat)
{
    n = rec_n(n);
    if (seat < 0 || seat >= m->n_seats) return n;
    n = tb_rec_forget(recs, n, m);
    if (n == TB_REC_BYTES) n -= TB_REC_LEN;             /* the oldest falls off */
    for (int i = n - 1; i >= 0; i--) recs[i + TB_REC_LEN] = recs[i];   /* one record down */
    tb_game_id(m->seed, recs);
    memcpy(recs + 8, m->seat[seat].tag, TB_TAG_LEN);
    return n + TB_REC_LEN;
}
