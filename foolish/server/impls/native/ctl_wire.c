// The control-plane codec - see ctl_wire.h for the frame layout, the string
// shape, and why the control plane stopped being JSON.
#include "ctl_wire.h"

#include <string.h>

// ---------- little-endian readers/writers ----------------------------------
// Explicit, byte at a time, the same way msg_wire.c writes its payloads: the
// wire's endianness must not inherit the host's.

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void wr16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)(v >> 8);
}

static uint64_t rd64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static void wr64(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i));
}

static int32_t rd32s(const unsigned char *p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--) v = (v << 8) | p[i];
    return (int32_t)v;
}

static void wr32s(unsigned char *p, int32_t v) {
    const uint32_t u = (uint32_t)v;   // unsigned shift: defined for a negative gauge
    for (int i = 0; i < 4; i++) p[i] = (unsigned char)(u >> (8 * i));
}

// ---------- a writer that can only run out of room --------------------------
// Every encoder below builds its payload through one of these, so "did it
// fit?" is asked once, at the end, instead of at every field. `ok` latches
// false on the first overflow and nothing is written after that, so a refused
// encode never leaves a half-frame behind for a caller that ignored the -1.

typedef struct {
    unsigned char *buf;
    int cap, len;
    bool ok;
} CtlW;

static void w_init(CtlW *w, unsigned char *buf, int cap) {
    w->buf = buf; w->cap = cap; w->len = 0; w->ok = true;
}

static void w_u8(CtlW *w, unsigned v) {
    if (!w->ok || w->len + 1 > w->cap) { w->ok = false; return; }
    w->buf[w->len++] = (unsigned char)(v & 0xff);
}

static void w_bytes(CtlW *w, const unsigned char *p, int n) {
    if (!w->ok || n < 0 || w->len + n > w->cap) { w->ok = false; return; }
    memcpy(w->buf + w->len, p, (size_t)n);
    w->len += n;
}

// [u8 len][len bytes]. A NULL string encodes as an empty one (the "absent
// key" a JSON body used to express by simply not carrying it); anything
// longer than 255 bytes cannot be expressed by this shape at all, so it is a
// refusal, not a truncation.
static void w_str(CtlW *w, const char *s) {
    const size_t n = s ? strlen(s) : 0;
    if (n > 255) { w->ok = false; return; }
    w_u8(w, (unsigned)n);
    w_bytes(w, (const unsigned char *)s, (int)n);
}

// Stamp the head over the space reserved at w_frame_open and return the whole
// frame's length, or -1 if anything overflowed on the way here.
static int w_frame_close(CtlW *w, int kind) {
    if (!w->ok) return -1;
    const int payload = w->len - CTL_HEAD_LEN;
    if (payload < 0 || payload > 0xffff) return -1;
    w->buf[0] = (unsigned char)CTL_WIRE_VERSION;
    w->buf[1] = (unsigned char)kind;
    wr16(w->buf + 2, (uint16_t)payload);
    return w->len;
}

static void w_frame_open(CtlW *w, unsigned char *buf, int cap) {
    w_init(w, buf, cap);
    if (cap < CTL_HEAD_LEN) { w->ok = false; return; }
    w->len = CTL_HEAD_LEN;   // the head is filled in by w_frame_close
}

// ---------- a reader that refuses rather than clamps ------------------------

typedef struct {
    const unsigned char *p;
    int len, off;
    bool ok;
} CtlR;

// Validates the head and positions the reader at the payload. `want_kind` of
// -1 accepts any kind (ctl_kind's own use).
static bool r_frame_open(CtlR *r, const unsigned char *buf, int len, int want_kind) {
    r->p = buf; r->len = len; r->off = CTL_HEAD_LEN; r->ok = false;
    if (!buf || len < CTL_HEAD_LEN) return false;
    if (buf[0] != CTL_WIRE_VERSION) return false;
    // The declared payload length must be EXACTLY the bytes present. A frame
    // that claims more (truncated in flight) or fewer (trailing junk) is not
    // this frame, and guessing which half to believe is how scrapers rot.
    if ((int)rd16(buf + 2) != len - CTL_HEAD_LEN) return false;
    if (want_kind >= 0 && buf[1] != (unsigned char)want_kind) return false;
    r->ok = true;
    return true;
}

static unsigned r_u8(CtlR *r) {
    if (!r->ok || r->off + 1 > r->len) { r->ok = false; return 0; }
    return r->p[r->off++];
}

