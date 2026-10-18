/* The whole-game envelope. See msg_whole.h. */
#include "msg_whole.h"
#include "../sha256.h"
#include "../b32.h"
#include "../le_bytes.h"
#include "../wire_check/wire_check.h"
#include <string.h>

/* ---- identity ---------------------------------------------------------------- */

static int len_of(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void salted(const char *salt, const uint8_t seed[MSG_WHOLE_SEED_LEN], const uint8_t *more,
                   int more_len, uint8_t out[SHA256_DIGEST_LEN])
{
    Sha256 c;
    sha256_init(&c);
    sha256_update(&c, salt, (size_t)len_of(salt));
    sha256_update(&c, seed, MSG_WHOLE_SEED_LEN);
    if (more && more_len > 0) sha256_update(&c, more, (size_t)more_len);
    sha256_final(&c, out);
}

void msg_whole_tag(const MsgWholeSpec *spec, const uint8_t seed[MSG_WHOLE_SEED_LEN],
                   const uint8_t *id, int id_len, uint8_t out[MSG_SEAT_TAG_LEN])
{
    uint8_t d[SHA256_DIGEST_LEN];
    salted(spec->seat_salt, seed, id, id_len, d);
    memcpy(out, d, MSG_SEAT_TAG_LEN);
}

void msg_whole_game_id(const MsgWholeSpec *spec, const uint8_t seed[MSG_WHOLE_SEED_LEN],
                       uint8_t out[MSG_WHOLE_ID_LEN])
{
    uint8_t d[SHA256_DIGEST_LEN];
    salted(spec->game_salt, seed, 0, 0, d);
    memcpy(out, d, MSG_WHOLE_ID_LEN);
}

/* ---- the roster's own rules ---------------------------------------------------- */

int msg_whole_started(const MsgWhole *w)
{
    return w->phase != MSG_WHOLE_WAITING;
}

static int cap_of(const MsgWholeSpec *spec, const MsgWhole *w)
{
    return w->dm ? MSG_LOBBY_ROSTER_DM_CAP : spec->group_cap;
}

static int seats_ok(const MsgWholeSpec *spec, const MsgWhole *w)
{
    return w->n_seats >= (msg_whole_started(w) ? 2 : 1) && w->n_seats <= cap_of(spec, w);
}

/* The roster tells one story: every name good, no tag and no name twice. */
static int roster_ok(const MsgWhole *w)
{
    for (int s = 0; s < w->n_seats; s++) {
        const MsgSeat *a = &w->seat[s];
        if (msg_seat_name_verdict(a->name, a->name_len) != MSG_SEAT_NAME_OK) return 0;
        for (int t = 0; t < s; t++) {
            if (!memcmp(a->tag, w->seat[t].tag, MSG_SEAT_TAG_LEN)) return 0;
            if (msg_seat_same_name(&w->seat[t], a->name, a->name_len)) return 0;
        }
    }
    return 1;
}

/* A roster's count and its changes agree: joins - leaves is the rows past the
 * creator, and a LEFT flag needs a leave to have happened. A started game
 * keeps the lobby_rev it started from (a start is not a roster change), so
 * this holds for every phase. */
static int lobby_rev_ok(const MsgWhole *w)
{
    int grown = w->n_seats - 1, rev = w->lobby_rev;
    if (rev < grown || (rev - grown) % 2) return 0;
    return !w->left || rev - grown >= 2;
}

/* ---- the lobby ------------------------------------------------------------------ */

int msg_whole_new(MsgWhole *w, const uint8_t seed[MSG_WHOLE_SEED_LEN], int dm,
                  const uint8_t tag[MSG_SEAT_TAG_LEN], const uint8_t *name, int name_len)
{
    if (msg_seat_name_verdict(name, name_len) != MSG_SEAT_NAME_OK) return MSG_WHOLE_EROSTER;
    memset(w, 0, sizeof *w);
    w->phase = MSG_WHOLE_WAITING;
    w->dm = (uint8_t)(dm != 0);
    w->n_seats = 1;
    w->starter = MSG_WHOLE_NO_SEAT;
    memcpy(w->seed, seed, MSG_WHOLE_SEED_LEN);
    memcpy(w->seat[0].tag, tag, MSG_SEAT_TAG_LEN);
    w->seat[0].name_len = (uint8_t)name_len;
    memcpy(w->seat[0].name, name, (size_t)name_len);
    return MSG_WHOLE_EOK;
}

void msg_whole_lobby(const MsgWholeSpec *spec, const MsgWhole *w, MsgLobbyRoster *l)
{
    memset(l, 0, sizeof *l);
    l->n_seats = w->n_seats;
    l->dm = w->dm;
    l->group_cap = spec->group_cap;
    l->started = (uint8_t)msg_whole_started(w);
    l->rev = w->lobby_rev;
    l->newest = (uint8_t)(w->left || msg_whole_started(w) ? MSG_WHOLE_NO_SEAT : w->n_seats - 1);
    for (int s = 0; s < w->n_seats; s++) l->who[s] = (uint16_t)(s + 1);
}

void msg_whole_lobby_pair(const MsgWholeSpec *spec, const MsgWhole *before, const MsgWhole *after,
                          MsgLobbyRoster *lb, MsgLobbyRoster *la)
{
    const uint8_t *tags[2 * MSG_WHOLE_MAX_SEATS];
    int nt = 0;
    msg_whole_lobby(spec, before, lb);
    msg_whole_lobby(spec, after, la);
    const MsgWhole *ws[2] = { before, after };
    MsgLobbyRoster *ls[2] = { lb, la };
    for (int k = 0; k < 2; k++)
        for (int s = 0; s < ws[k]->n_seats; s++) {
            int h = 0;
            while (h < nt && memcmp(tags[h], ws[k]->seat[s].tag, MSG_SEAT_TAG_LEN)) h++;
            if (h == nt) tags[nt++] = ws[k]->seat[s].tag;
            ls[k]->who[s] = (uint16_t)(h + 1);
        }
}

int msg_whole_offered(const MsgWholeSpec *spec, const MsgWhole *w, int seat)
{
    MsgLobbyRoster l;
    msg_whole_lobby(spec, w, &l);
    return msg_lobby_roster_offered(&l, seat);
}

int msg_whole_seat_of_tag(const MsgWhole *w, const uint8_t tag[MSG_SEAT_TAG_LEN])
{
    for (int s = 0; s < w->n_seats; s++)
        if (!memcmp(w->seat[s].tag, tag, MSG_SEAT_TAG_LEN)) return s;
    return -1;
}

int msg_whole_join(const MsgWholeSpec *spec, MsgWhole *w, const uint8_t tag[MSG_SEAT_TAG_LEN],
                   const uint8_t *name, int name_len)
{
    if (msg_seat_name_verdict(name, name_len) != MSG_SEAT_NAME_OK) return MSG_WHOLE_EROSTER;
    if (msg_whole_seat_of_tag(w, tag) >= 0) return MSG_WHOLE_EROSTER;
    for (int s = 0; s < w->n_seats; s++)
        if (msg_seat_same_name(&w->seat[s], name, name_len)) return MSG_WHOLE_EROSTER;
    MsgLobbyRoster l;
    msg_whole_lobby(spec, w, &l);
    if (msg_lobby_roster_offered(&l, -1) != MSG_LOBBY_ROSTER_JOIN) return MSG_WHOLE_EREFUSED;
    int s = msg_lobby_roster_join(&l, MSG_WHOLE_MAX_SEATS + 1);    /* a handle no row holds */
    if (s < 0) return MSG_WHOLE_EREFUSED;
    memcpy(w->seat[s].tag, tag, MSG_SEAT_TAG_LEN);
    w->seat[s].name_len = (uint8_t)name_len;
    memcpy(w->seat[s].name, name, (size_t)name_len);
    w->n_seats = l.n_seats;
    w->lobby_rev = l.rev;
    w->left = 0;
    return s;
}

int msg_whole_leave(const MsgWholeSpec *spec, MsgWhole *w, int seat)
{
    MsgLobbyRoster l;
    msg_whole_lobby(spec, w, &l);
    if (!msg_lobby_roster_leave(&l, seat)) return MSG_WHOLE_EREFUSED;
    for (int s = seat; s + 1 < w->n_seats; s++) w->seat[s] = w->seat[s + 1];
    memset(&w->seat[w->n_seats - 1], 0, sizeof w->seat[0]);
    w->n_seats = l.n_seats;
    w->lobby_rev = l.rev;
    w->left = 1;
    return MSG_WHOLE_EOK;
}

void msg_whole_mark_started(MsgWhole *w, int seat)
{
    w->phase = MSG_WHOLE_LIVE;
    w->starter = (uint8_t)seat;
    w->left = 0;
}

/* ---- the bytes ------------------------------------------------------------------- */

int msg_whole_writable(const MsgWholeSpec *spec, const MsgWhole *w)
{
    if (!seats_ok(spec, w) || !roster_ok(w) || !lobby_rev_ok(w)) return MSG_WHOLE_EROSTER;
    if (msg_whole_started(w) ? w->starter >= w->n_seats : w->starter != MSG_WHOLE_NO_SEAT)
        return MSG_WHOLE_EROSTER;
    return MSG_WHOLE_EOK;
}

int msg_whole_put_head(const MsgWholeSpec *spec, const MsgWhole *w, int phase, int flags,
                       const uint16_t *counters, uint8_t *buf)
{
    const int live = msg_whole_started(w);
    int n = 0;
    buf[n++] = spec->magic;
    buf[n++] = spec->format;
    buf[n++] = (uint8_t)phase;
    buf[n++] = (uint8_t)(flags | (w->dm ? spec->flag_dm : 0) | (!live && w->left ? spec->flag_left : 0));
    memcpy(buf + n, w->seed, MSG_WHOLE_SEED_LEN); n += MSG_WHOLE_SEED_LEN;
    le_put_u16(buf + n, w->lobby_rev); n += 2;
    for (int i = 0; i < spec->counters; i++) { le_put_u16(buf + n, counters[i]); n += 2; }
    buf[n++] = w->n_seats;
    buf[n++] = live ? w->starter : MSG_WHOLE_NO_SEAT;
    for (int s = 0; s < w->n_seats; s++) {
        memcpy(buf + n, w->seat[s].tag, MSG_SEAT_TAG_LEN); n += MSG_SEAT_TAG_LEN;
        buf[n++] = w->seat[s].name_len;
        memcpy(buf + n, w->seat[s].name, w->seat[s].name_len); n += w->seat[s].name_len;
    }
    return n;
}

int msg_whole_seal(uint8_t *buf, int head, int body_len)
{
    wire_check(buf, (size_t)head, buf + head + MSG_WHOLE_CHECK_LEN, (size_t)body_len,
               buf + head, MSG_WHOLE_CHECK_LEN);
    return head + MSG_WHOLE_CHECK_LEN + body_len;
}

int msg_whole_read(const MsgWholeSpec *spec, const uint8_t *in, int n, MsgWhole *w,
                   int *flags_out, uint16_t *counters, int *body_at, int *body_len)
{
    const int k = spec->counters;
    if (!in || n < 2) return MSG_WHOLE_ESHORT;
    if (in[0] != spec->magic) return MSG_WHOLE_EMAGIC;
    if (in[1] != spec->format) return MSG_WHOLE_EFORMAT;
    if (n < MSG_WHOLE_HEAD_LEN(k)) return MSG_WHOLE_ESHORT;
    if (n > spec->max_bytes) return MSG_WHOLE_EGAME;

    memset(w, 0, sizeof *w);
    /* THE PHASE HAS ONE JUDGE: any byte but WAITING is read as started, and
     * a started header must then say exactly what its replay says (LIVE or
     * FINISHED), so an unknown phase is refused by the product's replay. */
    const int phase = in[2], flags = in[3];
    const int live = phase != MSG_WHOLE_WAITING;
    if (flags & ~spec->flags_known) return MSG_WHOLE_EFLAGS;
    if ((flags & spec->flag_left) && live) return MSG_WHOLE_EFLAGS;
    if ((flags & spec->flags_live) && !live) return MSG_WHOLE_EFLAGS;
    w->phase = (uint8_t)phase;
    w->dm = (flags & spec->flag_dm) != 0;
    w->left = (flags & spec->flag_left) != 0;
    memcpy(w->seed, in + 4, MSG_WHOLE_SEED_LEN);
    w->lobby_rev = le_get_u16(in + 36);
    int any = 0;
    for (int i = 0; i < k; i++) { counters[i] = le_get_u16(in + 38 + 2 * i); any |= counters[i]; }
    w->n_seats = in[38 + 2 * k];
    w->starter = in[39 + 2 * k];
    if (!seats_ok(spec, w)) return MSG_WHOLE_EROSTER;

    int at = MSG_WHOLE_HEAD_LEN(k);
    for (int s = 0; s < w->n_seats; s++) {
        if (n - at < MSG_SEAT_TAG_LEN + 1) return MSG_WHOLE_ESHORT;
        memcpy(w->seat[s].tag, in + at, MSG_SEAT_TAG_LEN);
        at += MSG_SEAT_TAG_LEN;
        int len = in[at++];
        if (len < 1 || len > MSG_SEAT_NAME_MAX_BYTES) return MSG_WHOLE_EROSTER;
        if (n - at < len) return MSG_WHOLE_ESHORT;
        w->seat[s].name_len = (uint8_t)len;
        memcpy(w->seat[s].name, in + at, (size_t)len);
        at += len;
    }
    if (n - at < MSG_WHOLE_CHECK_LEN) return MSG_WHOLE_ESHORT;
    const int bat = at + MSG_WHOLE_CHECK_LEN, bn = n - bat;
    uint8_t want[MSG_WHOLE_CHECK_LEN];
    wire_check(in, (size_t)at, in + bat, (size_t)bn, want, MSG_WHOLE_CHECK_LEN);
    if (memcmp(want, in + at, MSG_WHOLE_CHECK_LEN)) return MSG_WHOLE_ECHECK;

    if (!roster_ok(w) || !lobby_rev_ok(w)) return MSG_WHOLE_EROSTER;
    if (!live) {
        if (w->starter != MSG_WHOLE_NO_SEAT) return MSG_WHOLE_EROSTER;
        if (bn != 0 || any) return MSG_WHOLE_EGAME;
    } else {
        if (w->starter >= w->n_seats) return MSG_WHOLE_EROSTER;
        if (bn < 1) return MSG_WHOLE_EGAME;
    }
    *flags_out = flags;
    *body_at = bat;
    *body_len = bn;
    return MSG_WHOLE_EOK;
}

int msg_whole_same(const MsgWhole *a, const MsgWhole *b)
{
    const int live = msg_whole_started(a);
    if (live != msg_whole_started(b) || a->dm != b->dm || a->lobby_rev != b->lobby_rev
        || a->n_seats != b->n_seats || memcmp(a->seed, b->seed, MSG_WHOLE_SEED_LEN))
        return 0;
    if (!live && (a->left != b->left || a->starter != b->starter)) return 0;
    for (int s = 0; s < a->n_seats; s++)
        if (memcmp(a->seat[s].tag, b->seat[s].tag, MSG_SEAT_TAG_LEN)
            || !msg_seat_same_name(&a->seat[s], b->seat[s].name, b->seat[s].name_len))
            return 0;
    return !live || a->starter == b->starter;
}

/* ---- the text ------------------------------------------------------------------- */

int msg_whole_text_put(const uint8_t *bytes, int n, char *out, int cap)
{
    if (n < 0) return n;
    if (cap < 4) return MSG_WHOLE_ECAP;
    memcpy(out, "?m=", 3);
    int w = b32_encode(bytes, n, out + 3, cap - 3);
    if (w < 0) return MSG_WHOLE_ECAP;
    return 3 + w;
}

/* The next `c` at or after `s`, or NULL: strchr, which the freestanding
 * wasm libc does not carry. */
static const char *find(const char *s, char c)
{
    for (; *s; s++) if (*s == c) return s;
    return 0;
}

int msg_whole_text_get(const char *text, char *span, int span_cap, uint8_t *bytes, int cap)
{
    if (!text) return MSG_WHOLE_ETEXT;
    /* Copied out first, because the base32 reader skips what it does not
     * know and would otherwise read on into the next parameter. */
    const char *q = find(text, '?'), *v = 0;
    while (q) {
        if (q[1] == 'm' && q[2] == '=') { v = q + 3; break; }
        q = find(q + 1, '&');
    }
    if (!v) return MSG_WHOLE_ETEXT;
    int k = 0;
    while (v[k] && v[k] != '&' && v[k] != '#') {
        if (k >= span_cap - 1) return MSG_WHOLE_ETEXT;
        span[k] = v[k];
        k++;
    }
    span[k] = 0;
    int n = b32_decode(span, bytes, cap);
    return n <= 0 ? MSG_WHOLE_ETEXT : n;
}

/* ---- two messages ---------------------------------------------------------------- */

int msg_whole_same_game(const MsgWhole *a, const MsgWhole *b)
{
    return memcmp(a->seed, b->seed, MSG_WHOLE_SEED_LEN) == 0;
}

int msg_whole_more(int mine, int tapped)
{
    return mine > tapped ? -1 : mine < tapped ? 1 : 0;
}

int msg_whole_prefer_lobby(const MsgWhole *mine, const MsgWhole *tapped)
{
    int r = msg_whole_more(mine->lobby_rev, tapped->lobby_rev);
    return r ? r : msg_whole_more(mine->n_seats, tapped->n_seats);
}

int msg_whole_prefer_bytes(const uint8_t *mine, int mine_n, const uint8_t *tapped, int tapped_n)
{
    if (mine_n < 0 || tapped_n < 0) return mine_n >= 0 ? -1 : tapped_n >= 0 ? 1 : 0;
    uint8_t da[SHA256_DIGEST_LEN], db[SHA256_DIGEST_LEN];
    sha256(mine, (size_t)mine_n, da);
    sha256(tapped, (size_t)tapped_n, db);
    int c = memcmp(da, db, SHA256_DIGEST_LEN);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

/* ---- the seats --------------------------------------------------------------------- */

int msg_whole_lobby_sender(const MsgWhole *w)
{
    return w->left ? -1 : w->n_seats - 1;
}

int msg_whole_resolve(const MsgWhole *w, int sender, int record, int tag_seat, int is_dm,
                      int i_sent, const uint8_t *name, int name_len, int *by)
{
    return msg_seat_resolve(w->seat, w->n_seats, msg_whole_started(w), sender, record, tag_seat,
                            is_dm, i_sent, name, name_len, by);
}

int msg_whole_rec_n(const MsgWholeSpec *spec, int n)
{
    if (n < 0) return 0;
    if (n > spec->rec_bytes) n = spec->rec_bytes;
    return n - n % spec->rec_len;
}

int msg_whole_rec_at(const MsgWholeSpec *spec, const uint8_t *recs, int n, const MsgWhole *w)
{
    uint8_t id[MSG_WHOLE_ID_LEN];
    msg_whole_game_id(spec, w->seed, id);
    n = recs ? msg_whole_rec_n(spec, n) : 0;
    for (int i = 0; i < n; i += spec->rec_len)
        if (!memcmp(recs + i, id, MSG_WHOLE_ID_LEN)) return i;
    return -1;
}

int msg_whole_rec_find(const MsgWholeSpec *spec, const uint8_t *recs, int n, const MsgWhole *w)
{
    uint8_t id[MSG_WHOLE_ID_LEN];
    msg_whole_game_id(spec, w->seed, id);
    n = recs ? msg_whole_rec_n(spec, n) : 0;
    int gone = 0;
    for (int i = 0; i < n; i += spec->rec_len)
        if (!memcmp(recs + i, id, MSG_WHOLE_ID_LEN)) {
            int s = msg_whole_seat_of_tag(w, recs + i + MSG_WHOLE_ID_LEN);
            if (s >= 0) return s;
            gone = 1;
        }
    return gone ? MSG_SEAT_REC_GONE : -1;
}

int msg_whole_rec_forget(const MsgWholeSpec *spec, uint8_t *recs, int n, const MsgWhole *w)
{
    uint8_t id[MSG_WHOLE_ID_LEN];
    msg_whole_game_id(spec, w->seed, id);
    n = msg_whole_rec_n(spec, n);
    int out = 0;
    for (int i = 0; i < n; i += spec->rec_len) {
        if (!memcmp(recs + i, id, MSG_WHOLE_ID_LEN)) continue;
        if (out != i) memcpy(recs + out, recs + i, (size_t)spec->rec_len);   /* out < i: never overlapping */
        out += spec->rec_len;
    }
    return out;
}

/* The bytes a record carries past its id and tag: the product's. */
#define REC_OWN (MSG_WHOLE_ID_LEN + MSG_SEAT_TAG_LEN)
#define REC_OWN_MAX 8

int msg_whole_rec_put(const MsgWholeSpec *spec, uint8_t *recs, int n, const MsgWhole *w, int seat)
{
    const int len = spec->rec_len, own = len - REC_OWN;
    n = msg_whole_rec_n(spec, n);
    if (seat < 0 || seat >= w->n_seats || own < 0 || own > REC_OWN_MAX) return n;
    uint8_t keep[REC_OWN_MAX] = { 0 };
    const int at = msg_whole_rec_at(spec, recs, n, w);
    if (at >= 0 && own > 0) memcpy(keep, recs + at + REC_OWN, (size_t)own);
    n = msg_whole_rec_forget(spec, recs, n, w);
    if (n == spec->rec_bytes) n -= len;                               /* the oldest falls off */
    for (int i = n - 1; i >= 0; i--) recs[i + len] = recs[i];          /* one record down */
    msg_whole_game_id(spec, w->seed, recs);
    memcpy(recs + MSG_WHOLE_ID_LEN, w->seat[seat].tag, MSG_SEAT_TAG_LEN);
    if (own > 0) memcpy(recs + REC_OWN, keep, (size_t)own);
    return n + len;
}

/* ---- a bridge's own state ------------------------------------------------------- */

void msg_whole_bridge_id(MsgWholeBridge *b, const uint8_t *id, int n)
{
    if (!id || n < 0) n = 0;
    if (n > MSG_WHOLE_ID_MAX) n = MSG_WHOLE_ID_MAX;
    if (n) memcpy(b->id, id, (size_t)n);
    b->id_n = n;
}

void msg_whole_bridge_nick(MsgWholeBridge *b, const uint8_t *name, int n)
{
    if (!name || msg_seat_name_verdict(name, n) != MSG_SEAT_NAME_OK) { b->nick_n = 0; return; }
    memcpy(b->nick, name, (size_t)n);
    b->nick_n = n;
}

void msg_whole_bridge_tag(const MsgWholeSpec *spec, const MsgWholeBridge *b,
                          const uint8_t seed[MSG_WHOLE_SEED_LEN], uint8_t tag[MSG_SEAT_TAG_LEN])
{
    msg_whole_tag(spec, seed, b->id, b->id_n, tag);
}

void msg_whole_bridge_sent(const MsgWholeSpec *spec, MsgWholeBridge *b, const char *text,
                           int is_dm, int i_sent)
{
    const int cap = MSG_WHOLE_TEXT_LEN(spec->max_bytes);
    b->sent_set = 0;
    if (!text || (i_sent != 0 && i_sent != 1) || cap > MSG_WHOLE_TEXT_MAX) return;
    int n = 0;
    while (text[n]) if (++n >= cap) return;
    memcpy(b->sent_text, text, (size_t)n + 1);
    b->sent_dm = is_dm != 0;
    b->sent_mine = i_sent;
    b->sent_set = 1;
}

void msg_whole_bridge_sent_of(const MsgWholeBridge *b, const char *text, int *is_dm, int *i_sent)
{
    int same = b->sent_set && text;
    for (int i = 0; same; i++) {
        if (text[i] != b->sent_text[i]) same = 0;
        else if (!text[i]) break;
    }
    *is_dm = same ? b->sent_dm : 0;
    *i_sent = same ? b->sent_mine : MSG_SEAT_SENT_UNKNOWN;
}

int msg_whole_bridge_seat(const MsgWholeSpec *spec, const MsgWholeBridge *b, const MsgWhole *w,
                          int sender, const uint8_t *recs, int rec_n, int is_dm, int i_sent, int *by)
{
    uint8_t tag[MSG_SEAT_TAG_LEN];
    msg_whole_bridge_tag(spec, b, w->seed, tag);
    int rec = msg_whole_rec_find(spec, recs, rec_n, w);
    return msg_whole_resolve(w, sender, rec, msg_whole_seat_of_tag(w, tag), is_dm, i_sent,
                             b->nick, b->nick_n, by);
}

void msg_whole_bridge_names(MsgWholeBridge *b, const MsgWhole *w)
{
    for (int s = 0; s < MSG_WHOLE_MAX_SEATS; s++) {
        int n = s < w->n_seats ? w->seat[s].name_len : 0;
        memcpy(b->name[s], w->seat[s].name, (size_t)n);
        b->name[s][n] = 0;
        b->names[s] = b->name[s];
    }
}
