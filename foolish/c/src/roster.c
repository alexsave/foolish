// roster.c - who sits where. See roster.h for both byte layouts.
//
// Written for size as well as clarity: bots.wasm ships to every edge cold start
// and every browser, so the repeated "length byte, then bytes" shapes of both
// layouts go through one helper each way rather than being spelled out per
// field.
#include "roster.h"
#include <string.h>

// Every durable field is a length byte followed by its bytes; these are the
// offsets of the LENGTH bytes (roster.h).
#define TITLE_AT       2
#define SEATS_AT       (TITLE_AT + 1 + ROSTER_TITLE_MAX)
#define SEAT_ID_AT     0
#define SEAT_NAME_AT   (SEAT_ID_AT + 1 + ROSTER_ID_MAX)
#define SEAT_BRAIN_AT  (SEAT_NAME_AT + 1 + ROSTER_NAME_MAX)

_Static_assert(SEATS_AT == 203 && SEAT_NAME_AT == 37 && SEAT_BRAIN_AT == 102, "roster.h offsets");
_Static_assert(SEAT_BRAIN_AT + 1 + ROSTER_BRAIN_MAX + 2 == ROSTER_SEAT_BYTES, "a seat record is 128 bytes");
_Static_assert(SEATS_AT + MAX_PLAYERS * ROSTER_SEAT_BYTES == ROSTER_BYTES, "the roster is 1227 bytes");

// ---------- byte rules --------------------------------------------------------

// Strict UTF-8: no overlong forms, no UTF-16 surrogates, nothing past U+10FFFF.
// TextEncoder and Swift's String.utf8 only ever produce this, so anything else
// is corruption or a hostile writer.
static int utf8_valid(const char *str, int n) {
    const uint8_t *s = (const uint8_t *)str;
    int i = 0;
    while (i < n) {
        // ASCII eight bytes at a time: names, ids and titles are mostly ASCII.
        uint64_t w;
        while (i + 8 <= n && (memcpy(&w, s + i, 8), (w & 0x8080808080808080ull) == 0)) i += 8;
        if (i >= n) break;
        const uint8_t c = s[i];
        int k;
        uint8_t lo = 0x80, hi = 0xbf;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xc2 && c <= 0xdf) k = 1;
        else if (c >= 0xe0 && c <= 0xef) { k = 2; if (c == 0xe0) lo = 0xa0; if (c == 0xed) hi = 0x9f; }
        else if (c >= 0xf0 && c <= 0xf4) { k = 3; if (c == 0xf0) lo = 0x90; if (c == 0xf4) hi = 0x8f; }
        else return 0;
        if (i + k >= n || s[i + 1] < lo || s[i + 1] > hi) return 0;
        for (int j = 2; j <= k; j++) if ((s[i + j] & 0xc0) != 0x80) return 0;
        i += k + 1;
    }
    return 1;
}

// A byte walk, not memcmp: the wasm builds have no libc, only the small one in
// shared/c/wasm, and a byte walk needs nothing from it.
// From the end: ids that differ tend to differ last (a UUID's variant and node,
// a bot id's counter), so two seats' ids part company in the first byte looked at.
static int bytes_eq(const char *a, const char *b, int n) {
    for (int i = n - 1; i >= 0; i--) if (a[i] != b[i]) return 0;
    return 1;
}

static int id_ok(const char *id, int len) {
    if (!id || len <= 0 || len > ROSTER_ID_MAX) return 0;
    // No NUL: in a word, a zero byte is one whose (b - 1) sets the top bit b does not.
    int i = 0;
    for (uint64_t w; i + 8 <= len; i += 8) {
        memcpy(&w, id + i, 8);
        if ((w - 0x0101010101010101ull) & ~w & 0x8080808080808080ull) return 0;
    }
    for (; i < len; i++) if (id[i] == 0) return 0;
    return utf8_valid(id, len);
}

static int brain_ok(const char *b, int len) {
    if (len < 0 || len > ROSTER_BRAIN_MAX || (len > 0 && !b)) return 0;
    for (int i = 0; i < len; i++) if ((uint8_t)b[i] < 0x21 || (uint8_t)b[i] > 0x7e) return 0;
    return 1;
}

