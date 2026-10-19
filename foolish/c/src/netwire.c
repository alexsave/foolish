// netwire.c - the backend's binary surface, both directions (netwire.h).
#include "netwire.h"
#include "../../../shared/c/le_bytes.h"
#include <string.h>

// ---- a bounded writer and reader ---------------------------------------------------

typedef struct {
    uint8_t *p;
    int      cap, n;
    int      err;      // NW_E_CAP once a put did not fit
} wr;

static void w_bytes(wr *w, const void *src, int n) {
    if (w->err) return;
    if (n < 0 || n > w->cap - w->n) { w->err = NW_E_CAP; return; }
    if (n) memcpy(w->p + w->n, src, (size_t)n);
    w->n += n;
}
static void w_u8(wr *w, uint8_t v) { w_bytes(w, &v, 1); }
static void w_u16(wr *w, uint16_t v) { uint8_t b[2]; le_put_u16(b, v); w_bytes(w, b, 2); }
static void w_u32(wr *w, uint32_t v) { uint8_t b[4]; le_put_u32(b, v); w_bytes(w, b, 4); }
static void w_u48(wr *w, uint64_t v) { uint8_t b[6]; le_put_u48(b, v); w_bytes(w, b, 6); }
static void w_str8(wr *w, const char *s, int n) { w_u8(w, (uint8_t)n); w_bytes(w, s, n); }
static void w_str16(wr *w, const char *s, int n) { w_u16(w, (uint16_t)n); w_bytes(w, s, n); }
static void w_blob(wr *w, NwBlob b) { w_u32(w, b.len); w_bytes(w, b.p, (int)b.len); }
static int w_done(const wr *w) { return w->err ? w->err : w->n; }

typedef struct {
    const uint8_t *p;
    int            len, at;
    int            bad;
} rd;

static const uint8_t *r_take(rd *r, int n) {
    if (r->bad || n < 0 || n > r->len - r->at) { r->bad = 1; return NULL; }   // never r->at + n: it can overflow
    const uint8_t *q = r->p + r->at;
    r->at += n;
    return q;
}
static uint8_t r_u8(rd *r) { const uint8_t *q = r_take(r, 1); return q ? q[0] : 0; }
static uint16_t r_u16(rd *r) { const uint8_t *q = r_take(r, 2); return q ? le_get_u16(q) : 0; }
static uint32_t r_u32(rd *r) { const uint8_t *q = r_take(r, 4); return q ? le_get_u32(q) : 0; }
static uint64_t r_u48(rd *r) { const uint8_t *q = r_take(r, 6); return q ? le_get_u48(q) : 0; }
// A string into a fixed field of `cap` bytes; longer is malformed.
static void r_strn(rd *r, int n, char *dst, int cap, void *len_out, int len_bytes) {
    if (n > cap) { r->bad = 1; return; }
    const uint8_t *q = r_take(r, n);
    if (!q) return;
    memcpy(dst, q, (size_t)n);
    if (len_bytes == 1) *(uint8_t *)len_out = (uint8_t)n;
    else *(uint16_t *)len_out = (uint16_t)n;
}
static void r_str8(rd *r, char *dst, int cap, uint8_t *len_out) { r_strn(r, r_u8(r), dst, cap, len_out, 1); }
static void r_str16(rd *r, char *dst, int cap, uint16_t *len_out) { r_strn(r, r_u16(r), dst, cap, len_out, 2); }
static void r_blob(rd *r, NwBlob *b) {
    uint32_t n = r_u32(r);
    // A length past INT_MAX turns negative here, which r_take refuses with any
    // other length the body cannot hold.
    b->p = r_take(r, (int)n);
    b->len = b->p ? n : 0;
}
// Opens a body of `kind`.
static void r_head(rd *r, const uint8_t *p, int len, uint8_t kind) {
    r->p = p;
    r->len = p ? len : 0;
    r->at = 0;
    r->bad = 0;
    if (r_u8(r) != NW_V1 || r_u8(r) != kind) r->bad = 1;
}
// Whole: every byte read and nothing past the end.
static int r_done(const rd *r) { return !r->bad && r->at == r->len ? NW_OK : NW_E_WIRE; }

static int fits8(int n, int cap) { return n >= 0 && n <= cap && n <= 255; }

// ---- requests ----------------------------------------------------------------------

