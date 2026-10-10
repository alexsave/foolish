/* Chui Niu - the envelope. See cn_msg.h. */
#include "cn_msg.h"
#include "../../../shared/c/sha256.h"
#include "../../../shared/c/wire_check/wire_check.h"
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

void cn_tag(const uint8_t seed[32], const uint8_t *id, int id_len, uint8_t out[CN_TAG_LEN])
{
    static const char salt[] = "chuiniu.seat.1|";
    uint8_t d[SHA256_DIGEST_LEN];
    salted(salt, (int)sizeof salt - 1, seed, id, id_len, d);
    memcpy(out, d, CN_TAG_LEN);
}

void cn_game_id(const uint8_t seed[32], uint8_t out[8])
{
    static const char salt[] = "chuiniu.game.1|";
    uint8_t d[SHA256_DIGEST_LEN];
    salted(salt, (int)sizeof salt - 1, seed, 0, 0, d);
    memcpy(out, d, 8);
}

static int cap_of(const CnMsg *m)
{
    return m->dm ? CN_LOBBY_DM_CAP : CN_LOBBY_GROUP_CAP;
}

static int started(const CnMsg *m)
{
    return m->phase != CN_PHASE_WAITING;
}

/* The roster tells one story: every name good, no tag and no name twice. */
static int roster_ok(const CnMsg *m)
{
    for (int s = 0; s < m->n_seats; s++) {
        const CnSeat *a = &m->seat[s];
        if (msg_seat_name_verdict(a->name, a->name_len) != CN_NAME_OK) return 0;
        for (int t = 0; t < s; t++) {
            if (!memcmp(a->tag, m->seat[t].tag, CN_TAG_LEN)) return 0;
            if (msg_seat_same_name(&m->seat[t], a->name, a->name_len)) return 0;
        }
    }
    return 1;
}

/* A roster's count and its changes agree: joins - leaves is the rows past the
 * creator, and a LEFT flag needs a leave to have happened. A started game
 * keeps the lobby_rev it started from (a start is not a roster change), so
 * this holds for every phase. */
static int lobby_rev_ok(const CnMsg *m)
{
    int grown = m->n_seats - 1, rev = m->lobby_rev;
    if (rev < grown || (rev - grown) % 2) return 0;
    return !m->left || rev - grown >= 2;
}

/* ---- the lobby ------------------------------------------------------------------ */

int cn_msg_new(CnMsg *m, const uint8_t seed[32], int dm, const uint8_t tag[CN_TAG_LEN],
               const uint8_t *name, int name_len)
{
    if (msg_seat_name_verdict(name, name_len) != CN_NAME_OK) return CN_EROSTER;
    memset(m, 0, sizeof *m);
    m->phase = CN_PHASE_WAITING;
    m->dm = (uint8_t)(dm != 0);
    m->n_seats = 1;
    m->starter = CN_SEAT_NONE;
    memcpy(m->seed, seed, 32);
    memcpy(m->seat[0].tag, tag, CN_TAG_LEN);
    m->seat[0].name_len = (uint8_t)name_len;
    memcpy(m->seat[0].name, name, (size_t)name_len);
    return CN_EOK;
}

void cn_msg_lobby(const CnMsg *m, CnLobby *l)
{
    memset(l, 0, sizeof *l);
    l->n_seats = m->n_seats;
    l->dm = m->dm;
    l->group_cap = CN_LOBBY_GROUP_CAP;
    l->started = (uint8_t)started(m);
    l->rev = m->lobby_rev;
    l->newest = (uint8_t)(m->left || started(m) ? CN_SEAT_NONE : m->n_seats - 1);
    for (int s = 0; s < m->n_seats; s++) l->who[s] = (uint16_t)(s + 1);
}

int cn_msg_offered(const CnMsg *m, int seat)
{
    CnLobby l;
    cn_msg_lobby(m, &l);
    return msg_lobby_roster_offered(&l, seat);
}