// Reads [u8 len][len bytes] into out[0..cap), NUL-terminated. A string that
// would not fit the caller's buffer is a refusal, never a truncation: a
// half-copied token or game id is worse than none, because it looks usable.
static void r_str(CtlR *r, char *out, int cap) {
    const unsigned n = r_u8(r);
    if (!r->ok) return;
    if ((int)n + 1 > cap || r->off + (int)n > r->len) { r->ok = false; return; }
    memcpy(out, r->p + r->off, (size_t)n);
    out[n] = 0;
    r->off += (int)n;
}

static void r_bytes(CtlR *r, unsigned char *out, int n) {
    if (!r->ok || r->off + n > r->len) { r->ok = false; return; }
    memcpy(out, r->p + r->off, (size_t)n);
    r->off += n;
}

// Every field must have been read, and nothing may be left over: a payload
// with trailing bytes is a payload this reader did not understand.
static int r_done(const CtlR *r) { return (r->ok && r->off == r->len) ? 1 : 0; }

// ---------- requests --------------------------------------------------------

int ctl_enc_auth(const char *username, unsigned char *buf, int cap) {
    CtlW w; w_frame_open(&w, buf, cap);
    w_str(&w, username);
    return w_frame_close(&w, CTL_AUTH);
}

int ctl_dec_auth(const unsigned char *buf, int len, char *username_out, int cap) {
    CtlR r;
    if (!r_frame_open(&r, buf, len, CTL_AUTH)) return 0;
    char name[CTL_NAME_CAP];
    r_str(&r, name, (int)sizeof name);
    if (!r_done(&r)) return 0;
    if ((int)strlen(name) + 1 > cap) return 0;
    memcpy(username_out, name, strlen(name) + 1);
    return 1;
}

int ctl_enc_meta(int verb, const char *game_id, const char *strategy, unsigned char *buf, int cap) {
    CtlW w; w_frame_open(&w, buf, cap);
    w_u8(&w, (unsigned)verb);
    w_str(&w, game_id);
    w_str(&w, strategy);
    return w_frame_close(&w, CTL_META);
}

int ctl_dec_meta(const unsigned char *buf, int len, CtlMeta *out) {
    CtlR r;
    if (!r_frame_open(&r, buf, len, CTL_META)) return 0;
    CtlMeta m;
    memset(&m, 0, sizeof m);
    m.verb = (int)r_u8(&r);
    r_str(&r, m.game_id, (int)sizeof m.game_id);
    r_str(&r, m.strategy, (int)sizeof m.strategy);
    if (!r_done(&r)) return 0;
    *out = m;
    return 1;
}

// ---------- responses -------------------------------------------------------

int ctl_enc_session(const char *user_id, const char *username, const char *token,
                    unsigned char *buf, int cap) {
    CtlW w; w_frame_open(&w, buf, cap);
    w_str(&w, user_id);
    w_str(&w, username);
    w_str(&w, token);
    return w_frame_close(&w, CTL_SESSION);
}

int ctl_dec_session(const unsigned char *buf, int len, CtlSession *out) {
    CtlR r;
    if (!r_frame_open(&r, buf, len, CTL_SESSION)) return 0;
    CtlSession s;
    memset(&s, 0, sizeof s);
    r_str(&r, s.user_id, (int)sizeof s.user_id);
    r_str(&r, s.username, (int)sizeof s.username);
    r_str(&r, s.token, (int)sizeof s.token);
    if (!r_done(&r)) return 0;
    *out = s;
    return 1;
}

int ctl_enc_game(const char *game_id, unsigned char *buf, int cap) {
    CtlW w; w_frame_open(&w, buf, cap);
    w_str(&w, game_id);
    return w_frame_close(&w, CTL_GAME);
}

int ctl_dec_game(const unsigned char *buf, int len, char *game_id_out, int cap) {
    CtlR r;
    if (!r_frame_open(&r, buf, len, CTL_GAME)) return 0;
    char gid[CTL_ID_CAP];
    r_str(&r, gid, (int)sizeof gid);
    if (!r_done(&r)) return 0;
    if ((int)strlen(gid) + 1 > cap) return 0;
    memcpy(game_id_out, gid, strlen(gid) + 1);
    return 1;
}

int ctl_enc_lobby(const char *game_id, int status, unsigned char *buf, int cap) {
    CtlW w; w_frame_open(&w, buf, cap);
    w_str(&w, game_id);
    w_u8(&w, (unsigned)status);
    return w_frame_close(&w, CTL_LOBBY);
}

