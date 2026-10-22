// mvt.c - mvt.h's writer: a protobuf encoder for the one shape MVT 2.1 needs.
//
// Tile { layers = 3 }  Layer { version = 15, name = 1, features = 2, keys = 3, values = 4,
// extent = 5 }  Feature { id = 1, tags = 2 packed, type = 3, geometry = 4 packed }
// Value { int_value = 4 }. Every field here is a varint or length-delimited, so the encoder is
// two primitives. A submessage needs its length before its bytes, so each feature is SIZED
// first (one pass over the points) and then written; a layer is sized the same way at its end
// from the feature bytes already in lbuf. Nothing is allocated, nothing moves twice: a
// feature's bytes land in lbuf once and are copied into buf once, behind the layer header.
//
// WHEN IT DOES NOT FIT the writer sets `full` and ignores every further call, so the caller
// writes the whole tile without checking and reads one answer from finish: a half-written tile
// would decode as a wrong map, which is worse than no tile.
#include "mvt.h"

#include "util/err.h"

#include <string.h>

// Field tags: (number << 3) | wire type, 0 a varint and 2 length-delimited.
#define PB_TILE_LAYER    0x1A
#define PB_LAYER_NAME    0x0A
#define PB_LAYER_FEATURE 0x12
#define PB_LAYER_KEY     0x1A
#define PB_LAYER_VALUE   0x22
#define PB_LAYER_EXTENT  0x28
#define PB_LAYER_VERSION 0x78
#define PB_VALUE_INT     0x20
#define PB_FEATURE_ID    0x08
#define PB_FEATURE_TAGS  0x12
#define PB_FEATURE_TYPE  0x18
#define PB_FEATURE_GEOM  0x22
#define MVT_VERSION      2
#define MVT_LINESTRING   2
#define CMD_MOVE_TO      1
#define CMD_LINE_TO      2

// Bytes a varint takes: one per started group of 7 significant bits (v | 1 keeps zero at one).
static inline size_t varint_len(uint64_t v) { return (size_t)(64 - __builtin_clzll(v | 1) + 6) / 7; }

static inline uint64_t zigzag(int64_t v) { return ((uint64_t)v << 1) ^ (uint64_t)(v >> 63); }

static inline uint64_t command(uint32_t id, uint32_t count) { return (id & 0x7u) | ((uint64_t)count << 3); }

// One output: the tile's buf or the layer's lbuf. The writes below never check room: a caller
// sizes what it is about to write and reserves it once with `reserve`, so an overflow is
// decided before the first byte and the buffer never holds a partial message.
typedef struct {
    uint8_t *p;
    size_t  *len;
} out_t;

// Room for `need` more bytes in a buffer of cap holding len: false (and counted) when not.
static bool reserve(adv_mvt *w, size_t len, size_t cap, size_t need) {
    if (w->full) return false;
    if (need > cap - len) {
        w->dropped++;
        return false;
    }
    return true;
}