int nw_request_encode(const NwRequest *q, uint8_t *out, int cap) {
    if (!q) return NW_E_WIRE;
    wr w = { out, out ? cap : 0, 0, 0 };
    w_u8(&w, NW_V1);
    w_u8(&w, q->kind);
    switch (q->kind) {
    case NW_SIGNUP:
    case NW_SIGNIN:
        if (!fits8(q->name_len, NW_NAME_MAX) || !fits8(q->password_len, NW_PASSWORD_MAX)) return NW_E_WIRE;
        w_str8(&w, q->name, q->name_len);
        w_str8(&w, q->password, q->password_len);
        break;
    case NW_REFRESH:
        if (!fits8(q->refresh_len, NW_REFRESH_MAX)) return NW_E_WIRE;
        w_str8(&w, q->refresh, q->refresh_len);
        break;
    case NW_SIGNOUT:
        if (q->scope != NW_SCOPE_LOCAL && q->scope != NW_SCOPE_GLOBAL) return NW_E_WIRE;
        w_u8(&w, q->scope);
        break;
    case NW_PASSWORD:
        if (!fits8(q->password_len, NW_PASSWORD_MAX) || !fits8(q->new_password_len, NW_PASSWORD_MAX)) return NW_E_WIRE;
        w_str8(&w, q->password, q->password_len);
        w_str8(&w, q->new_password, q->new_password_len);
        break;
    case NW_CHAT_SEND:
        if (q->message_len > NW_CHAT_MAX) return NW_E_WIRE;
        w_str16(&w, q->message, q->message_len);
        break;
    default:
        return NW_E_WIRE;
    }
    return w_done(&w);
}

int nw_request_decode(const uint8_t *p, int len, NwRequest *out) {
    memset(out, 0, sizeof *out);
    if (!p || len < 2 || p[0] != NW_V1) return NW_E_WIRE;
    rd r;
    r_head(&r, p, len, p[1]);
    out->kind = p[1];
    switch (out->kind) {
    case NW_SIGNUP:
    case NW_SIGNIN:
        r_str8(&r, out->name, NW_NAME_MAX, &out->name_len);
        r_str8(&r, out->password, NW_PASSWORD_MAX, &out->password_len);
        break;
    case NW_REFRESH: r_str8(&r, out->refresh, NW_REFRESH_MAX, &out->refresh_len); break;
    case NW_SIGNOUT:
        out->scope = r_u8(&r);
        if (out->scope != NW_SCOPE_LOCAL && out->scope != NW_SCOPE_GLOBAL) r.bad = 1;
        break;
    case NW_PASSWORD:
        r_str8(&r, out->password, NW_PASSWORD_MAX, &out->password_len);
        r_str8(&r, out->new_password, NW_PASSWORD_MAX, &out->new_password_len);
        break;
    case NW_CHAT_SEND: r_str16(&r, out->message, NW_CHAT_MAX, &out->message_len); break;
    default: r.bad = 1;
    }
    int rc = r_done(&r);
    if (rc != NW_OK) memset(out, 0, sizeof *out);
    return rc;
}

// ---- single answers ------------------------------------------------------------------

int nw_session_encode(const NwSession *s, uint8_t *out, int cap) {
    if (!s || s->access_len > NW_ACCESS_MAX || !fits8(s->refresh_len, NW_REFRESH_MAX) ||
        !fits8(s->user_id_len, NW_ID_MAX) || !fits8(s->username_len, NW_NAME_MAX) || s->expires_at_ms > LE_U48_MAX)
        return NW_E_WIRE;
    wr w = { out, out ? cap : 0, 0, 0 };
    w_u8(&w, NW_V1);
    w_u8(&w, NW_SESSION);
    w_str16(&w, s->access, s->access_len);
    w_str8(&w, s->refresh, s->refresh_len);
    w_u48(&w, s->expires_at_ms);
    w_str8(&w, s->user_id, s->user_id_len);
    w_str8(&w, s->username, s->username_len);
    return w_done(&w);
}

int nw_session_decode(const uint8_t *p, int len, NwSession *out) {
    memset(out, 0, sizeof *out);
    rd r;
    r_head(&r, p, len, NW_SESSION);
    r_str16(&r, out->access, NW_ACCESS_MAX, &out->access_len);
    r_str8(&r, out->refresh, NW_REFRESH_MAX, &out->refresh_len);
    out->expires_at_ms = r_u48(&r);
    r_str8(&r, out->user_id, NW_ID_MAX, &out->user_id_len);
    r_str8(&r, out->username, NW_NAME_MAX, &out->username_len);
    int rc = r_done(&r);
    if (rc != NW_OK) memset(out, 0, sizeof *out);
    return rc;
}