// A field into the struct: the bytes, a zero after them, and the length.
static void put_field(char *dst, int cap, uint8_t *len_out, const char *src, int len) {
    memset(dst, 0, (size_t)cap + 1);
    if (len > 0) memcpy(dst, src, (size_t)len);
    *len_out = (uint8_t)len;
}

int roster_name_trim(const char *utf8, int len) {
    if (!utf8 || len <= 0) return 0;
    if (len <= ROSTER_NAME_MAX) return len;
    // The byte at the budget is either the first byte of the scalar that would
    // cross it (cut there) or inside that scalar (back off to its lead byte).
    int k = ROSTER_NAME_MAX;
    while (k > 0 && ((uint8_t)utf8[k] & 0xc0) == 0x80) k--;
    return k;
}

// The length a name is stored at, or -1 when the stored bytes are not UTF-8.
static int name_keep(const char *name, int len) {
    if (len < 0 || (len > 0 && !name)) return -1;
    const int keep = roster_name_trim(name, len);
    return utf8_valid(name, keep) ? keep : -1;
}

// ---------- validation --------------------------------------------------------

int roster_validate(const Roster *r) {
    if (!r || r->n < 0 || r->n > MAX_PLAYERS) return ROSTER_E_COUNT;
    if (r->title_len > ROSTER_TITLE_MAX || !utf8_valid(r->title, r->title_len)) return ROSTER_E_TITLE;
    for (int s = 0; s < r->n; s++) {
        const RosterSeat *x = &r->seats[s];
        if (!id_ok(x->id, x->id_len)) return ROSTER_E_ID;
        if (x->name_len > ROSTER_NAME_MAX || !utf8_valid(x->name, x->name_len)) return ROSTER_E_NAME;
        if (!brain_ok(x->brain, x->brain_len)) return ROSTER_E_BRAIN;
        for (int o = 0; o < s; o++)
            if (r->seats[o].id_len == x->id_len && bytes_eq(r->seats[o].id, x->id, x->id_len))
                return ROSTER_E_DUPLICATE;
    }
    return ROSTER_OK;
}

// ---------- durable encoding --------------------------------------------------

static void emit(uint8_t *at, const char *src, uint8_t len) {
    at[0] = len;
    memcpy(at + 1, src, len);
}

int roster_encode(const Roster *r, uint8_t *out, int cap) {
    const int v = roster_validate(r);
    if (v != ROSTER_OK) return v;
    if (!out || cap < ROSTER_BYTES) return ROSTER_E_CAP;
    memset(out, 0, ROSTER_BYTES);
    out[0] = ROSTER_FORMAT_VERSION;
    out[1] = (uint8_t)r->n;
    emit(out + TITLE_AT, r->title, r->title_len);
    for (int s = 0; s < r->n; s++) {
        const RosterSeat *x = &r->seats[s];
        uint8_t *q = out + SEATS_AT + s * ROSTER_SEAT_BYTES;
        emit(q + SEAT_ID_AT, x->id, x->id_len);
        emit(q + SEAT_NAME_AT, x->name, x->name_len);
        emit(q + SEAT_BRAIN_AT, x->brain, x->brain_len);
    }
    return ROSTER_BYTES;
}

// One durable field into the struct; `e` when its length is over the cap.
static int take(const uint8_t *at, int cap, int e, char *dst, uint8_t *len_out) {
    if (at[0] > cap) return e;
    put_field(dst, cap, len_out, (const char *)at + 1, at[0]);
    return ROSTER_OK;
}