static inline void put_varint(out_t o, uint64_t v) {
    if (v < 0x80) {   // most of a tile: small deltas, small ids, tags
        o.p[(*o.len)++] = (uint8_t)v;
        return;
    }
    while (v >= 0x80) {
        o.p[(*o.len)++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    o.p[(*o.len)++] = (uint8_t)v;
}

static void put_bytes(out_t o, const void *p, size_t n) {
    memcpy(o.p + *o.len, p, n);
    *o.len += n;
}

static void put_str(out_t o, uint8_t tag, const char *s) {
    size_t n = strlen(s);
    o.p[(*o.len)++] = tag;
    put_varint(o, n);
    put_bytes(o, s, n);
}

void adv_mvt_init(adv_mvt *w, uint8_t *scratch, size_t cap) {
    memset(w, 0, sizeof *w);
    // Half each: a layer's features can never exceed the tile they are moved into, so a bigger
    // lbuf could not be used, and a smaller one would refuse a tile that fits.
    w->buf = scratch;
    w->cap = cap / 2;
    w->lbuf = scratch + w->cap;
    w->lcap = cap - w->cap;
}

void adv_mvt_layer_begin(adv_mvt *w, const char *name, const char *const *keys, int n_keys) {
    w->name = name;
    w->keys = keys;
    w->n_keys = n_keys < 0 ? 0 : n_keys > ADV_MVT_MAX_TAGS ? ADV_MVT_MAX_TAGS : n_keys;
    w->n_values = 0;
    w->n_features = 0;
    w->llen = 0;
}

// A linestring's geometry, encoded in one pass. The points after the first go into `pts` as
// zigzag deltas while they are deduplicated (consecutive equal rounded points collapse, so a
// zero-length line is found before a byte of the tile is spent); the three words that precede
// them (MoveTo, the first point, LineTo with the count) are written by the caller once the count
// is known. int32 deltas zigzag into 32 bits, five varint bytes at most: the buffer's size is a
// fact of ADV_SEG_MAX_PTS, not a hope.
typedef struct {
    uint64_t zx0, zy0;                       // the first point, zigzagged from the origin
    int      m;                              // distinct points
    size_t   n;                              // bytes in pts
    uint8_t  pts[(ADV_SEG_MAX_PTS - 1) * 10];
} geom_t;

static void geometry(const int32_t *xy, int n, geom_t *g) {
    int32_t cx = 0, cy = 0;   // MVT's cursor starts at the origin for every feature
    g->m = 0;
    g->n = 0;
    out_t o = { g->pts, &g->n };
    for (int i = 0; i < n; i++) {
        int32_t x = xy[2 * i], y = xy[2 * i + 1];
        if (g->m > 0 && x == cx && y == cy) continue;
        uint64_t zx = zigzag((int64_t)x - cx), zy = zigzag((int64_t)y - cy);
        if (g->m == 0) {
            g->zx0 = zx;
            g->zy0 = zy;
        } else {
            put_varint(o, zx);
            put_varint(o, zy);
        }
        cx = x;
        cy = y;
        g->m++;
    }
}

void adv_mvt_feature_line(adv_mvt *w, uint64_t id, const int *tags, const int32_t *xy, int n) {
    if (w->full || n < 2) return;
    if (n > ADV_SEG_MAX_PTS) {
        w->full = true;   // not a shape this file can hold: a caller's bug, surfaced as a refused tile
        return;
    }
    geom_t g;
    geometry(xy, n, &g);
    if (g.m < 2) return;   // a speck: every point rounded onto the same one
    uint64_t move = command(CMD_MOVE_TO, 1), line = command(CMD_LINE_TO, (uint32_t)(g.m - 1));
    size_t geom_len = varint_len(move) + varint_len(g.zx0) + varint_len(g.zy0) + varint_len(line) + g.n;
    // Tags: (key index, value index) pairs for the keys the feature carries. A value above the
    // cap is clamped, since the layer's values table is sized by the largest value declared and
    // an unbounded value would mean an unbounded table.
    size_t tags_len = 0;
    int n_tags = 0, max_value = -1;
    int v[ADV_MVT_MAX_TAGS];
    for (int i = 0; i < w->n_keys; i++) {
        v[i] = tags ? tags[i] : -1;
        if (v[i] < 0) continue;
        if (v[i] > ADV_MVT_MAX_VALUE) v[i] = ADV_MVT_MAX_VALUE;
        tags_len += varint_len((uint64_t)i) + varint_len((uint64_t)v[i]);
        if (v[i] > max_value) max_value = v[i];
        n_tags++;
    }
    size_t body = 1 + varint_len(id) + 2 + 1 + varint_len(geom_len) + geom_len;
    if (n_tags) body += 1 + varint_len(tags_len) + tags_len;
    size_t total = 1 + varint_len(body) + body;
    if (!reserve(w, w->llen, w->lcap, total)) return;
    out_t o = { w->lbuf, &w->llen };
    size_t at = w->llen;
    o.p[(*o.len)++] = PB_LAYER_FEATURE;
    put_varint(o, body);
    o.p[(*o.len)++] = PB_FEATURE_ID;
    put_varint(o, id);
    if (n_tags) {
        o.p[(*o.len)++] = PB_FEATURE_TAGS;
        put_varint(o, tags_len);
        for (int i = 0; i < w->n_keys; i++) {
            if (v[i] < 0) continue;
            put_varint(o, (uint64_t)i);
            put_varint(o, (uint64_t)v[i]);
        }
    }
    o.p[(*o.len)++] = PB_FEATURE_TYPE;
    o.p[(*o.len)++] = MVT_LINESTRING;
    o.p[(*o.len)++] = PB_FEATURE_GEOM;
    put_varint(o, geom_len);
    put_varint(o, move);
    put_varint(o, g.zx0);
    put_varint(o, g.zy0);
    put_varint(o, line);
    put_bytes(o, g.pts, g.n);
    // The sizes and the bytes must agree exactly, or the length prefix lies and the tile decodes
    // as garbage; a disagreement is a bug here, refused as a full tile rather than served.
    if (w->llen - at != total) w->full = true;
    if (max_value + 1 > w->n_values) w->n_values = max_value + 1;
    w->n_features++;
}

void adv_mvt_layer_end(adv_mvt *w) {
    if (w->full || !w->name) return;
    size_t name_len = strlen(w->name);
    size_t body = 1 + varint_len(name_len) + name_len;
    for (int i = 0; i < w->n_keys; i++) {
        size_t k = strlen(w->keys[i]);
        body += 1 + varint_len(k) + k;
    }
    // Value i is the message { int_value = i }: tag, length, tag, varint(i).
    for (int i = 0; i < w->n_values; i++) body += 2 + 1 + varint_len((uint64_t)i);
    body += 1 + varint_len(ADV_MVT_EXTENT);
    body += 1 + varint_len(MVT_VERSION);
    body += w->llen;
    size_t total = 1 + varint_len(body) + body;
    if (!reserve(w, w->len, w->cap, total)) {   // the whole layer is left out
        w->dropped += w->n_features;
        w->name = NULL;
        w->llen = 0;
        return;
    }
    out_t o = { w->buf, &w->len };
    size_t at = w->len;
    o.p[(*o.len)++] = PB_TILE_LAYER;
    put_varint(o, body);
    put_str(o, PB_LAYER_NAME, w->name);
    for (int i = 0; i < w->n_keys; i++) put_str(o, PB_LAYER_KEY, w->keys[i]);
    for (int i = 0; i < w->n_values; i++) {
        o.p[(*o.len)++] = PB_LAYER_VALUE;
        put_varint(o, 1 + varint_len((uint64_t)i));
        o.p[(*o.len)++] = PB_VALUE_INT;
        put_varint(o, (uint64_t)i);
    }
    o.p[(*o.len)++] = PB_LAYER_EXTENT;
    put_varint(o, ADV_MVT_EXTENT);
    o.p[(*o.len)++] = PB_LAYER_VERSION;
    put_varint(o, MVT_VERSION);
    put_bytes(o, w->lbuf, w->llen);
    if (w->len - at != total) w->full = true;
    w->name = NULL;
    w->llen = 0;
}

int adv_mvt_finish(adv_mvt *w, const uint8_t **out) {
    if (w->full) return CB_E_FULL;
    *out = w->buf;
    return (int)w->len;
}
