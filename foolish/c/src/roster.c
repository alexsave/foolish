// roster.c - who sits where. See roster.h for both byte layouts.
//
// Written for size as well as clarity: bots.wasm ships to every edge cold start
// and every browser, so the repeated "length byte, then bytes" shapes of both
// layouts go through one helper each way rather than being spelled out per
// field.
#include "roster.h"
#include "../../../shared/c/text_util/utf8.h"
#include <string.h>

// FORMAT 1 (read only): every field is a length byte followed by its bytes at a
// fixed place; these are the offsets of the LENGTH bytes (roster.h).
#define TITLE_AT       2
#define SEATS_AT       (TITLE_AT + 1 + ROSTER_TITLE_MAX)
#define SEAT_ID_AT     0
#define SEAT_NAME_AT   (SEAT_ID_AT + 1 + ROSTER_ID_MAX)
#define SEAT_BRAIN_AT  (SEAT_NAME_AT + 1 + ROSTER_NAME_MAX)

_Static_assert(SEATS_AT == 203 && SEAT_NAME_AT == 37 && SEAT_BRAIN_AT == 102, "roster.h offsets");
_Static_assert(SEAT_BRAIN_AT + 1 + ROSTER_BRAIN_MAX + 2 == ROSTER_V1_SEAT_BYTES, "a seat record is 128 bytes");
_Static_assert(SEATS_AT + MAX_PLAYERS * ROSTER_V1_SEAT_BYTES == ROSTER_V1_BYTES, "a format 1 roster is 1227 bytes");
_Static_assert(ROSTER_MAX == 1211, "the longest format 2 roster");

// ---------- byte rules --------------------------------------------------------