int roster_decode(Roster *r, const uint8_t *p, int len) {
    uint8_t canon[ROSTER_BYTES];
    int v;
    memset(r, 0, sizeof(*r));
    if (!p || len != ROSTER_BYTES) return ROSTER_E_LENGTH;
    if (p[0] != ROSTER_FORMAT_VERSION) return ROSTER_E_VERSION;
    if (p[1] > MAX_PLAYERS) return ROSTER_E_COUNT;
    r->n = (int8_t)p[1];

    // Caps first (they bound the copies), then the rules, then the padding: a
    // field cut mid-sequence is a bad name, not a stray pad byte.
    v = take(p + TITLE_AT, ROSTER_TITLE_MAX, ROSTER_E_TITLE, r->title, &r->title_len);
    for (int s = 0; s < r->n && v == ROSTER_OK; s++) {
        const uint8_t *q = p + SEATS_AT + s * ROSTER_SEAT_BYTES;
        RosterSeat *x = &r->seats[s];
        if ((v = take(q + SEAT_ID_AT, ROSTER_ID_MAX, ROSTER_E_ID, x->id, &x->id_len)) == ROSTER_OK
            && (v = take(q + SEAT_NAME_AT, ROSTER_NAME_MAX, ROSTER_E_NAME, x->name, &x->name_len)) == ROSTER_OK)
            v = take(q + SEAT_BRAIN_AT, ROSTER_BRAIN_MAX, ROSTER_E_BRAIN, x->brain, &x->brain_len);
    }
    // encode validates, then writes the ONE canonical byte string for this
    // roster: every pad, reserved and unused-record byte zero. Anything else in
    // those places is not this roster, so it is refused rather than dropped.
    if (v == ROSTER_OK) {
        v = roster_encode(r, canon, ROSTER_BYTES);
        if (v == ROSTER_BYTES)
            v = bytes_eq((const char *)canon, (const char *)p, ROSTER_BYTES) ? ROSTER_OK : ROSTER_E_PADDING;
    }
    if (v != ROSTER_OK) memset(r, 0, sizeof(*r));
    return v;
}

// ---------- lookups and edits -------------------------------------------------

int roster_seat_of(const Roster *r, const char *id, int id_len) {
    if (!r || !id || id_len <= 0 || id_len > ROSTER_ID_MAX) return -1;
    for (int s = 0; s < r->n && s < MAX_PLAYERS; s++)
        if (r->seats[s].id_len == id_len && bytes_eq(r->seats[s].id, id, id_len)) return s;
    return -1;
}

uint32_t roster_bot_mask(const Roster *r) {
    uint32_t m = 0;
    for (int s = 0; r && s < r->n && s < MAX_PLAYERS; s++)
        if (r->seats[s].brain_len > 0) m |= 1u << s;
    return m;
}

int roster_seat_add(Roster *r, const char *id, int id_len, const char *name, int name_len,
                    const char *brain, int brain_len) {
    if (!id_ok(id, id_len)) return ROSTER_E_ID;
    if (roster_seat_of(r, id, id_len) >= 0) return ROSTER_E_DUPLICATE;
    if (r->n < 0 || r->n >= MAX_PLAYERS) return ROSTER_E_FULL;
    const int keep = name_keep(name, name_len);
    if (keep < 0) return ROSTER_E_NAME;
    if (!brain_ok(brain, brain_len)) return ROSTER_E_BRAIN;
    RosterSeat *x = &r->seats[r->n];
    put_field(x->id, ROSTER_ID_MAX, &x->id_len, id, id_len);
    put_field(x->name, ROSTER_NAME_MAX, &x->name_len, name, keep);
    put_field(x->brain, ROSTER_BRAIN_MAX, &x->brain_len, brain, brain_len);
    return r->n++;
}

int roster_seat_remove(Roster *r, int seat) {
    if (!r || seat < 0 || seat >= r->n || r->n > MAX_PLAYERS) return ROSTER_E_SEAT;
    for (int s = seat; s + 1 < r->n; s++) r->seats[s] = r->seats[s + 1];
    memset(&r->seats[--r->n], 0, sizeof(RosterSeat));
    return ROSTER_OK;
}

int roster_reorder(Roster *r, const int8_t *perm, int n) {
    RosterSeat old[MAX_PLAYERS];
    uint32_t seen = 0;
    if (!r || !perm || n != r->n || n < 0 || n > MAX_PLAYERS) return ROSTER_E_PERM;
    for (int i = 0; i < n; i++) {
        if (perm[i] < 0 || perm[i] >= n || (seen & (1u << perm[i]))) return ROSTER_E_PERM;
        seen |= 1u << perm[i];
    }
    memcpy(old, r->seats, sizeof(old));
    for (int i = 0; i < n; i++) r->seats[i] = old[perm[i]];
    return ROSTER_OK;
}