int cn_msg_seat_of_tag(const CnMsg *m, const uint8_t tag[CN_TAG_LEN])
{
    for (int s = 0; s < m->n_seats; s++)
        if (!memcmp(m->seat[s].tag, tag, CN_TAG_LEN)) return s;
    return -1;
}

int cn_msg_join(CnMsg *m, const uint8_t tag[CN_TAG_LEN], const uint8_t *name, int name_len)
{
    if (msg_seat_name_verdict(name, name_len) != CN_NAME_OK) return CN_EROSTER;
    if (cn_msg_seat_of_tag(m, tag) >= 0) return CN_EROSTER;
    for (int s = 0; s < m->n_seats; s++)
        if (msg_seat_same_name(&m->seat[s], name, name_len)) return CN_EROSTER;
    CnLobby l;
    cn_msg_lobby(m, &l);
    if (msg_lobby_roster_offered(&l, -1) != CN_LOBBY_JOIN) return CN_EREFUSED;
    int s = msg_lobby_roster_join(&l, CN_MAX_SEATS + 1);      /* a handle no row holds */
    if (s < 0) return CN_EREFUSED;
    memcpy(m->seat[s].tag, tag, CN_TAG_LEN);
    m->seat[s].name_len = (uint8_t)name_len;
    memcpy(m->seat[s].name, name, (size_t)name_len);
    m->n_seats = l.n_seats;
    m->lobby_rev = l.rev;
    m->left = 0;
    return s;
}

int cn_msg_leave(CnMsg *m, int seat)
{
    CnLobby l;
    cn_msg_lobby(m, &l);
    if (!msg_lobby_roster_leave(&l, seat)) return CN_EREFUSED;
    for (int s = seat; s + 1 < m->n_seats; s++) m->seat[s] = m->seat[s + 1];
    memset(&m->seat[m->n_seats - 1], 0, sizeof m->seat[0]);
    m->n_seats = l.n_seats;
    m->lobby_rev = l.rev;
    m->left = 1;
    return CN_EOK;
}

int cn_msg_start(CnMsg *m, int seat)
{
    CnLobby l;
    cn_msg_lobby(m, &l);
    static CnGame g;
    if (!cn_lobby_start(&l, seat, m->seed, &g)) return CN_EREFUSED;
    m->game = g;
    m->phase = CN_PHASE_LIVE;
    m->starter = (uint8_t)seat;
    m->left = 0;
    return CN_EOK;
}

int cn_msg_join_start(CnMsg *m, const uint8_t tag[CN_TAG_LEN], const uint8_t *name, int name_len)
{
    CnLobby l;
    cn_msg_lobby(m, &l);
    if (!msg_lobby_roster_can_join_and_start(&l)) return CN_EREFUSED;
    static CnMsg t;
    t = *m;
    int s = cn_msg_join(&t, tag, name, name_len);
    if (s < 0) return s;
    int e = cn_msg_start(&t, s);
    if (e) return e;
    *m = t;
    return s;
}

/* Each person is known by their tag across the two rosters: handles come
 * from the union of both, in order of first appearance. */
int cn_msg_plan_lobby(const CnMsg *before, const CnMsg *after, CnEvent *out, int cap)
{
    const uint8_t *tags[2 * CN_MAX_SEATS];
    int nt = 0;
    CnLobby lb, la;
    cn_msg_lobby(before, &lb);
    cn_msg_lobby(after, &la);
    const CnMsg *ms[2] = { before, after };
    CnLobby *ls[2] = { &lb, &la };
    for (int k = 0; k < 2; k++)
        for (int s = 0; s < ms[k]->n_seats; s++) {
            int h = 0;
            while (h < nt && memcmp(tags[h], ms[k]->seat[s].tag, CN_TAG_LEN)) h++;
            if (h == nt) tags[nt++] = ms[k]->seat[s].tag;
            ls[k]->who[s] = (uint16_t)(h + 1);
        }
    return cn_plan_lobby(&lb, &la, out, cap);
}

/* ---- the bytes ------------------------------------------------------------------- */