// Strict UTF-8 (shared/c/text_util/utf8.h): TextEncoder and Swift's String.utf8
// only ever produce it, so anything else is corruption or a hostile writer.
static int utf8_valid(const char *str, int n) {
    return text_utf8_scan(str, (size_t)n, 0, 0, 0) == 0;
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

// ---------- packed UUIDs ------------------------------------------------------

static const char HEX[] = "0123456789abcdef";

static int hex_val(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;   // uppercase too: only the canonical lowercase text packs
}

int roster_id_pack(const char *id, int len, uint8_t out[ROSTER_UUID_BYTES]) {
    if (!id || len != ROSTER_UUID_TEXT) return 0;
    int k = 0;
    for (int i = 0; i < ROSTER_UUID_TEXT; ) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-') return 0;
            i++;
            continue;
        }
        const int hi = hex_val((uint8_t)id[i]), lo = hex_val((uint8_t)id[i + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[k++] = (uint8_t)(hi << 4 | lo);
        i += 2;
    }
    return 1;
}

// The 36 characters of a packed UUID: 8-4-4-4-12 hex digits, two per byte, straight-line (it
// runs for every seat of every adopt).
static void uuid_text(const uint8_t *b, char *out) {
    static const uint8_t AT[ROSTER_UUID_BYTES] = { 0, 2, 4, 6, 9, 11, 14, 16, 19, 21, 24, 26, 28, 30, 32, 34 };
    for (int k = 0; k < ROSTER_UUID_BYTES; k++) {
        out[AT[k]] = HEX[b[k] >> 4];
        out[AT[k] + 1] = HEX[b[k] & 15];
    }
    out[8] = out[13] = out[18] = out[23] = '-';
}

// ---------- durable encoding --------------------------------------------------

static uint8_t *emit(uint8_t *at, const char *src, uint8_t len) {
    at[0] = len;
    memcpy(at + 1, src, len);
    return at + 1 + len;
}

int roster_encode(const Roster *r, uint8_t *out, int cap) {
    const int v = roster_validate(r);
    if (v != ROSTER_OK) return v;
    return roster_encode_validated(r, out, cap);
}

int roster_encode_validated(const Roster *r, uint8_t *out, int cap) {
    // Packed first, so the length is known before anything is written.
    uint8_t packed[MAX_PLAYERS][ROSTER_UUID_BYTES];
    uint8_t is_packed[MAX_PLAYERS];
    int need = 3 + r->title_len;
    for (int s = 0; s < r->n; s++) {
        const RosterSeat *x = &r->seats[s];
        is_packed[s] = (uint8_t)roster_id_pack(x->id, x->id_len, packed[s]);
        need += 3 + (is_packed[s] ? ROSTER_UUID_BYTES : x->id_len) + x->name_len + x->brain_len;
    }
    if (!out || cap < need) return ROSTER_E_CAP;
    uint8_t *q = out;
    *q++ = ROSTER_FORMAT_VERSION;
    *q++ = (uint8_t)r->n;
    q = emit(q, r->title, r->title_len);
    for (int s = 0; s < r->n; s++) {
        const RosterSeat *x = &r->seats[s];
        if (is_packed[s]) {
            *q++ = 0;
            memcpy(q, packed[s], ROSTER_UUID_BYTES);
            q += ROSTER_UUID_BYTES;
        } else {
            q = emit(q, x->id, x->id_len);
        }
        q = emit(q, x->name, x->name_len);
        q = emit(q, x->brain, x->brain_len);
    }
    return (int)(q - out);
}

// One durable field into the struct; `e` when its length is over the cap.
static int take(const uint8_t *at, int cap, int e, char *dst, uint8_t *len_out) {
    if (at[0] > cap) return e;
    put_field(dst, cap, len_out, (const char *)at + 1, at[0]);
    return ROSTER_OK;
}

// Format 1: the fixed-width record, refused unless it is the one canonical byte string for its
// roster (every pad, reserved and unused-record byte zero).
static int decode_v1(Roster *r, const uint8_t *p) {
    if (p[1] > MAX_PLAYERS) return ROSTER_E_COUNT;
    r->n = (int8_t)p[1];
    // Caps first (they bound the copies), then the rules, then the padding: a
    // field cut mid-sequence is a bad name, not a stray pad byte.
    int v = take(p + TITLE_AT, ROSTER_TITLE_MAX, ROSTER_E_TITLE, r->title, &r->title_len);
    for (int s = 0; s < r->n && v == ROSTER_OK; s++) {
        const uint8_t *q = p + SEATS_AT + s * ROSTER_V1_SEAT_BYTES;
        RosterSeat *x = &r->seats[s];
        if ((v = take(q + SEAT_ID_AT, ROSTER_ID_MAX, ROSTER_E_ID, x->id, &x->id_len)) == ROSTER_OK
            && (v = take(q + SEAT_NAME_AT, ROSTER_NAME_MAX, ROSTER_E_NAME, x->name, &x->name_len)) == ROSTER_OK)
            v = take(q + SEAT_BRAIN_AT, ROSTER_BRAIN_MAX, ROSTER_E_BRAIN, x->brain, &x->brain_len);
    }
    if (v == ROSTER_OK) v = roster_validate(r);
    if (v != ROSTER_OK) return v;
    // The fields are the roster's; everything else in the 1227 bytes must be zero.
    uint8_t mask[ROSTER_V1_BYTES];
    memset(mask, 0, sizeof mask);
    mask[0] = mask[1] = 1;
    memset(mask + TITLE_AT, 1, 1 + (size_t)r->title_len);
    for (int s = 0; s < r->n; s++) {
        const RosterSeat *x = &r->seats[s];
        uint8_t *m = mask + SEATS_AT + s * ROSTER_V1_SEAT_BYTES;
        memset(m + SEAT_ID_AT, 1, 1 + (size_t)x->id_len);
        memset(m + SEAT_NAME_AT, 1, 1 + (size_t)x->name_len);
        memset(m + SEAT_BRAIN_AT, 1, 1 + (size_t)x->brain_len);
    }
    for (int i = 0; i < ROSTER_V1_BYTES; i++)
        if (!mask[i] && p[i]) return ROSTER_E_PADDING;
    return ROSTER_OK;
}

// THE FORMAT 2 WALK, shared by the decoder and roster_seat_refs so the two cannot read a
// roster differently: one length-prefixed field at *at, checked against its cap and the end.
static int walk_field(const uint8_t *p, int len, int *at, int cap, int e, const uint8_t **f, int *n) {
    if (*at >= len) return ROSTER_E_LENGTH;
    *n = p[*at];
    if (*n > cap) return e;
    if (*at + 1 + *n > len) return ROSTER_E_LENGTH;
    *f = p + *at + 1;
    *at += 1 + *n;
    return ROSTER_OK;
}

// A format 2 id at *at: text, or (length byte 0) a packed UUID.
static int walk_id(const uint8_t *p, int len, int *at, const uint8_t **f, int *n, int *packed) {
    if (*at >= len) return ROSTER_E_LENGTH;
    *packed = p[*at] == 0;
    if (!*packed) return walk_field(p, len, at, ROSTER_ID_MAX, ROSTER_E_ID, f, n);
    if (*at + 1 + ROSTER_UUID_BYTES > len) return ROSTER_E_LENGTH;
    *f = p + *at + 1;
    *n = ROSTER_UUID_BYTES;
    *at += 1 + ROSTER_UUID_BYTES;
    return ROSTER_OK;
}

// The format 2 seats into refs (shape only): their count. The same walk as unpack_v2, field by
// field (walk_field, walk_id), so the two cannot read a roster differently.
static int walk_v2(const uint8_t *p, int len, RosterSeatRef *refs) {
    if (len < 2) return ROSTER_E_LENGTH;
    if (p[1] > MAX_PLAYERS) return ROSTER_E_COUNT;
    const int n = p[1];
    int at = 2, v, k;
    const uint8_t *f;
    if ((v = walk_field(p, len, &at, ROSTER_TITLE_MAX, ROSTER_E_TITLE, &f, &k)) != ROSTER_OK) return v;
    for (int s = 0; s < n; s++) {
        RosterSeatRef *x = &refs[s];
        int packed;
        if ((v = walk_id(p, len, &at, &x->id, &k, &packed)) != ROSTER_OK) return v;
        x->id_len = (uint8_t)k;
        x->packed = (uint8_t)packed;
        if ((v = walk_field(p, len, &at, ROSTER_NAME_MAX, ROSTER_E_NAME, &f, &k)) != ROSTER_OK) return v;
        if ((v = walk_field(p, len, &at, ROSTER_BRAIN_MAX, ROSTER_E_BRAIN, &f, &k)) != ROSTER_OK) return v;
        x->bot = k > 0;
    }
    return at == len ? n : ROSTER_E_LENGTH;   // nothing after the last seat
}

// The format 2 fields into *r, which is zeroed: the bytes, and the zero after each that the
// struct keeps (roster.h). Shape only, in one pass; `canon` also refuses a UUID spelled as text.
// A plain copy per field, not put_field: the struct is zero already, so nothing is cleared twice.
static int unpack_v2(Roster *r, const uint8_t *p, int len, int canon) {
    if (len < 2) return ROSTER_E_LENGTH;
    if (p[1] > MAX_PLAYERS) return ROSTER_E_COUNT;
    const int n = p[1];
    int at = 2, v, k, packed;
    const uint8_t *f;
    if ((v = walk_field(p, len, &at, ROSTER_TITLE_MAX, ROSTER_E_TITLE, &f, &k)) != ROSTER_OK) return v;
    r->n = (int8_t)n;
    r->title_len = (uint8_t)k;
    memcpy(r->title, f, (size_t)k);
    for (int s = 0; s < n; s++) {
        RosterSeat *x = &r->seats[s];
        if ((v = walk_id(p, len, &at, &f, &k, &packed)) != ROSTER_OK) return v;
        if (packed) {
            uuid_text(f, x->id);
            x->id_len = ROSTER_UUID_TEXT;
        } else {
            uint8_t unused[ROSTER_UUID_BYTES];
            if (canon && roster_id_pack((const char *)f, k, unused)) return ROSTER_E_CANON;
            memcpy(x->id, f, (size_t)k);
            x->id_len = (uint8_t)k;
        }
        if ((v = walk_field(p, len, &at, ROSTER_NAME_MAX, ROSTER_E_NAME, &f, &k)) != ROSTER_OK) return v;
        memcpy(x->name, f, (size_t)k);
        x->name_len = (uint8_t)k;
        if ((v = walk_field(p, len, &at, ROSTER_BRAIN_MAX, ROSTER_E_BRAIN, &f, &k)) != ROSTER_OK) return v;
        memcpy(x->brain, f, (size_t)k);
        x->brain_len = (uint8_t)k;
    }
    return at == len ? ROSTER_OK : ROSTER_E_LENGTH;   // nothing after the last seat
}

int roster_decode(Roster *r, const uint8_t *p, int len) {
    memset(r, 0, sizeof(*r));
    if (!p || len < 1) return ROSTER_E_LENGTH;
    int v;
    if (p[0] == ROSTER_FORMAT_VERSION) {
        v = unpack_v2(r, p, len, 1);
        if (v == ROSTER_OK) v = roster_validate(r);
    } else if (p[0] == ROSTER_FORMAT_V1) {
        v = len == ROSTER_V1_BYTES ? decode_v1(r, p) : ROSTER_E_LENGTH;
    } else {
        v = ROSTER_E_VERSION;
    }
    if (v != ROSTER_OK) memset(r, 0, sizeof(*r));
    return v;
}

int roster_unpack(Roster *r, const uint8_t *p, int len) {
    if (!p || len < 1 || p[0] != ROSTER_FORMAT_VERSION) return roster_decode(r, p, len);
    memset(r, 0, sizeof(*r));
    const int v = unpack_v2(r, p, len, 0);
    if (v != ROSTER_OK) memset(r, 0, sizeof(*r));
    return v;
}

// ---------- the seats of durable bytes ----------------------------------------

int roster_seat_refs(const uint8_t *p, int len, RosterSeatRef *out) {
    if (!p || len < 1) return ROSTER_E_LENGTH;
    if (p[0] == ROSTER_FORMAT_VERSION) return walk_v2(p, len, out);
    if (p[0] != ROSTER_FORMAT_V1) return ROSTER_E_VERSION;
    if (len != ROSTER_V1_BYTES) return ROSTER_E_LENGTH;
    if (p[1] > MAX_PLAYERS) return ROSTER_E_COUNT;
    for (int s = 0; s < p[1]; s++) {
        const uint8_t *q = p + SEATS_AT + s * ROSTER_V1_SEAT_BYTES;
        if (q[SEAT_ID_AT] > ROSTER_ID_MAX) return ROSTER_E_ID;
        if (q[SEAT_BRAIN_AT] > ROSTER_BRAIN_MAX) return ROSTER_E_BRAIN;
        out[s] = (RosterSeatRef){ q + SEAT_ID_AT + 1, q[SEAT_ID_AT], 0, q[SEAT_BRAIN_AT] > 0 };
    }
    return p[1];
}

int roster_ref_text(const RosterSeatRef *s, char *out) {
    if (s->packed) {
        uuid_text(s->id, out);
        out[ROSTER_UUID_TEXT] = 0;
        return ROSTER_UUID_TEXT;
    }
    memcpy(out, s->id, s->id_len);
    out[s->id_len] = 0;
    return s->id_len;
}

int roster_ref_is(const RosterSeatRef *s, const char *id, int id_len, const uint8_t *packed) {
    if (s->packed) return packed && bytes_eq((const char *)s->id, (const char *)packed, ROSTER_UUID_BYTES);
    return s->id_len == id_len && bytes_eq((const char *)s->id, id, id_len);
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

// JavaScript's String.prototype.trim whitespace, as UTF-8 (ECMA-262 WhiteSpace
// and LineTerminator): the length of the whitespace scalar at p, or 0.
static int js_space(const uint8_t *p, int n) {
    if (n >= 1 && ((p[0] >= 0x09 && p[0] <= 0x0d) || p[0] == 0x20)) return 1;
    if (n >= 2 && p[0] == 0xc2 && p[1] == 0xa0) return 2;                               // U+00A0
    if (n >= 3 && p[0] == 0xe1 && p[1] == 0x9a && p[2] == 0x80) return 3;               // U+1680
    if (n >= 3 && p[0] == 0xe2 && p[1] == 0x80 && ((p[2] >= 0x80 && p[2] <= 0x8a)       // U+2000-200A
        || p[2] == 0xa8 || p[2] == 0xa9 || p[2] == 0xaf)) return 3;                      // U+2028, 2029, 202F
    if (n >= 3 && p[0] == 0xe2 && p[1] == 0x81 && p[2] == 0x9f) return 3;               // U+205F
    if (n >= 3 && p[0] == 0xe3 && p[1] == 0x80 && p[2] == 0x80) return 3;               // U+3000
    if (n >= 3 && p[0] == 0xef && p[1] == 0xbb && p[2] == 0xbf) return 3;               // U+FEFF
    return 0;
}

int roster_title_keep(const char *t, int len, int *lo_out) {
    const uint8_t *p = (const uint8_t *)t;
    if (len < 0 || (len > 0 && !p)) return ROSTER_E_TITLE;
    int units;
    if (text_utf8_scan(t, (size_t)len, 0, 0, &units) != 0) return ROSTER_E_TITLE;
    if (units > 50) return ROSTER_E_TITLE;
    int lo = 0, hi = len, k;
    while (lo < hi && (k = js_space(p + lo, hi - lo)) > 0) lo += k;
    for (int moved = 1; moved && hi > lo; ) {
        moved = 0;
        for (int w = 3; w >= 1; w--) {
            if (hi - w >= lo && js_space(p + hi - w, w) == w) { hi -= w; moved = 1; break; }
        }
    }
    if (hi == lo) return ROSTER_E_TITLE;
    *lo_out = lo;
    return hi - lo;
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

// The trailer is sized first and refused whole when it does not fit; then every byte is a plain
// store (a length prefix, u16 when `wide`, then the bytes copied).
static uint8_t *put_blob(uint8_t *p, int wide, const char *b, int n) {
    *p++ = (uint8_t)(n & 0xff);
    if (wide) *p++ = (uint8_t)((n >> 8) & 0xff);
    if (n > 0) memcpy(p, b, (size_t)n);
    return p + n;
}

int roster_trailer_write(const Roster *r, const char *game_id, int gid_len, int status,
                         uint32_t good_mask, int passing, uint8_t *out, int cap) {
    return roster_trailer_write_ai(r, game_id, gid_len, status, good_mask, roster_bot_mask(r), passing, out, cap);
}

int roster_trailer_write_ai(const Roster *r, const char *game_id, int gid_len, int status,
                            uint32_t good_mask, uint32_t ai_mask, int passing, uint8_t *out, int cap) {
    const int v = roster_validate(r);
    if (v != ROSTER_OK) return v;
    return roster_trailer_write_validated(r, game_id, gid_len, status, good_mask, ai_mask, passing, out, cap);
}

int roster_trailer_write_validated(const Roster *r, const char *game_id, int gid_len, int status,
                                   uint32_t good_mask, uint32_t ai_mask, int passing, uint8_t *out, int cap) {
    if (gid_len < 0 || gid_len > ROSTER_GAME_ID_MAX || (gid_len > 0 && !game_id)) return ROSTER_E_GAME_ID;
    if (status < 0 || status > 2) return ROSTER_E_STATUS;
    if ((good_mask >> r->n) != 0) return ROSTER_E_GOOD;
    if (passing != 0 && passing != 1) return ROSTER_E_RULES;
    if (!out || cap < 0) return ROSTER_E_CAP;

    int need = 1 + 2 + gid_len + 2 + r->title_len + 1 + 1 + 1 + 1 + (passing ? 0 : 1);   // + n_good, has_ts, the rules byte
    int n_good = 0;
    for (int s = 0; s < r->n; s++) {
        need += 2 + r->seats[s].name_len + 2 + r->seats[s].id_len + 1;
        if ((good_mask >> s) & 1u) need += 2 + r->seats[s].id_len, n_good++;
    }
    if (need > cap) return ROSTER_E_CAP;
    uint8_t *p = out;
    // The oldest format that can carry the table (roster.h): a passing table is
    // the format 1 trailer it always was.
    *p++ = passing ? ROSTER_TRAILER_FORMAT_V1 : ROSTER_TRAILER_FORMAT_V2;
    p = put_blob(p, 1, game_id, gid_len);
    p = put_blob(p, 1, r->title, r->title_len);
    *p++ = (uint8_t)status;
    *p++ = (uint8_t)r->n;
    for (int s = 0; s < r->n; s++) {
        *p++ = (uint8_t)s;
        p = put_blob(p, 0, r->seats[s].name, r->seats[s].name_len);
    }
    for (int s = 0; s < r->n; s++) {
        p = put_blob(p, 1, r->seats[s].id, r->seats[s].id_len);
        *p++ = (uint8_t)((ai_mask >> s) & 1u);
    }
    *p++ = (uint8_t)n_good;
    for (int s = 0; s < r->n; s++)
        if ((good_mask >> s) & 1u) p = put_blob(p, 1, r->seats[s].id, r->seats[s].id_len);
    *p++ = 0;   // has_ts
    if (!passing) *p++ = 0;   // the rules byte: podkidnoy, ROSTER_TRAILER_PASSING clear
    return (int)(p - out);
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

static int trailer_parse(TR *t, Roster *r, char *gid, uint8_t *gl, int *st, uint32_t *ai, int *passing) {
    int v, n, flag, format;
    if (!tr_u8(t, &format)) return ROSTER_E_SHORT;
    if (format != ROSTER_TRAILER_FORMAT_V1 && format != ROSTER_TRAILER_FORMAT_V2) return ROSTER_E_VERSION;
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
    *passing = 1;   // format 1: the passing game, all it could ever describe
    if (format == ROSTER_TRAILER_FORMAT_V2) {
        if (!tr_u8(t, &flag)) return ROSTER_E_SHORT;
        if (flag & ~ROSTER_TRAILER_PASSING) return ROSTER_E_RULES;   // a rule this kernel cannot honour
        *passing = flag & ROSTER_TRAILER_PASSING;
    }
    return roster_validate(r);
}

int roster_trailer_read(Roster *r, char *game_id, int *gid_len, int *status,
                        uint32_t *ai_mask, int *passing, const uint8_t *p, int len, int *consumed) {
    char gid[ROSTER_GAME_ID_MAX + 1];
    uint8_t gl = 0;
    int st = 0;
    uint32_t ai = 0;
    int pass = 1;
    TR t = { p, 0, p ? len : 0 };
    memset(r, 0, sizeof(*r));
    const int v = trailer_parse(&t, r, gid, &gl, &st, &ai, &pass);
    if (v != ROSTER_OK) { memset(r, 0, sizeof(*r)); return v; }
    if (passing) *passing = pass;
    if (game_id) memcpy(game_id, gid, sizeof(gid));
    if (gid_len) *gid_len = gl;
    if (status) *status = st;
    if (ai_mask) *ai_mask = ai;
    if (consumed) *consumed = t.at;
    return ROSTER_OK;
}