int roster_set_title(Roster *r, const char *t, int len) {
    if (!r || len < 0 || len > ROSTER_TITLE_MAX || (len > 0 && !t) || !utf8_valid(t, len)) return ROSTER_E_TITLE;
    put_field(r->title, ROSTER_TITLE_MAX, &r->title_len, t, len);
    return ROSTER_OK;
}

int roster_redact(Roster *r, const char *id, int id_len, const char *name, int name_len) {
    const int s = roster_seat_of(r, id, id_len);
    if (s < 0) return ROSTER_E_SEAT;
    const int keep = name_keep(name, name_len);
    if (keep < 0) return ROSTER_E_NAME;
    put_field(r->seats[s].name, ROSTER_NAME_MAX, &r->seats[s].name_len, name, keep);
    return s;
}

// ---------- the envelope trailer ----------------------------------------------

// A writer that counts past its cap instead of stopping, so one pass both
// writes and sizes; a short buffer is then refused whole.
typedef struct { uint8_t *p; int at, cap; } TW;

static void tw_u8(TW *w, int v) { if (w->at < w->cap) w->p[w->at] = (uint8_t)v; w->at++; }
// A length prefix (u16 when `wide`, else u8), then the bytes.
static void tw_blob(TW *w, int wide, const char *b, int n) {
    tw_u8(w, n & 0xff);
    if (wide) tw_u8(w, (n >> 8) & 0xff);
    for (int i = 0; i < n; i++) tw_u8(w, (uint8_t)b[i]);
}

int roster_trailer_write(const Roster *r, const char *game_id, int gid_len, int status,
                         uint32_t good_mask, uint8_t *out, int cap) {
    return roster_trailer_write_ai(r, game_id, gid_len, status, good_mask, roster_bot_mask(r), out, cap);
}

int roster_trailer_write_ai(const Roster *r, const char *game_id, int gid_len, int status,
                            uint32_t good_mask, uint32_t ai_mask, uint8_t *out, int cap) {
    const int v = roster_validate(r);
    if (v != ROSTER_OK) return v;
    if (gid_len < 0 || gid_len > ROSTER_GAME_ID_MAX || (gid_len > 0 && !game_id)) return ROSTER_E_GAME_ID;
    if (status < 0 || status > 2) return ROSTER_E_STATUS;
    if ((good_mask >> r->n) != 0) return ROSTER_E_GOOD;
    if (!out || cap < 0) return ROSTER_E_CAP;

    TW w = { out, 0, cap };
    int n_good = 0;
    tw_u8(&w, ROSTER_TRAILER_FORMAT);
    tw_blob(&w, 1, game_id, gid_len);
    tw_blob(&w, 1, r->title, r->title_len);
    tw_u8(&w, status);
    tw_u8(&w, r->n);
    for (int s = 0; s < r->n; s++) {
        tw_u8(&w, s);
        tw_blob(&w, 0, r->seats[s].name, r->seats[s].name_len);
    }
    for (int s = 0; s < r->n; s++) {
        tw_blob(&w, 1, r->seats[s].id, r->seats[s].id_len);
        tw_u8(&w, (int)((ai_mask >> s) & 1u));
        n_good += (good_mask >> s) & 1u;
    }
    tw_u8(&w, n_good);
    for (int s = 0; s < r->n; s++)
        if ((good_mask >> s) & 1u) tw_blob(&w, 1, r->seats[s].id, r->seats[s].id_len);
    tw_u8(&w, 0);   // has_ts
    return w.at > cap ? ROSTER_E_CAP : w.at;
}

typedef struct { const uint8_t *p; int at, len; } TR;

static int tr_u8(TR *t, int *v) {
    if (t->at >= t->len) return 0;
    *v = t->p[t->at++];
    return 1;
}