/* The same message, by everything that is not derived (the phase within a
 * started game): what the encoder is asked to write against what it reads
 * back. */
static int same_msg(const CnMsg *a, const CnMsg *b)
{
    if (started(a) != started(b) || a->dm != b->dm || a->lobby_rev != b->lobby_rev
        || a->n_seats != b->n_seats || memcmp(a->seed, b->seed, 32))
        return 0;
    if (!started(a) && (a->left != b->left || a->starter != b->starter)) return 0;
    for (int s = 0; s < a->n_seats; s++)
        if (memcmp(a->seat[s].tag, b->seat[s].tag, CN_TAG_LEN)
            || !msg_seat_same_name(&a->seat[s], b->seat[s].name, b->seat[s].name_len))
            return 0;
    return !started(a) || (a->starter == b->starter && cn_hash(&a->game) == cn_hash(&b->game));
}

int cn_msg_encode(const CnMsg *m, uint8_t *out, int cap)
{
    static uint8_t buf[CN_MSG_MAX_BYTES];
    static CnMsg back;
    const int live = started(m);
    const CnGame *g = &m->game;
    if (m->n_seats < (live ? 2 : 1) || m->n_seats > cap_of(m)) return CN_EROSTER;
    if (!roster_ok(m) || !lobby_rev_ok(m)) return CN_EROSTER;
    if (live) {
        if (g->n != m->n_seats || m->starter >= m->n_seats) return CN_EROSTER;
    } else {
        if (m->starter != CN_SEAT_NONE) return CN_EROSTER;
    }

    int n = 0;
    buf[n++] = CN_MSG_MAGIC;
    buf[n++] = CN_MSG_FORMAT;
    buf[n++] = (uint8_t)(!live ? CN_PHASE_WAITING : g->phase == CN_PH_OVER ? CN_PHASE_FINISHED : CN_PHASE_LIVE);
    buf[n++] = (uint8_t)((m->dm ? CN_FLAG_DM : 0) | (!live && m->left ? CN_FLAG_LEFT : 0));
    memcpy(buf + n, m->seed, 32); n += 32;
    put16(buf + n, m->lobby_rev); n += 2;
    put16(buf + n, live ? g->hist_n : 0); n += 2;
    buf[n++] = m->n_seats;
    buf[n++] = live ? m->starter : CN_SEAT_NONE;
    for (int s = 0; s < m->n_seats; s++) {
        memcpy(buf + n, m->seat[s].tag, CN_TAG_LEN); n += CN_TAG_LEN;
        buf[n++] = m->seat[s].name_len;
        memcpy(buf + n, m->seat[s].name, m->seat[s].name_len); n += m->seat[s].name_len;
    }
    int head = n;
    n += CN_CHECK_LEN;
    int bn = 0;
    if (live) {
        bn = cn_code_encode(g, buf + n, CN_CODE_MAX);
        if (bn < 0) return CN_EGAME;
    }
    wire_check(buf, (size_t)head, buf + n, (size_t)bn, buf + head, CN_CHECK_LEN);
    n += bn;

    /* WHAT WAS WRITTEN IS READ BACK: a payload this build would refuse, or one
     * that reads as a different game, is never handed to a host. */
    int e = cn_msg_decode(buf, n, &back);
    if (e) return e;
    if (!same_msg(&back, m)) return CN_EGAME;
    if (n > cap) return CN_ECAP;
    memcpy(out, buf, (size_t)n);
    return n;
}

