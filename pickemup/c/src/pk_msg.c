/* Pick 'Em Up - the envelope. See pk_msg.h. */
#include "pk_msg.h"
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

void pk_tag(const uint8_t seed[32], const uint8_t *id, int id_len, uint8_t out[PK_TAG_LEN])
{
    static const char salt[] = "pickemup.seat.1|";
    uint8_t d[SHA256_DIGEST_LEN];
    salted(salt, (int)sizeof salt - 1, seed, id, id_len, d);
    memcpy(out, d, PK_TAG_LEN);
}

void pk_game_id(const uint8_t seed[32], uint8_t out[8])
{
    static const char salt[] = "pickemup.game.1|";
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

int pk_name_verdict(const uint8_t *name, int len)
{
    if (!name || len <= 0) return PK_NAME_EMPTY;
    if (len > PK_NAME_MAX_BYTES) return PK_NAME_TOO_LONG;
    int chars = utf8_chars(name, len);
    if (chars < 0) return PK_NAME_BAD;
    return chars > PK_NAME_MAX_CHARS ? PK_NAME_TOO_LONG : PK_NAME_OK;
}

static int same_name(const PkSeat *s, const uint8_t *name, int len)
{
    return s->name_len == len && len > 0 && memcmp(s->name, name, (size_t)len) == 0;
}

static int cap_of(const PkMsg *m)
{
    return m->dm ? PK_LOBBY_DM_CAP : PK_LOBBY_GROUP_CAP;
}

static int started(const PkMsg *m)
{
    return m->phase != PK_PHASE_WAITING;
}

/* The roster tells one story: every name good, no tag and no name twice. */
static int roster_ok(const PkMsg *m)
{
    for (int s = 0; s < m->n_seats; s++) {
        const PkSeat *a = &m->seat[s];
        if (pk_name_verdict(a->name, a->name_len) != PK_NAME_OK) return 0;
        for (int t = 0; t < s; t++) {
            if (!memcmp(a->tag, m->seat[t].tag, PK_TAG_LEN)) return 0;
            if (same_name(&m->seat[t], a->name, a->name_len)) return 0;
        }
    }
    return 1;
}

/* A roster's count and its changes agree: joins - leaves is the rows past the
 * creator, and a LEFT flag needs a leave to have happened. A started game
 * keeps the lobby_rev it started from (a start is not a roster change), so
 * this holds for every phase. */
static int lobby_rev_ok(const PkMsg *m)
{
    int grown = m->n_seats - 1, rev = m->lobby_rev;
    if (rev < grown || (rev - grown) % 2) return 0;
    return !m->left || rev - grown >= 2;
}

static int tip_said_of(const PkGame *g)
{
    for (int i = g->hist_n - 1; i >= 0; i--)
        if (g->hist[i].kind == PK_A_BUBBLE) return (g->hist[i].b & PK_BR_SAID) != 0;
    return 0;
}

/* ---- the lobby ------------------------------------------------------------------ */

int pk_msg_new(PkMsg *m, const uint8_t seed[32], int dm, const uint8_t tag[PK_TAG_LEN],
               const uint8_t *name, int name_len)
{
    if (pk_name_verdict(name, name_len) != PK_NAME_OK) return PK_EROSTER;
    memset(m, 0, sizeof *m);
    m->phase = PK_PHASE_WAITING;
    m->dm = (uint8_t)(dm != 0);
    m->n_seats = 1;
    m->starter = PK_SEAT_NONE;
    memcpy(m->seed, seed, 32);
    memcpy(m->seat[0].tag, tag, PK_TAG_LEN);
    m->seat[0].name_len = (uint8_t)name_len;
    memcpy(m->seat[0].name, name, (size_t)name_len);
    return PK_EOK;
}

void pk_msg_lobby(const PkMsg *m, PkLobby *l)
{
    memset(l, 0, sizeof *l);
    l->n_seats = m->n_seats;
    l->dm = m->dm;
    l->started = (uint8_t)started(m);
    l->rev = m->lobby_rev;
    l->newest = (uint8_t)(m->left || started(m) ? PK_SEAT_NONE : m->n_seats - 1);
    for (int s = 0; s < m->n_seats; s++) l->who[s] = (uint16_t)(s + 1);
}

int pk_msg_offered(const PkMsg *m, int seat)
{
    PkLobby l;
    pk_msg_lobby(m, &l);
    return pk_lobby_offered(&l, seat);
}

int pk_msg_seat_of_tag(const PkMsg *m, const uint8_t tag[PK_TAG_LEN])
{
    for (int s = 0; s < m->n_seats; s++)
        if (!memcmp(m->seat[s].tag, tag, PK_TAG_LEN)) return s;
    return -1;
}

int pk_msg_join(PkMsg *m, const uint8_t tag[PK_TAG_LEN], const uint8_t *name, int name_len)
{
    if (pk_name_verdict(name, name_len) != PK_NAME_OK) return PK_EROSTER;
    if (pk_msg_seat_of_tag(m, tag) >= 0) return PK_EROSTER;
    for (int s = 0; s < m->n_seats; s++)
        if (same_name(&m->seat[s], name, name_len)) return PK_EROSTER;
    PkLobby l;
    pk_msg_lobby(m, &l);
    if (pk_lobby_offered(&l, -1) != PK_LOBBY_JOIN) return PK_EREFUSED;
    int s = pk_lobby_join(&l, PK_MAX_SEATS + 1);      /* a handle no row holds */
    if (s < 0) return PK_EREFUSED;
    memcpy(m->seat[s].tag, tag, PK_TAG_LEN);
    m->seat[s].name_len = (uint8_t)name_len;
    memcpy(m->seat[s].name, name, (size_t)name_len);
    m->n_seats = l.n_seats;
    m->lobby_rev = l.rev;
    m->left = 0;
    return s;
}

int pk_msg_leave(PkMsg *m, int seat)
{
    PkLobby l;
    pk_msg_lobby(m, &l);
    if (!pk_lobby_leave(&l, seat)) return PK_EREFUSED;
    for (int s = seat; s + 1 < m->n_seats; s++) m->seat[s] = m->seat[s + 1];
    memset(&m->seat[m->n_seats - 1], 0, sizeof m->seat[0]);
    m->n_seats = l.n_seats;
    m->lobby_rev = l.rev;
    m->left = 1;
    return PK_EOK;
}

int pk_msg_start(PkMsg *m, int seat)
{
    PkLobby l;
    pk_msg_lobby(m, &l);
    static PkGame g;
    if (!pk_lobby_start(&l, seat, m->seed, &g)) return PK_EREFUSED;
    m->game = g;
    m->phase = PK_PHASE_LIVE;
    m->starter = (uint8_t)seat;
    m->left = 0;
    return PK_EOK;
}

int pk_msg_join_start(PkMsg *m, const uint8_t tag[PK_TAG_LEN], const uint8_t *name, int name_len)
{
    PkLobby l;
    pk_msg_lobby(m, &l);
    if (!pk_lobby_can_join_and_start(&l)) return PK_EREFUSED;
    static PkMsg t;
    t = *m;
    int s = pk_msg_join(&t, tag, name, name_len);
    if (s < 0) return s;
    int e = pk_msg_start(&t, s);
    if (e) return e;
    *m = t;
    return s;
}

/* Each person is known by their tag across the two rosters: handles come
 * from the union of both, in order of first appearance. */
int pk_msg_plan_lobby(const PkMsg *before, const PkMsg *after, PkEvent *out, int cap)
{
    const uint8_t *tags[2 * PK_MAX_SEATS];
    int nt = 0;
    PkLobby lb, la;
    pk_msg_lobby(before, &lb);
    pk_msg_lobby(after, &la);
    const PkMsg *ms[2] = { before, after };
    PkLobby *ls[2] = { &lb, &la };
    for (int k = 0; k < 2; k++)
        for (int s = 0; s < ms[k]->n_seats; s++) {
            int h = 0;
            while (h < nt && memcmp(tags[h], ms[k]->seat[s].tag, PK_TAG_LEN)) h++;
            if (h == nt) tags[nt++] = ms[k]->seat[s].tag;
            ls[k]->who[s] = (uint16_t)(h + 1);
        }
    return pk_plan_lobby(&lb, &la, out, cap);
}

/* ---- the bytes ------------------------------------------------------------------- */

static void check_of(const uint8_t *head, int hn, const uint8_t *body, int bn, uint8_t out[PK_CHECK_LEN])
{
    uint8_t d[SHA256_DIGEST_LEN];
    Sha256 c;
    sha256_init(&c);
    sha256_update(&c, head, (size_t)hn);
    if (bn > 0) sha256_update(&c, body, (size_t)bn);
    sha256_final(&c, d);
    memcpy(out, d, PK_CHECK_LEN);
}

/* The same message, by everything that is not derived (phase within a
 * started game, TIP_SAID): what the encoder is asked to write against what
 * it reads back. */
static int same_msg(const PkMsg *a, const PkMsg *b)
{
    if (started(a) != started(b) || a->dm != b->dm || a->lobby_rev != b->lobby_rev
        || a->n_seats != b->n_seats || memcmp(a->seed, b->seed, 32))
        return 0;
    if (!started(a) && (a->left != b->left || a->starter != b->starter)) return 0;
    for (int s = 0; s < a->n_seats; s++)
        if (memcmp(a->seat[s].tag, b->seat[s].tag, PK_TAG_LEN)
            || !same_name(&a->seat[s], b->seat[s].name, b->seat[s].name_len))
            return 0;
    return !started(a) || (a->starter == b->starter && pk_hash(&a->game) == pk_hash(&b->game));
}

int pk_msg_encode(const PkMsg *m, uint8_t *out, int cap)
{
    static uint8_t buf[PK_MSG_MAX_BYTES];
    static PkMsg back;
    const int live = started(m);
    const PkGame *g = &m->game;
    if (m->n_seats < (live ? 2 : 1) || m->n_seats > cap_of(m)) return PK_EROSTER;
    if (!roster_ok(m) || !lobby_rev_ok(m)) return PK_EROSTER;
    if (live) {
        if (g->n != m->n_seats || m->starter >= m->n_seats || g->starter != m->starter) return PK_EROSTER;
        if (g->b_open) return PK_EGAME;
    } else {
        if (m->starter != PK_SEAT_NONE) return PK_EROSTER;
    }

    int n = 0;
    buf[n++] = PK_MSG_MAGIC;
    buf[n++] = PK_MSG_FORMAT;
    buf[n++] = (uint8_t)(!live ? PK_PHASE_WAITING : g->over ? PK_PHASE_FINISHED : PK_PHASE_LIVE);
    buf[n++] = (uint8_t)((m->dm ? PK_FLAG_DM : 0) | (live && tip_said_of(g) ? PK_FLAG_TIP_SAID : 0)
                         | (!live && m->left ? PK_FLAG_LEFT : 0));
    memcpy(buf + n, m->seed, 32); n += 32;
    put16(buf + n, m->lobby_rev); n += 2;
    put16(buf + n, live ? g->bubbles : 0); n += 2;
    put16(buf + n, live ? g->turns : 0); n += 2;
    buf[n++] = m->n_seats;
    buf[n++] = live ? m->starter : PK_SEAT_NONE;
    for (int s = 0; s < m->n_seats; s++) {
        memcpy(buf + n, m->seat[s].tag, PK_TAG_LEN); n += PK_TAG_LEN;
        buf[n++] = m->seat[s].name_len;
        memcpy(buf + n, m->seat[s].name, m->seat[s].name_len); n += m->seat[s].name_len;
    }
    int head = n;
    n += PK_CHECK_LEN;
    int bn = 0;
    if (live) {
        bn = pk_code_encode(g, buf + n, PK_CODE_MAX);
        if (bn < 0) return PK_EGAME;
    }
    check_of(buf, head, buf + n, bn, buf + head);
    n += bn;

    /* WHAT WAS WRITTEN IS READ BACK: a payload this build would refuse, or one
     * that reads as a different game, is never handed to a host. */
    int e = pk_msg_decode(buf, n, &back);
    if (e) return e;
    if (!same_msg(&back, m)) return PK_EGAME;
    if (n > cap) return PK_ECAP;
    memcpy(out, buf, (size_t)n);
    return n;
}

int pk_msg_decode(const uint8_t *in, int n, PkMsg *out)
{
    static PkMsg m;
    if (!in || n < 2) return PK_ESHORT;
    if (in[0] != PK_MSG_MAGIC) return PK_EMAGIC;
    if (in[1] != PK_MSG_FORMAT) return PK_EFORMAT;
    if (n < PK_HEAD_LEN) return PK_ESHORT;
    if (n > PK_MSG_MAX_BYTES) return PK_EGAME;

    memset(&m, 0, sizeof m);
    /* THE PHASE HAS ONE JUDGE: any byte but WAITING is read as started, and
     * a started header must then say exactly what its replay says (LIVE or
     * FINISHED, below), so an unknown phase is refused there. */
    int phase = in[2], flags = in[3];
    const int live = phase != PK_PHASE_WAITING;
    if (flags & ~PK_FLAGS_KNOWN) return PK_EFLAGS;
    if ((flags & PK_FLAG_LEFT) && live) return PK_EFLAGS;
    if ((flags & PK_FLAG_TIP_SAID) && !live) return PK_EFLAGS;
    m.phase = (uint8_t)phase;
    m.dm = (flags & PK_FLAG_DM) != 0;
    m.left = (flags & PK_FLAG_LEFT) != 0;
    m.tip_said = (flags & PK_FLAG_TIP_SAID) != 0;
    memcpy(m.seed, in + 4, 32);
    m.lobby_rev = get16(in + 36);
    int bubbles = get16(in + 38), turns = get16(in + 40);
    m.n_seats = in[42];
    m.starter = in[43];
    if (m.n_seats < (live ? 2 : 1) || m.n_seats > cap_of(&m)) return PK_EROSTER;

    int at = PK_HEAD_LEN;
    for (int s = 0; s < m.n_seats; s++) {
        if (n - at < PK_TAG_LEN + 1) return PK_ESHORT;
        memcpy(m.seat[s].tag, in + at, PK_TAG_LEN);
        at += PK_TAG_LEN;
        int len = in[at++];
        if (len < 1 || len > PK_NAME_MAX_BYTES) return PK_EROSTER;
        if (n - at < len) return PK_ESHORT;
        m.seat[s].name_len = (uint8_t)len;
        memcpy(m.seat[s].name, in + at, (size_t)len);
        at += len;
    }
    if (n - at < PK_CHECK_LEN) return PK_ESHORT;
    const uint8_t *body = in + at + PK_CHECK_LEN;
    int bn = n - at - PK_CHECK_LEN;
    uint8_t want[PK_CHECK_LEN];
    check_of(in, at, body, bn, want);
    if (memcmp(want, in + at, PK_CHECK_LEN)) return PK_ECHECK;

    if (!roster_ok(&m) || !lobby_rev_ok(&m)) return PK_EROSTER;
    if (!live) {
        if (m.starter != PK_SEAT_NONE) return PK_EROSTER;
        if (bn != 0 || bubbles != 0 || turns != 0) return PK_EGAME;
    } else {
        if (m.starter >= m.n_seats) return PK_EROSTER;
        if (bn < 1) return PK_EGAME;
        if (!pk_code_decode(&m.game, m.seed, m.n_seats, m.starter, bubbles, body, bn)) return PK_EGAME;
        /* the header says what the replay says, or nothing is believed */
        if (m.game.turns != turns || m.game.bubbles != bubbles
            || (m.game.over ? PK_PHASE_FINISHED : PK_PHASE_LIVE) != phase
            || tip_said_of(&m.game) != m.tip_said)
            return PK_EGAME;
    }
    *out = m;
    return PK_EOK;
}

int pk_msg_text_encode(const PkMsg *m, char *out, int cap)
{
    static uint8_t b[PK_MSG_MAX_BYTES];
    int n = pk_msg_encode(m, b, (int)sizeof b);
    if (n < 0) return n;
    if (cap < 4) return PK_ECAP;
    memcpy(out, "?m=", 3);
    int w = b32_encode(b, n, out + 3, cap - 3);
    if (w < 0) return PK_ECAP;
    return 3 + w;
}

/* The next `c` at or after `s`, or NULL: strchr, which the freestanding
 * wasm libc does not carry. */
static const char *find(const char *s, char c)
{
    for (; *s; s++) if (*s == c) return s;
    return 0;
}

int pk_msg_text_decode(const char *text, PkMsg *out)
{
    static char span[PK_MSG_MAX_TEXT];
    static uint8_t b[PK_MSG_MAX_BYTES];
    if (!text) return PK_ETEXT;
    /* The value of `m` in the query: after "?m=" or "&m=", up to the next '&'
     * or '#'. Copied out first, because the base32 reader skips what it does
     * not know and would otherwise read on into the next parameter. */
    const char *q = find(text, '?'), *v = 0;
    while (q) {
        if (q[1] == 'm' && q[2] == '=') { v = q + 3; break; }
        q = find(q + 1, '&');
    }
    if (!v) return PK_ETEXT;
    int k = 0;
    while (v[k] && v[k] != '&' && v[k] != '#') {
        if (k >= (int)sizeof span - 1) return PK_ETEXT;
        span[k] = v[k];
        k++;
    }
    span[k] = 0;
    int n = b32_decode(span, b, (int)sizeof b);
    if (n <= 0) return PK_ETEXT;
    return pk_msg_decode(b, n, out);
}

/* ---- two messages ---------------------------------------------------------------- */

int pk_msg_same_game(const PkMsg *a, const PkMsg *b)
{
    return memcmp(a->seed, b->seed, 32) == 0;
}

static int digest_of(const PkMsg *m, uint8_t out[SHA256_DIGEST_LEN])
{
    static uint8_t b[PK_MSG_MAX_BYTES];
    int n = pk_msg_encode(m, b, (int)sizeof b);
    if (n < 0) return 0;
    sha256(b, (size_t)n, out);
    return 1;
}

static int cmp_int(int mine, int tapped)      /* the larger wins */
{
    return mine > tapped ? -1 : mine < tapped ? 1 : 0;
}

int pk_msg_prefer(const PkMsg *mine, const PkMsg *tapped)
{
    if (!pk_msg_same_game(mine, tapped)) return 1;                                 /* 1 */
    int r;
    if ((r = cmp_int(started(mine), started(tapped)))) return r;                    /* 2 */
    const int la = started(mine), lb = started(tapped);
    if ((r = cmp_int(la ? mine->game.turns : 0, lb ? tapped->game.turns : 0))) return r;       /* 3 */
    if ((r = cmp_int(la ? mine->game.bubbles : 0, lb ? tapped->game.bubbles : 0))) return r;   /* 4 */
    if ((r = cmp_int(la && tip_said_of(&mine->game), lb && tip_said_of(&tapped->game)))) return r; /* 5 */
    if ((r = cmp_int(mine->lobby_rev, tapped->lobby_rev))) return r;                /* 6 */
    if ((r = cmp_int(mine->n_seats, tapped->n_seats))) return r;
    uint8_t da[SHA256_DIGEST_LEN], db[SHA256_DIGEST_LEN];                           /* 7 */
    int oa = digest_of(mine, da), ob = digest_of(tapped, db);
    if (!oa || !ob) return oa ? -1 : ob ? 1 : 0;       /* the one that cannot be written loses */
    int c = memcmp(da, db, SHA256_DIGEST_LEN);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

/* Where bubble k (0-based) of `g` ends in hist: the next BUBBLE record, or
 * the end. */
static int bubble_end(const PkGame *g, int k)
{
    int seen = 0;
    for (int i = 0; i < g->hist_n; i++)
        if (g->hist[i].kind == PK_A_BUBBLE && seen++ == k + 1) return i;
    return g->hist_n;
}

int pk_common_bubbles(const PkMsg *a, const PkMsg *b)
{
    if (!pk_msg_same_game(a, b) || !started(a) || !started(b)) return 0;
    if (a->game.n != b->game.n || a->game.starter != b->game.starter) return 0;
    int most = a->game.bubbles < b->game.bubbles ? a->game.bubbles : b->game.bubbles;
    int k = 0;
    while (k < most) {
        int ea = bubble_end(&a->game, k), eb = bubble_end(&b->game, k);
        if (ea != eb || memcmp(a->game.hist, b->game.hist, sizeof(PkAct) * (size_t)ea)) break;
        k++;
    }
    return k;
}

/* ---- the seats --------------------------------------------------------------------- */

int pk_msg_sender(const PkMsg *m)
{
    if (!started(m)) return m->left ? -1 : m->n_seats - 1;
    const PkGame *g = &m->game;
    for (int i = g->hist_n - 1; i >= 0; i--)
        if (g->hist[i].kind == PK_A_BUBBLE && (g->hist[i].b & PK_BR_SEALED)) return g->hist[i].a;
    return m->starter < m->n_seats ? m->starter : -1;
}

int pk_msg_resolve(const PkMsg *m, int record, int tag_seat, int is_dm, int i_sent,
                   const uint8_t *name, int name_len, int *by)
{
    const int n = m->n_seats;
    int b = PK_BY_NONE, seat = -1;
    if (record >= 0 && record < n) {
        b = PK_BY_RECORD;
        seat = record;
    } else if (tag_seat >= 0 && tag_seat < n) {
        b = PK_BY_TAG;
        seat = tag_seat;
    } else if (record != PK_REC_GONE) {
        /* THE INFERENCES, only for a device with no word on this game: a
         * record whose tag has no row says "not me" (D51), and a namesake who
         * took the freed name, or the next joiner's bubble, cannot overrule it */
        int s = -1, snd = pk_msg_sender(m);
        if (i_sent == 1) s = snd;
        else if (i_sent == 0 && is_dm && n == 2 && snd >= 0) s = 1 - snd;
        /* THE LOBBY GATE: a named device gets a lobby seat by its name or not
         * by inference at all */
        if (s >= 0 && !started(m) && name && name_len > 0 && !same_name(&m->seat[s], name, name_len))
            s = -1;
        if (s >= 0) {
            b = PK_BY_SENDER;
            seat = s;
        } else if (name && name_len > 0) {
            for (int t = 0; t < n; t++)
                if (same_name(&m->seat[t], name, name_len)) { b = PK_BY_NAME; seat = t; break; }
        }
    }
    if (by) *by = b;
    return seat;
}

static int rec_n(int n)
{
    if (n < 0) return 0;
    if (n > PK_REC_BYTES) n = PK_REC_BYTES;
    return n - n % PK_REC_LEN;
}

int pk_rec_find(const uint8_t *recs, int n, const PkMsg *m)
{
    uint8_t id[8];
    pk_game_id(m->seed, id);
    n = recs ? rec_n(n) : 0;
    int gone = 0;
    for (int i = 0; i < n; i += PK_REC_LEN)
        if (!memcmp(recs + i, id, 8)) {
            int s = pk_msg_seat_of_tag(m, recs + i + 8);
            if (s >= 0) return s;
            gone = 1;
        }
    return gone ? PK_REC_GONE : -1;
}

int pk_rec_forget(uint8_t *recs, int n, const PkMsg *m)
{
    uint8_t id[8];
    pk_game_id(m->seed, id);
    n = rec_n(n);
    int w = 0;
    for (int i = 0; i < n; i += PK_REC_LEN) {
        if (!memcmp(recs + i, id, 8)) continue;
        if (w != i) memcpy(recs + w, recs + i, PK_REC_LEN);   /* w < i: never overlapping */
        w += PK_REC_LEN;
    }
    return w;
}

int pk_rec_put(uint8_t *recs, int n, const PkMsg *m, int seat)
{
    n = rec_n(n);
    if (seat < 0 || seat >= m->n_seats) return n;
    n = pk_rec_forget(recs, n, m);
    if (n == PK_REC_BYTES) n -= PK_REC_LEN;             /* the oldest falls off */
    for (int i = n - 1; i >= 0; i--) recs[i + PK_REC_LEN] = recs[i];   /* one record down */
    pk_game_id(m->seed, recs);
    memcpy(recs + 8, m->seat[seat].tag, PK_TAG_LEN);
    return n + PK_REC_LEN;
}