// A length prefix (u16 when `wide`), then that many bytes: into dst when dst is
// set, skipped otherwise. ROSTER_OK, E_SHORT, or `e` when over `cap`.
static int tr_blob(TR *t, int wide, int cap, int e, char *dst, uint8_t *len_out) {
    int n, hi = 0;
    if (!tr_u8(t, &n) || (wide && !tr_u8(t, &hi))) return ROSTER_E_SHORT;
    n |= hi << 8;
    if (n > cap) return e;
    if (t->at + n > t->len) return ROSTER_E_SHORT;
    if (dst) put_field(dst, cap, len_out, (const char *)t->p + t->at, n);
    t->at += n;
    return ROSTER_OK;
}

static int trailer_parse(TR *t, Roster *r, char *gid, uint8_t *gl, int *st, uint32_t *ai) {
    int v, n, flag;
    if (!tr_u8(t, &v)) return ROSTER_E_SHORT;
    if (v != ROSTER_TRAILER_FORMAT) return ROSTER_E_VERSION;
    if ((v = tr_blob(t, 1, ROSTER_GAME_ID_MAX, ROSTER_E_GAME_ID, gid, gl)) != ROSTER_OK) return v;
    if ((v = tr_blob(t, 1, ROSTER_TITLE_MAX, ROSTER_E_TITLE, r->title, &r->title_len)) != ROSTER_OK) return v;
    if (!tr_u8(t, st)) return ROSTER_E_SHORT;
    if (*st > 2) return ROSTER_E_STATUS;
    if (!tr_u8(t, &n)) return ROSTER_E_SHORT;
    if (n > MAX_PLAYERS) return ROSTER_E_COUNT;
    r->n = (int8_t)n;
    for (int s = 0; s < n; s++) {
        if (!tr_u8(t, &v)) return ROSTER_E_SHORT;
        if (v != s) return ROSTER_E_SEAT;
        if ((v = tr_blob(t, 0, ROSTER_NAME_MAX, ROSTER_E_NAME, r->seats[s].name, &r->seats[s].name_len)) != ROSTER_OK)
            return v;
    }
    for (int s = 0; s < n; s++) {
        if ((v = tr_blob(t, 1, ROSTER_ID_MAX, ROSTER_E_ID, r->seats[s].id, &r->seats[s].id_len)) != ROSTER_OK) return v;
        if (!tr_u8(t, &flag)) return ROSTER_E_SHORT;
        if (flag > 1) return ROSTER_E_FLAG;
        *ai |= (uint32_t)flag << s;
    }
    // Good ids: shape only. They are an order the web used to keep, and the
    // mask they stood for lives in the state blob.
    if (!tr_u8(t, &n)) return ROSTER_E_SHORT;
    for (int i = 0; i < n; i++)
        if ((v = tr_blob(t, 1, ROSTER_ID_MAX, ROSTER_E_ID, 0, 0)) != ROSTER_OK) return v;
    if (!tr_u8(t, &flag)) return ROSTER_E_SHORT;
    if (flag > 1) return ROSTER_E_FLAG;
    if (flag && (t->at += 8) > t->len) return ROSTER_E_SHORT;   // an old server's f64 timestamp
    return roster_validate(r);
}

int roster_trailer_read(Roster *r, char *game_id, int *gid_len, int *status,
                        uint32_t *ai_mask, const uint8_t *p, int len, int *consumed) {
    char gid[ROSTER_GAME_ID_MAX + 1];
    uint8_t gl = 0;
    int st = 0;
    uint32_t ai = 0;
    TR t = { p, 0, p ? len : 0 };
    memset(r, 0, sizeof(*r));
    const int v = trailer_parse(&t, r, gid, &gl, &st, &ai);
    if (v != ROSTER_OK) { memset(r, 0, sizeof(*r)); return v; }
    if (game_id) memcpy(game_id, gid, sizeof(gid));
    if (gid_len) *gid_len = gl;
    if (status) *status = st;
    if (ai_mask) *ai_mask = ai;
    if (consumed) *consumed = t.at;
    return ROSTER_OK;
}