int ctl_dec_lobby(const unsigned char *buf, int len, char *game_id_out, int cap, int *status_out) {
    CtlR r;
    if (!r_frame_open(&r, buf, len, CTL_LOBBY)) return 0;
    char gid[CTL_ID_CAP];
    r_str(&r, gid, (int)sizeof gid);
    const int status = (int)r_u8(&r);
    if (!r_done(&r)) return 0;
    if ((int)strlen(gid) + 1 > cap) return 0;
    memcpy(game_id_out, gid, strlen(gid) + 1);
    if (status_out) *status_out = status;
    return 1;
}

int ctl_enc_applied(bool ok, int status, unsigned char *buf, int cap) {
    CtlW w; w_frame_open(&w, buf, cap);
    w_u8(&w, ok ? 1u : 0u);
    w_u8(&w, (unsigned)status);
    return w_frame_close(&w, CTL_APPLIED);
}

int ctl_dec_applied(const unsigned char *buf, int len, bool *ok_out, int *status_out) {
    CtlR r;
    if (!r_frame_open(&r, buf, len, CTL_APPLIED)) return 0;
    const unsigned ok = r_u8(&r);
    const int status = (int)r_u8(&r);
    if (!r_done(&r)) return 0;
    if (ok_out) *ok_out = ok != 0;
    if (status_out) *status_out = status;
    return 1;
}

// The game status is signed: h_status answers -1 for a game that does not
// exist, which a plain byte would report as 255.
int ctl_enc_status(int status, unsigned char *buf, int cap) {
    CtlW w; w_frame_open(&w, buf, cap);
    w_u8(&w, (unsigned)(int8_t)status);
    return w_frame_close(&w, CTL_STATUS);
}

int ctl_dec_status(const unsigned char *buf, int len, int *status_out) {
    CtlR r;
    if (!r_frame_open(&r, buf, len, CTL_STATUS)) return 0;
    const int status = (int)(int8_t)r_u8(&r);
    if (!r_done(&r)) return 0;
    if (status_out) *status_out = status;
    return 1;
}

int ctl_enc_stats(const CtlStats *st, unsigned char *buf, int cap) {
    CtlW w; w_frame_open(&w, buf, cap);
    unsigned char body[CTL_STATS_PAYLOAD_LEN];
    wr32s(body +  0, st->live_connections);
    wr32s(body +  4, st->max_connections);
    wr32s(body +  8, st->games);
    wr32s(body + 12, st->games_live);
    wr32s(body + 16, st->free_slots);
    wr32s(body + 20, st->users);
    wr64 (body + 24, st->games_reclaimed);
    wr64 (body + 32, st->moves_applied);
    wr64 (body + 40, st->bot_decisions);
    wr64 (body + 48, st->octogen_decisions);
    w_bytes(&w, body, (int)sizeof body);
    return w_frame_close(&w, CTL_STATS);
}

int ctl_dec_stats(const unsigned char *buf, int len, CtlStats *out) {
    CtlR r;
    if (!r_frame_open(&r, buf, len, CTL_STATS)) return 0;
    unsigned char body[CTL_STATS_PAYLOAD_LEN];
    r_bytes(&r, body, (int)sizeof body);
    if (!r_done(&r)) return 0;
    CtlStats s;
    s.live_connections  = rd32s(body +  0);
    s.max_connections   = rd32s(body +  4);
    s.games             = rd32s(body +  8);
    s.games_live        = rd32s(body + 12);
    s.free_slots        = rd32s(body + 16);
    s.users             = rd32s(body + 20);
    s.games_reclaimed   = rd64 (body + 24);
    s.moves_applied     = rd64 (body + 32);
    s.bot_decisions     = rd64 (body + 40);
    s.octogen_decisions = rd64 (body + 48);
    *out = s;
    return 1;
}

int ctl_enc_health(unsigned char *buf, int cap) {
    CtlW w; w_frame_open(&w, buf, cap);
    return w_frame_close(&w, CTL_HEALTH);
}

int ctl_enc_error(int reason, unsigned char *buf, int cap) {
    CtlW w; w_frame_open(&w, buf, cap);
    w_u8(&w, (unsigned)reason);
    return w_frame_close(&w, CTL_ERROR);
}

int ctl_dec_error(const unsigned char *buf, int len, int *reason_out) {
    CtlR r;
    if (!r_frame_open(&r, buf, len, CTL_ERROR)) return 0;
    const int reason = (int)r_u8(&r);
    if (!r_done(&r)) return 0;
    if (reason_out) *reason_out = reason;
    return 1;
}

int ctl_kind(const unsigned char *buf, int len) {
    CtlR r;
    if (!r_frame_open(&r, buf, len, -1)) return -1;
    return (int)buf[1];
}