int cn_msg_decode(const uint8_t *in, int n, CnMsg *out)
{
    static CnMsg m;
    if (!in || n < 2) return CN_ESHORT;
    if (in[0] != CN_MSG_MAGIC) return CN_EMAGIC;
    if (in[1] != CN_MSG_FORMAT) return CN_EFORMAT;
    if (n < CN_HEAD_LEN) return CN_ESHORT;
    if (n > CN_MSG_MAX_BYTES) return CN_EGAME;

    memset(&m, 0, sizeof m);
    /* THE PHASE HAS ONE JUDGE: any byte but WAITING is read as started, and
     * a started header must then say exactly what its replay says (LIVE or
     * FINISHED, below), so an unknown phase is refused there. */
    int phase = in[2], flags = in[3];
    const int live = phase != CN_PHASE_WAITING;
    if (flags & ~CN_FLAGS_KNOWN) return CN_EFLAGS;
    if ((flags & CN_FLAG_LEFT) && live) return CN_EFLAGS;
    m.phase = (uint8_t)phase;
    m.dm = (flags & CN_FLAG_DM) != 0;
    m.left = (flags & CN_FLAG_LEFT) != 0;
    memcpy(m.seed, in + 4, 32);
    m.lobby_rev = get16(in + 36);
    int moves = get16(in + 38);
    m.n_seats = in[40];
    m.starter = in[41];
    if (m.n_seats < (live ? 2 : 1) || m.n_seats > cap_of(&m)) return CN_EROSTER;

    int at = CN_HEAD_LEN;
    for (int s = 0; s < m.n_seats; s++) {
        if (n - at < CN_TAG_LEN + 1) return CN_ESHORT;
        memcpy(m.seat[s].tag, in + at, CN_TAG_LEN);
        at += CN_TAG_LEN;
        int len = in[at++];
        if (len < 1 || len > CN_NAME_MAX_BYTES) return CN_EROSTER;
        if (n - at < len) return CN_ESHORT;
        m.seat[s].name_len = (uint8_t)len;
        memcpy(m.seat[s].name, in + at, (size_t)len);
        at += len;
    }
    if (n - at < CN_CHECK_LEN) return CN_ESHORT;
    const uint8_t *body = in + at + CN_CHECK_LEN;
    int bn = n - at - CN_CHECK_LEN;
    uint8_t want[CN_CHECK_LEN];
    wire_check(in, (size_t)at, body, (size_t)bn, want, CN_CHECK_LEN);
    if (memcmp(want, in + at, CN_CHECK_LEN)) return CN_ECHECK;

    if (!roster_ok(&m) || !lobby_rev_ok(&m)) return CN_EROSTER;
    if (!live) {
        if (m.starter != CN_SEAT_NONE) return CN_EROSTER;
        if (bn != 0 || moves != 0) return CN_EGAME;
    } else {
        if (m.starter >= m.n_seats) return CN_EROSTER;
        if (bn < 1) return CN_EGAME;
        if (!cn_code_decode(&m.game, m.seed, m.n_seats, moves, body, bn)) return CN_EGAME;
        /* the header says what the replay says, or nothing is believed */
        if (m.game.hist_n != moves || (m.game.phase == CN_PH_OVER ? CN_PHASE_FINISHED : CN_PHASE_LIVE) != phase)
            return CN_EGAME;
    }
    *out = m;
    return CN_EOK;
}

int cn_msg_text_encode(const CnMsg *m, char *out, int cap)
{
    static uint8_t b[CN_MSG_MAX_BYTES];
    int n = cn_msg_encode(m, b, (int)sizeof b);
    if (n < 0) return n;
    if (cap < 4) return CN_ECAP;
    memcpy(out, "?m=", 3);
    int w = b32_encode(b, n, out + 3, cap - 3);
    if (w < 0) return CN_ECAP;
    return 3 + w;
}

/* The next `c` at or after `s`, or NULL: strchr, which the freestanding
 * wasm libc does not carry. */
static const char *find(const char *s, char c)
{
    for (; *s; s++) if (*s == c) return s;
    return 0;
}