int nw_view_encode(uint32_t version, NwBlob envelope, uint8_t *out, int cap) {
    wr w = { out, out ? cap : 0, 0, 0 };
    w_u8(&w, NW_V1);
    w_u8(&w, NW_VIEW);
    w_u32(&w, version);
    w_blob(&w, envelope);
    return w_done(&w);
}

int nw_view_decode(const uint8_t *p, int len, uint32_t *version, NwBlob *envelope) {
    rd r;
    r_head(&r, p, len, NW_VIEW);
    *version = r_u32(&r);
    r_blob(&r, envelope);
    int rc = r_done(&r);
    if (rc != NW_OK) { *version = 0; envelope->p = NULL; envelope->len = 0; }
    return rc;
}

int nw_replay_encode(NwBlob moves, NwBlob extras, uint8_t *out, int cap) {
    wr w = { out, out ? cap : 0, 0, 0 };
    w_u8(&w, NW_V1);
    w_u8(&w, NW_REPLAY);
    w_blob(&w, moves);
    w_blob(&w, extras);
    return w_done(&w);
}

int nw_replay_decode(const uint8_t *p, int len, NwBlob *moves, NwBlob *extras) {
    rd r;
    r_head(&r, p, len, NW_REPLAY);
    r_blob(&r, moves);
    r_blob(&r, extras);
    int rc = r_done(&r);
    if (rc != NW_OK) { moves->p = extras->p = NULL; moves->len = extras->len = 0; }
    return rc;
}

// ---- lists ---------------------------------------------------------------------------

static int list_kind(uint8_t kind) {
    return kind == NW_GAMES || kind == NW_CHATS || kind == NW_BOTS || kind == NW_BOARD;
}

int nw_list_head(uint8_t kind, int n, uint8_t *out, int cap) {
    if (!list_kind(kind) || n < 0 || n > 0xffff) return NW_E_WIRE;
    wr w = { out, out ? cap : 0, 0, 0 };
    w_u8(&w, NW_V1);
    w_u8(&w, kind);
    w_u16(&w, (uint16_t)n);
    return w_done(&w);
}

int nw_history_head(int has_rating, uint32_t rating, int n, uint8_t *out, int cap) {
    if (n < 0 || n > 0xffff) return NW_E_WIRE;
    wr w = { out, out ? cap : 0, 0, 0 };
    w_u8(&w, NW_V1);
    w_u8(&w, NW_HISTORY);
    w_u8(&w, has_rating ? 1 : 0);
    w_u32(&w, has_rating ? rating : 0);
    w_u16(&w, (uint16_t)n);
    return w_done(&w);
}

int nw_put_game(uint32_t version, NwBlob envelope, uint8_t *out, int cap) {
    wr w = { out, out ? cap : 0, 0, 0 };
    w_u32(&w, version);
    w_blob(&w, envelope);
    return w_done(&w);
}

static void put_chat(wr *w, const NwChat *c) {
    w_str8(w, c->id, c->id_len);
    w_str8(w, c->user_id, c->user_id_len);
    w_u48(w, c->created_ms);
    w_u8(w, c->is_system ? 1 : 0);
    w_str16(w, c->message, c->message_len);
}
static int chat_ok(const NwChat *c) {
    return c && fits8(c->id_len, NW_ID_MAX) && fits8(c->user_id_len, NW_ID_MAX) && c->message_len <= NW_CHAT_MAX &&
           c->created_ms <= LE_U48_MAX;
}

int nw_put_chat(const NwChat *c, uint8_t *out, int cap) {
    if (!chat_ok(c)) return NW_E_WIRE;
    wr w = { out, out ? cap : 0, 0, 0 };
    put_chat(&w, c);
    return w_done(&w);
}

int nw_put_bot(const NwBot *b, uint8_t *out, int cap) {
    if (!b || !fits8(b->id_len, NW_ID_MAX) || !fits8(b->nickname_len, NW_NAME_MAX) || !fits8(b->key_len, NW_KEY_MAX))
        return NW_E_WIRE;
    wr w = { out, out ? cap : 0, 0, 0 };
    w_str8(&w, b->id, b->id_len);
    w_str8(&w, b->nickname, b->nickname_len);
    w_str8(&w, b->key, b->key_len);
    return w_done(&w);
}

int nw_put_board(const NwBoardRow *b, uint8_t *out, int cap) {
    if (!b || !fits8(b->id_len, NW_ID_MAX) || !fits8(b->name_len, NW_NAME_MAX)) return NW_E_WIRE;
    wr w = { out, out ? cap : 0, 0, 0 };
    w_u8(&w, b->is_bot ? 1 : 0);
    w_str8(&w, b->id, b->id_len);
    w_str8(&w, b->name, b->name_len);
    w_u32(&w, b->rating);
    w_u32(&w, b->games);
    return w_done(&w);
}