int cn_msg_text_decode(const char *text, CnMsg *out)
{
    static char span[CN_MSG_MAX_TEXT];
    static uint8_t b[CN_MSG_MAX_BYTES];
    if (!text) return CN_ETEXT;
    /* The value of `m` in the query: after "?m=" or "&m=", up to the next '&'
     * or '#'. Copied out first, because the base32 reader skips what it does
     * not know and would otherwise read on into the next parameter. */
    const char *q = find(text, '?'), *v = 0;
    while (q) {
        if (q[1] == 'm' && q[2] == '=') { v = q + 3; break; }
        q = find(q + 1, '&');
    }
    if (!v) return CN_ETEXT;
    int k = 0;
    while (v[k] && v[k] != '&' && v[k] != '#') {
        if (k >= (int)sizeof span - 1) return CN_ETEXT;
        span[k] = v[k];
        k++;
    }
    span[k] = 0;
    int n = b32_decode(span, b, (int)sizeof b);
    if (n <= 0) return CN_ETEXT;
    return cn_msg_decode(b, n, out);
}

/* ---- two messages ---------------------------------------------------------------- */

int cn_msg_same_game(const CnMsg *a, const CnMsg *b)
{
    return memcmp(a->seed, b->seed, 32) == 0;
}

static int digest_of(const CnMsg *m, uint8_t out[SHA256_DIGEST_LEN])
{
    static uint8_t b[CN_MSG_MAX_BYTES];
    int n = cn_msg_encode(m, b, (int)sizeof b);
    if (n < 0) return 0;
    sha256(b, (size_t)n, out);
    return 1;
}

static int cmp_int(int mine, int tapped)      /* the larger wins */
{
    return mine > tapped ? -1 : mine < tapped ? 1 : 0;
}