int nw_put_history(const NwHistoryRow *h, uint8_t *out, int cap) {
    if (!h || !fits8(h->id_len, NW_ID_MAX) || h->n_players > MAX_PLAYERS || h->created_ms > LE_U48_MAX || h->n_names > MAX_PLAYERS) return NW_E_WIRE;
    for (int i = 0; i < h->n_names; i++)
        if (!fits8(h->name_len[i], NW_NAME_MAX)) return NW_E_WIRE;
    wr w = { out, out ? cap : 0, 0, 0 };
    w_str8(&w, h->id, h->id_len);
    w_u48(&w, h->created_ms);
    w_u8(&w, h->n_players);
    w_u8(&w, h->seat);
    w_u8(&w, h->rank);
    w_u8(&w, h->fool);
    w_u32(&w, h->duration_s);
    w_u8(&w, h->n_names);
    for (int i = 0; i < h->n_names; i++) w_str8(&w, h->name[i], h->name_len[i]);
    return w_done(&w);
}

// Row readers: one per kind, reading at the reader's position.
static void get_game(rd *r, uint32_t *version, NwBlob *env) {
    *version = r_u32(r);
    r_blob(r, env);
}
static void get_chat(rd *r, NwChat *c) {
    r_str8(r, c->id, NW_ID_MAX, &c->id_len);
    r_str8(r, c->user_id, NW_ID_MAX, &c->user_id_len);
    c->created_ms = r_u48(r);
    c->is_system = r_u8(r);
    if (c->is_system > 1) r->bad = 1;
    r_str16(r, c->message, NW_CHAT_MAX, &c->message_len);
}
static void get_bot(rd *r, NwBot *b) {
    r_str8(r, b->id, NW_ID_MAX, &b->id_len);
    r_str8(r, b->nickname, NW_NAME_MAX, &b->nickname_len);
    r_str8(r, b->key, NW_KEY_MAX, &b->key_len);
}
static void get_board(rd *r, NwBoardRow *b) {
    b->is_bot = r_u8(r);
    if (b->is_bot > 1) r->bad = 1;
    r_str8(r, b->id, NW_ID_MAX, &b->id_len);
    r_str8(r, b->name, NW_NAME_MAX, &b->name_len);
    b->rating = r_u32(r);
    b->games = r_u32(r);
}
static void get_history(rd *r, NwHistoryRow *h) {
    r_str8(r, h->id, NW_ID_MAX, &h->id_len);
    h->created_ms = r_u48(r);
    h->n_players = r_u8(r);
    if (h->n_players > MAX_PLAYERS) { r->bad = 1; return; }
    h->seat = r_u8(r);
    h->rank = r_u8(r);
    h->fool = r_u8(r);
    h->duration_s = r_u32(r);
    h->n_names = r_u8(r);
    if (h->n_names > MAX_PLAYERS) { r->bad = 1; return; }
    for (int i = 0; i < h->n_names; i++) r_str8(r, h->name[i], NW_NAME_MAX, &h->name_len[i]);
}

// Scratch for skipping rows while walking to row i; each kind's row is read
// into its own and thrown away.
typedef union {
    NwChat       chat;
    NwBot        bot;
    NwBoardRow   board;
    NwHistoryRow hist;
    struct { uint32_t v; NwBlob b; } game;
} any_row;

static void get_any(rd *r, uint8_t kind, any_row *a) {
    switch (kind) {
    case NW_GAMES: get_game(r, &a->game.v, &a->game.b); break;
    case NW_CHATS: get_chat(r, &a->chat); break;
    case NW_BOTS: get_bot(r, &a->bot); break;
    case NW_BOARD: get_board(r, &a->board); break;
    case NW_HISTORY: get_history(r, &a->hist); break;
    default: r->bad = 1;
    }
}

// Opens `kind` and reads its count; for NW_HISTORY the rating too.
static int open_list(rd *r, const uint8_t *p, int len, uint8_t kind, int *has, uint32_t *rating) {
    r_head(r, p, len, kind);
    if (kind == NW_HISTORY) {
        int h = r_u8(r);
        uint32_t v = r_u32(r);
        if (h > 1 || (!h && v)) r->bad = 1;
        if (has) *has = h;
        if (rating) *rating = v;
    } else if (!list_kind(kind)) r->bad = 1;
    int n = r_u16(r);
    return r->bad ? NW_E_WIRE : n;
}