int cn_msg_prefer(const CnMsg *mine, const CnMsg *tapped)
{
    if (!cn_msg_same_game(mine, tapped)) return 1;                                 /* 1 */
    int r;
    if ((r = cmp_int(started(mine), started(tapped)))) return r;                    /* 2 */
    const int la = started(mine), lb = started(tapped);
    if ((r = cmp_int(la ? mine->game.hist_n : 0, lb ? tapped->game.hist_n : 0))) return r;     /* 3 */
    if ((r = cmp_int(mine->lobby_rev, tapped->lobby_rev))) return r;                /* 4 */
    if ((r = cmp_int(mine->n_seats, tapped->n_seats))) return r;
    uint8_t da[SHA256_DIGEST_LEN], db[SHA256_DIGEST_LEN];                           /* 5 */
    int oa = digest_of(mine, da), ob = digest_of(tapped, db);
    if (!oa || !ob) return oa ? -1 : ob ? 1 : 0;       /* the one that cannot be written loses */
    int c = memcmp(da, db, SHA256_DIGEST_LEN);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

int cn_common_moves(const CnMsg *a, const CnMsg *b)
{
    if (!cn_msg_same_game(a, b) || !started(a) || !started(b) || a->game.n != b->game.n) return 0;
    int most = a->game.hist_n < b->game.hist_n ? a->game.hist_n : b->game.hist_n;
    int k = 0;
    while (k < most && a->game.hist[k].q == b->game.hist[k].q && a->game.hist[k].f == b->game.hist[k].f) k++;
    return k;
}

/* ---- the seats --------------------------------------------------------------------- */

int cn_msg_sender(const CnMsg *m)
{
    if (!started(m)) return m->left ? -1 : m->n_seats - 1;
    const CnGame *g = &m->game;
    if (g->hist_n > 0 && g->last_seat < m->n_seats) return g->last_seat;
    return m->starter < m->n_seats ? m->starter : -1;
}

int cn_msg_resolve(const CnMsg *m, int record, int tag_seat, int is_dm, int i_sent,
                   const uint8_t *name, int name_len, int *by)
{
    return msg_seat_resolve(m->seat, m->n_seats, started(m), cn_msg_sender(m), record, tag_seat,
                            is_dm, i_sent, name, name_len, by);
}

static int rec_n(int n)
{
    if (n < 0) return 0;
    if (n > CN_REC_BYTES) n = CN_REC_BYTES;
    return n - n % CN_REC_LEN;
}

int cn_rec_find(const uint8_t *recs, int n, const CnMsg *m)
{
    uint8_t id[8];
    cn_game_id(m->seed, id);
    n = recs ? rec_n(n) : 0;
    int gone = 0;
    for (int i = 0; i < n; i += CN_REC_LEN)
        if (!memcmp(recs + i, id, 8)) {
            int s = cn_msg_seat_of_tag(m, recs + i + 8);
            if (s >= 0) return s;
            gone = 1;
        }
    return gone ? CN_REC_GONE : -1;
}

int cn_rec_forget(uint8_t *recs, int n, const CnMsg *m)
{
    uint8_t id[8];
    cn_game_id(m->seed, id);
    n = rec_n(n);
    int w = 0;
    for (int i = 0; i < n; i += CN_REC_LEN) {
        if (!memcmp(recs + i, id, 8)) continue;
        if (w != i) memcpy(recs + w, recs + i, CN_REC_LEN);   /* w < i: never overlapping */
        w += CN_REC_LEN;
    }
    return w;
}

int cn_rec_put(uint8_t *recs, int n, const CnMsg *m, int seat)
{
    n = rec_n(n);
    if (seat < 0 || seat >= m->n_seats) return n;
    const int seen = cn_rec_seen(recs, n, m);
    n = cn_rec_forget(recs, n, m);
    if (n == CN_REC_BYTES) n -= CN_REC_LEN;             /* the oldest falls off */
    for (int i = n - 1; i >= 0; i--) recs[i + CN_REC_LEN] = recs[i];   /* one record down */
    cn_game_id(m->seed, recs);
    memcpy(recs + 8, m->seat[seat].tag, CN_TAG_LEN);
    recs[CN_REC_SEEN] = (uint8_t)seen;
    return n + CN_REC_LEN;
}

/* m's game's record, or -1 (one record a game: cn_rec_put forgets first) */
static int rec_at(const uint8_t *recs, int n, const CnMsg *m)
{
    uint8_t id[8];
    cn_game_id(m->seed, id);
    n = recs ? rec_n(n) : 0;
    for (int i = 0; i < n; i += CN_REC_LEN)
        if (!memcmp(recs + i, id, 8)) return i;
    return -1;
}

int cn_rec_seen(const uint8_t *recs, int n, const CnMsg *m)
{
    int i = rec_at(recs, n, m);
    return i < 0 ? 0 : recs[i + CN_REC_SEEN];
}

int cn_rec_see(uint8_t *recs, int n, const CnMsg *m, int seen)
{
    int i = rec_at(recs, n, m);
    if (i < 0 || seen < 1 || seen > 255 || recs[i + CN_REC_SEEN] >= seen) return 0;
    recs[i + CN_REC_SEEN] = (uint8_t)seen;
    return 1;
}

int cn_rec_load(uint8_t *recs, const uint8_t *bytes, int n)
{
    if (!bytes || n < 0) return 0;
    if (n >= CN_REC_MAGIC_LEN && !memcmp(bytes, CN_REC_MAGIC, CN_REC_MAGIC_LEN)) {
        n = rec_n(n - CN_REC_MAGIC_LEN);
        if (n) memcpy(recs, bytes + CN_REC_MAGIC_LEN, (size_t)n);
        return n;
    }
    /* THE FIRST FORM: id and tag, no seen byte; every round reads unseen */
    int w = 0;
    for (int i = 0; i + CN_REC_LEN_V1 <= n && w < CN_REC_BYTES; i += CN_REC_LEN_V1) {
        memcpy(recs + w, bytes + i, CN_REC_LEN_V1);
        recs[w + CN_REC_SEEN] = 0;
        w += CN_REC_LEN;
    }
    return w;
}

int cn_rec_save(const uint8_t *recs, int n, uint8_t *out, int cap)
{
    n = rec_n(n);
    if (!out || cap < CN_REC_MAGIC_LEN + n) return -1;
    memcpy(out, CN_REC_MAGIC, CN_REC_MAGIC_LEN);
    if (n) memcpy(out + CN_REC_MAGIC_LEN, recs, (size_t)n);
    return CN_REC_MAGIC_LEN + n;
}