// The whole body walked: exactly n well-formed rows, row `want` (or none for
// -1) left in *keep. The rows are read in order because each one's length is
// its own; a body is checked whole before any row of it is believed.
static int walk(const uint8_t *p, int len, uint8_t kind, int want, any_row *keep) {
    rd r;
    int n = open_list(&r, p, len, kind, NULL, NULL);
    if (n < 0) return n;
    if (want >= n) return NW_E_WIRE;
    any_row tmp;
    for (int i = 0; i < n; i++) {
        any_row *dst = i == want ? keep : &tmp;
        memset(dst, 0, sizeof *dst);
        get_any(&r, kind, dst);
        if (r.bad) return NW_E_WIRE;
    }
    if (r_done(&r) != NW_OK) return NW_E_WIRE;
    return want < 0 ? n : NW_OK;
}

int nw_list_open(const uint8_t *p, int len, uint8_t kind) {
    if (!list_kind(kind) && kind != NW_HISTORY) return NW_E_WIRE;
    any_row a;
    return walk(p, len, kind, -1, &a);
}

int nw_history_rating(const uint8_t *p, int len, int *has_rating, uint32_t *rating) {
    rd r;
    *has_rating = 0;
    *rating = 0;
    int n = open_list(&r, p, len, NW_HISTORY, has_rating, rating);
    if (n < 0) { *has_rating = 0; *rating = 0; }
    return n < 0 ? n : NW_OK;
}

int nw_get_game(const uint8_t *p, int len, int i, uint32_t *version, NwBlob *envelope) {
    any_row a;
    int rc = i < 0 ? NW_E_WIRE : walk(p, len, NW_GAMES, i, &a);
    *version = rc == NW_OK ? a.game.v : 0;
    envelope->p = rc == NW_OK ? a.game.b.p : NULL;
    envelope->len = rc == NW_OK ? a.game.b.len : 0;
    return rc;
}

#define GET_ROW(fn, KIND, T, field)                                    \
    int fn(const uint8_t *p, int len, int i, T *out) {                 \
        any_row a;                                                     \
        int rc = i < 0 ? NW_E_WIRE : walk(p, len, KIND, i, &a);        \
        if (rc == NW_OK) *out = a.field;                               \
        else memset(out, 0, sizeof *out);                              \
        return rc;                                                     \
    }
GET_ROW(nw_get_chat, NW_CHATS, NwChat, chat)
GET_ROW(nw_get_bot, NW_BOTS, NwBot, bot)
GET_ROW(nw_get_board, NW_BOARD, NwBoardRow, board)
GET_ROW(nw_get_history, NW_HISTORY, NwHistoryRow, hist)

// ---- the stream ---------------------------------------------------------------------

int nw_frame_encode(const NwFrame *f, uint8_t *out, int cap) {
    if (!f || !fits8(f->gid_len, NW_GID_MAX)) return NW_E_WIRE;
    wr w = { out, out ? cap : 0, 0, 0 };
    w_u8(&w, NW_V1);
    w_u8(&w, f->kind);
    switch (f->kind) {
    case NW_WS_VIEW:
    case NW_WS_CAST:
        w_str8(&w, f->gid, f->gid_len);
        w_u32(&w, f->version);
        w_blob(&w, f->blob);
        break;
    case NW_WS_CHAT:
        if (!chat_ok(&f->chat)) return NW_E_WIRE;
        w_str8(&w, f->gid, f->gid_len);
        put_chat(&w, &f->chat);
        break;
    default:
        return NW_E_WIRE;
    }
    return w_done(&w);
}

int nw_frame_decode(const uint8_t *p, int len, NwFrame *out) {
    memset(out, 0, sizeof *out);
    if (!p || len < 2 || p[0] != NW_V1) return NW_E_WIRE;
    rd r;
    r_head(&r, p, len, p[1]);
    out->kind = p[1];
    switch (out->kind) {
    case NW_WS_VIEW:
    case NW_WS_CAST:
        r_str8(&r, out->gid, NW_GID_MAX, &out->gid_len);
        out->version = r_u32(&r);
        r_blob(&r, &out->blob);
        break;
    case NW_WS_CHAT:
        r_str8(&r, out->gid, NW_GID_MAX, &out->gid_len);
        get_chat(&r, &out->chat);
        break;
    default: r.bad = 1;
    }
    int rc = r_done(&r);
    if (rc != NW_OK) memset(out, 0, sizeof *out);
    return rc;
}
