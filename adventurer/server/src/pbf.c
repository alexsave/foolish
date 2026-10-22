// pbf.c - pbf.h: the protobuf primitives, the blob framing, and the block decoders.
#include "pbf.h"

#include "util/err.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

// ---- a bounded cursor over bytes ----------------------------------------------------------
// Every read is against `end`; a read that would pass it returns false and the caller unwinds
// with CB_E_INVALID. No cursor ever moves without a check, which is the whole safety argument.

typedef struct {
    const uint8_t *p, *end;
} cur;

static bool rd_varint(cur *c, uint64_t *out) {
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (c->p >= c->end) return false;
        uint8_t b = *c->p++;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) {
            *out = v;
            return true;
        }
    }
    return false;   // more than 10 bytes: not a varint
}

static inline int64_t zigzag(uint64_t u) { return (int64_t)(u >> 1) ^ -(int64_t)(u & 1); }

static bool rd_sint(cur *c, int64_t *out) {
    uint64_t u;
    if (!rd_varint(c, &u)) return false;
    *out = zigzag(u);
    return true;
}

// The next field: its number, wire type, and for a length-delimited one its bytes as a cursor.
// Varint payloads land in *v; fixed32/fixed64 are skipped (their value is never needed here).
// Groups (wire types 3, 4) and the unused type 6, 7 are a malformed file.
typedef struct {
    uint32_t num;
    uint32_t wire;
    uint64_t v;
    cur      sub;
} field;

static bool rd_field(cur *c, field *f) {
    uint64_t tag;
    if (!rd_varint(c, &tag)) return false;
    f->num = (uint32_t)(tag >> 3);
    f->wire = (uint32_t)(tag & 7);
    if (f->num == 0 || f->num > (1u << 29) - 1) return false;
    switch (f->wire) {
    case 0: return rd_varint(c, &f->v);
    case 1:
        if (c->end - c->p < 8) return false;
        c->p += 8;
        return true;
    case 2: {
        uint64_t len;
        if (!rd_varint(c, &len) || len > (uint64_t)(c->end - c->p)) return false;
        f->sub.p = c->p;
        f->sub.end = c->p + len;
        c->p += len;
        return true;
    }
    case 5:
        if (c->end - c->p < 4) return false;
        c->p += 4;
        return true;
    default: return false;
    }
}

// ---- scratch the decoders fill -------------------------------------------------------------
// Each array grows on demand to a hard cap, and is reused across blobs so a steady file never
// allocates after its first few blocks.

typedef struct {
    void    *p;
    size_t   n, cap, each, max;
} arr;

static int arr_reserve(arr *a, size_t need) {
    if (need <= a->cap) return CB_OK;
    if (need > a->max) return CB_E_FULL;
    size_t nc = a->cap ? a->cap : 1024;
    while (nc < need) nc *= 2;
    if (nc > a->max) nc = a->max;
    void *np = realloc(a->p, nc * a->each);
    if (!np) return CB_E_NOMEM;
    a->p = np;
    a->cap = nc;
    return CB_OK;
}
#define ARR(a, T) ((T *)(a)->p)

typedef struct {
    unsigned      which;
    const pbf_cb *cb;
    void         *ud;
    // The blob's inflated bytes.
    uint8_t *raw;
    size_t   raw_cap;
    // The block's string table: every string copied with a NUL after it, and a pointer each.
    arr strings;   // const char *
    arr sbytes;    // char
    // Per entity.
    arr tags;      // pbf_tag
    arr refs;      // int64_t
    arr members;   // pbf_member
    arr ids, lats, lons, kv;   // int64_t, int64_t, int64_t, uint32_t: DenseNodes' columns
    arr keys, vals;            // uint32_t: a Way's / Relation's / Node's key and val ids
    arr roles, memids, types;  // uint32_t, int64_t, uint32_t
    // The block's coordinate frame.
    int64_t granularity, lat_off, lon_off;
    char   *err;
    size_t  cap;
} rd;

static int invalid(rd *r, const char *what) {
    if (r->cap) snprintf(r->err, r->cap, "pbf: %s", what);
    return CB_E_INVALID;
}

// A packed field (wire type 2) or a lone element (wire type 0) of a repeated varint field,
// appended to a: proto2 writers may use either and a reader accepts both.
static int rd_packed_u32(rd *r, const field *f, arr *a) {
    if (f->wire == 0) {
        int rc = arr_reserve(a, a->n + 1);
        if (rc != CB_OK) return rc;
        ARR(a, uint32_t)[a->n++] = (uint32_t)f->v;
        return CB_OK;
    }
    if (f->wire != 2) return CB_E_INVALID;
    cur c = f->sub;
    while (c.p < c.end) {
        uint64_t v;
        if (!rd_varint(&c, &v)) return CB_E_INVALID;
        int rc = arr_reserve(a, a->n + 1);
        if (rc != CB_OK) return rc;
        ARR(a, uint32_t)[a->n++] = (uint32_t)v;
    }
    return CB_OK;
}

static int rd_packed_s64(rd *r, const field *f, arr *a) {
    if (f->wire == 0) {
        int rc = arr_reserve(a, a->n + 1);
        if (rc != CB_OK) return rc;
        ARR(a, int64_t)[a->n++] = zigzag(f->v);
        return CB_OK;
    }
    if (f->wire != 2) return CB_E_INVALID;
    cur c = f->sub;
    while (c.p < c.end) {
        int64_t v;
        if (!rd_sint(&c, &v)) return CB_E_INVALID;
        int rc = arr_reserve(a, a->n + 1);
        if (rc != CB_OK) return rc;
        ARR(a, int64_t)[a->n++] = v;
    }
    return CB_OK;
}

// Delta-coded columns become absolute in place (the file stores differences; overflow of a
// crafted sum wraps, harmlessly: the value is just a number the caller validates).
static void undelta(arr *a) {
    int64_t *v = ARR(a, int64_t);
    int64_t acc = 0;
    for (size_t i = 0; i < a->n; i++) {
        acc = (int64_t)((uint64_t)acc + (uint64_t)v[i]);
        v[i] = acc;
    }
}

static const char *str_at(rd *r, uint32_t id) {
    return id < r->strings.n ? ARR(&r->strings, const char *)[id] : NULL;
}

// keys[] and vals[] (equal counts) into r->tags. A bad id is a malformed file.
static int tags_of(rd *r, const arr *keys, const arr *vals, int *n_out) {
    if (keys->n != vals->n) return invalid(r, "a tag list with more keys than values");
    if (keys->n > PBF_MAX_TAGS) return invalid(r, "too many tags on one entity");
    int rc = arr_reserve(&r->tags, keys->n);
    if (rc != CB_OK) return rc;
    pbf_tag *t = ARR(&r->tags, pbf_tag);
    for (size_t i = 0; i < keys->n; i++) {
        t[i].k = str_at(r, ARR(keys, uint32_t)[i]);
        t[i].v = str_at(r, ARR(vals, uint32_t)[i]);
        if (!t[i].k || !t[i].v) return invalid(r, "a tag names a string the table does not have");
    }
    *n_out = (int)keys->n;
    return CB_OK;
}

// Nanodegrees to geo.h units (1e-7 deg), rounded half away from zero; out of the world is a
// malformed file (an int32 would also overflow past 214 degrees).
static bool coord(rd *r, int64_t raw, int64_t off, int64_t limit_deg, int32_t *out) {
    // Unsigned so a crafted raw wraps instead of overflowing; the range check below catches it.
    int64_t nano = (int64_t)((uint64_t)off + (uint64_t)r->granularity * (uint64_t)raw);
    if (nano < -400000000000LL || nano > 400000000000LL) return false;
    int64_t u = (nano + (nano >= 0 ? 50 : -50)) / 100;
    if (u < -limit_deg * 10000000 || u > limit_deg * 10000000) return false;
    *out = (int32_t)u;
    return true;
}

// ---- the entities ----------------------------------------------------------------------------

static int do_dense(rd *r, cur c) {
    r->ids.n = r->lats.n = r->lons.n = r->kv.n = 0;
    bool want_tags = !(r->which & PBF_SKIP_TAGS);
    while (c.p < c.end) {
        field f;
        if (!rd_field(&c, &f)) return invalid(r, "DenseNodes");
        int rc = CB_OK;
        switch (f.num) {
        case 1: rc = rd_packed_s64(r, &f, &r->ids); break;
        case 8: rc = rd_packed_s64(r, &f, &r->lats); break;
        case 9: rc = rd_packed_s64(r, &f, &r->lons); break;
        case 10: if (want_tags) rc = rd_packed_u32(r, &f, &r->kv); break;
        default: break;   // denseinfo (5) and anything newer
        }
        if (rc == CB_E_FULL) return invalid(r, "too many nodes in one DenseNodes group");
        if (rc != CB_OK) return rc == CB_E_INVALID ? invalid(r, "DenseNodes columns") : rc;
    }
    if (r->ids.n != r->lats.n || r->ids.n != r->lons.n) return invalid(r, "DenseNodes columns of different lengths");
    undelta(&r->ids);
    undelta(&r->lats);
    undelta(&r->lons);
    const int64_t *ids = ARR(&r->ids, int64_t), *lats = ARR(&r->lats, int64_t), *lons = ARR(&r->lons, int64_t);
    const uint32_t *kv = ARR(&r->kv, uint32_t);
    size_t kvi = 0, kvn = r->kv.n;
    for (size_t i = 0; i < r->ids.n; i++) {
        int n_tags = 0;
        // This node's (key, val) pairs up to the 0 that ends it; a table that ran out means
        // the rest of the group is untagged (the spec allows an empty keys_vals outright).
        if (kvi < kvn) {
            r->tags.n = 0;
            pbf_tag *t = ARR(&r->tags, pbf_tag);
            while (kvi < kvn && kv[kvi] != 0) {
                if (kvi + 1 >= kvn) return invalid(r, "a dense node's key without its value");
                if ((size_t)n_tags >= PBF_MAX_TAGS) return invalid(r, "too many tags on one node");
                int rc = arr_reserve(&r->tags, (size_t)n_tags + 1);
                if (rc != CB_OK) return rc;
                t = ARR(&r->tags, pbf_tag);
                t[n_tags].k = str_at(r, kv[kvi]);
                t[n_tags].v = str_at(r, kv[kvi + 1]);
                if (!t[n_tags].k || !t[n_tags].v) return invalid(r, "a dense node's tag names a string the table does not have");
                n_tags++;
                kvi += 2;
            }
            kvi++;   // the 0
        }
        int32_t lat, lon;
        if (!coord(r, lats[i], r->lat_off, 90, &lat) || !coord(r, lons[i], r->lon_off, 180, &lon))
            return invalid(r, "a node off the planet");
        if (r->cb->node) {
            int rc = r->cb->node(r->ud, ids[i], lat, lon, n_tags ? ARR(&r->tags, pbf_tag) : NULL, n_tags);
            if (rc != CB_OK) return rc;
        }
    }
    return CB_OK;
}

static int do_node(rd *r, cur c) {
    int64_t id = 0, lat_raw = 0, lon_raw = 0;
    bool have_id = false, have_lat = false, have_lon = false;
    bool want_tags = !(r->which & PBF_SKIP_TAGS);
    r->keys.n = r->vals.n = 0;
    while (c.p < c.end) {
        field f;
        if (!rd_field(&c, &f)) return invalid(r, "Node");
        int rc = CB_OK;
        switch (f.num) {
        case 1: if (f.wire != 0) return invalid(r, "Node id"); id = zigzag(f.v); have_id = true; break;
        case 2: if (want_tags) rc = rd_packed_u32(r, &f, &r->keys); break;
        case 3: if (want_tags) rc = rd_packed_u32(r, &f, &r->vals); break;
        case 8: if (f.wire != 0) return invalid(r, "Node lat"); lat_raw = zigzag(f.v); have_lat = true; break;
        case 9: if (f.wire != 0) return invalid(r, "Node lon"); lon_raw = zigzag(f.v); have_lon = true; break;
        default: break;
        }
        if (rc == CB_E_FULL) return invalid(r, "too many tags on one node");
        if (rc != CB_OK) return rc == CB_E_INVALID ? invalid(r, "Node tags") : rc;
    }
    if (!have_id || !have_lat || !have_lon) return invalid(r, "a Node without id or position");
    int n_tags = 0;
    int rc = tags_of(r, &r->keys, &r->vals, &n_tags);
    if (rc != CB_OK) return rc;
    int32_t lat, lon;
    if (!coord(r, lat_raw, r->lat_off, 90, &lat) || !coord(r, lon_raw, r->lon_off, 180, &lon))
        return invalid(r, "a node off the planet");
    if (r->cb->node) return r->cb->node(r->ud, id, lat, lon, n_tags ? ARR(&r->tags, pbf_tag) : NULL, n_tags);
    return CB_OK;
}

static int do_way(rd *r, cur c) {
    int64_t id = 0;
    bool have_id = false;
    bool want_tags = !(r->which & PBF_SKIP_TAGS);
    r->keys.n = r->vals.n = r->refs.n = 0;
    while (c.p < c.end) {
        field f;
        if (!rd_field(&c, &f)) return invalid(r, "Way");
        int rc = CB_OK;
        switch (f.num) {
        case 1: if (f.wire != 0) return invalid(r, "Way id"); id = (int64_t)f.v; have_id = true; break;
        case 2: if (want_tags) rc = rd_packed_u32(r, &f, &r->keys); break;
        case 3: if (want_tags) rc = rd_packed_u32(r, &f, &r->vals); break;
        case 8: rc = rd_packed_s64(r, &f, &r->refs); break;
        default: break;   // info (4), lat/lon (9, 10: the optional LocationsOnWays)
        }
        if (rc == CB_E_FULL) return invalid(r, "too many refs or tags on one way");
        if (rc != CB_OK) return rc == CB_E_INVALID ? invalid(r, "Way columns") : rc;
    }
    if (!have_id) return invalid(r, "a Way without an id");
    int n_tags = 0;
    int rc = tags_of(r, &r->keys, &r->vals, &n_tags);
    if (rc != CB_OK) return rc;
    undelta(&r->refs);
    if (r->cb->way)
        return r->cb->way(r->ud, id, n_tags ? ARR(&r->tags, pbf_tag) : NULL, n_tags, ARR(&r->refs, int64_t), (int)r->refs.n);
    return CB_OK;
}

static int do_relation(rd *r, cur c) {
    int64_t id = 0;
    bool have_id = false;
    bool want_tags = !(r->which & PBF_SKIP_TAGS);
    r->keys.n = r->vals.n = r->roles.n = r->memids.n = r->types.n = 0;
    while (c.p < c.end) {
        field f;
        if (!rd_field(&c, &f)) return invalid(r, "Relation");
        int rc = CB_OK;
        switch (f.num) {
        case 1: if (f.wire != 0) return invalid(r, "Relation id"); id = (int64_t)f.v; have_id = true; break;
        case 2: if (want_tags) rc = rd_packed_u32(r, &f, &r->keys); break;
        case 3: if (want_tags) rc = rd_packed_u32(r, &f, &r->vals); break;
        case 8: rc = rd_packed_u32(r, &f, &r->roles); break;
        case 9: rc = rd_packed_s64(r, &f, &r->memids); break;
        case 10: rc = rd_packed_u32(r, &f, &r->types); break;
        default: break;
        }
        if (rc == CB_E_FULL) return invalid(r, "too many members or tags on one relation");
        if (rc != CB_OK) return rc == CB_E_INVALID ? invalid(r, "Relation columns") : rc;
    }
    if (!have_id) return invalid(r, "a Relation without an id");
    if (r->roles.n != r->memids.n || r->types.n != r->memids.n) return invalid(r, "Relation member columns of different lengths");
    int n_tags = 0;
    int rc = tags_of(r, &r->keys, &r->vals, &n_tags);
    if (rc != CB_OK) return rc;
    undelta(&r->memids);
    if ((rc = arr_reserve(&r->members, r->memids.n)) != CB_OK) return rc;
    pbf_member *m = ARR(&r->members, pbf_member);
    const uint32_t *roles = ARR(&r->roles, uint32_t), *types = ARR(&r->types, uint32_t);
    const int64_t *memids = ARR(&r->memids, int64_t);
    for (size_t i = 0; i < r->memids.n; i++) {
        if (types[i] > PBF_M_RELATION) return invalid(r, "a member of an unknown type");
        m[i].id = memids[i];
        m[i].type = (uint8_t)types[i];
        m[i].role = str_at(r, roles[i]);
        if (!m[i].role) return invalid(r, "a member role names a string the table does not have");
    }
    if (r->cb->relation)
        return r->cb->relation(r->ud, id, n_tags ? ARR(&r->tags, pbf_tag) : NULL, n_tags, m, (int)r->memids.n);
    return CB_OK;
}

// ---- the block ---------------------------------------------------------------------------------

static bool group_wanted(rd *r, cur c) {
    while (c.p < c.end) {
        field f;
        if (!rd_field(&c, &f)) return true;   // let the real decode report it
        if (f.wire != 2) continue;
        if ((f.num == 1 || f.num == 2) && (r->which & PBF_NODES)) return true;
        if (f.num == 3 && (r->which & PBF_WAYS)) return true;
        if (f.num == 4 && (r->which & PBF_RELATIONS)) return true;
    }
    return false;
}

static int do_group(rd *r, cur c) {
    while (c.p < c.end) {
        field f;
        if (!rd_field(&c, &f)) return invalid(r, "PrimitiveGroup");
        if (f.wire != 2) continue;
        int rc = CB_OK;
        switch (f.num) {
        case 1: if (r->which & PBF_NODES) rc = do_node(r, f.sub); break;
        case 2: if (r->which & PBF_NODES) rc = do_dense(r, f.sub); break;
        case 3: if (r->which & PBF_WAYS) rc = do_way(r, f.sub); break;
        case 4: if (r->which & PBF_RELATIONS) rc = do_relation(r, f.sub); break;
        default: break;   // changesets (5)
        }
        if (rc != CB_OK) return rc;
    }
    return CB_OK;
}

static int string_table(rd *r, cur c) {
    r->strings.n = r->sbytes.n = 0;
    // Every string plus its NUL fits in the table's own byte length plus one per string, and
    // there are at most as many strings as bytes (an empty string is still a 2-byte field).
    size_t len = (size_t)(c.end - c.p);
    int rc = arr_reserve(&r->sbytes, len + len / 2 + 2);
    if (rc != CB_OK) return rc;
    while (c.p < c.end) {
        field f;
        if (!rd_field(&c, &f)) return invalid(r, "StringTable");
        if (f.num != 1 || f.wire != 2) continue;
        size_t n = (size_t)(f.sub.end - f.sub.p);
        if ((rc = arr_reserve(&r->strings, r->strings.n + 1)) != CB_OK) return rc;
        if ((rc = arr_reserve(&r->sbytes, r->sbytes.n + n + 1)) != CB_OK) return rc;
        char *dst = ARR(&r->sbytes, char) + r->sbytes.n;
        if (n) memcpy(dst, f.sub.p, n);
        dst[n] = 0;
        // Offsets, not pointers, until the bytes stop moving: pointers are patched below.
        ARR(&r->strings, const char *)[r->strings.n++] = (const char *)(uintptr_t)r->sbytes.n;
        r->sbytes.n += n + 1;
    }
    const char *base = ARR(&r->sbytes, char);
    const char **s = ARR(&r->strings, const char *);
    for (size_t i = 0; i < r->strings.n; i++) s[i] = base + (uintptr_t)s[i];
    return CB_OK;
}

static int do_block(rd *r, const uint8_t *p, size_t n) {
    cur top = { p, p + n };
    // First scan: the frame and whether any group is wanted; second: the groups themselves,
    // so an unwanted block never builds its string table.
    r->granularity = 100;
    r->lat_off = r->lon_off = 0;
    bool any = false;
    cur strtab = { NULL, NULL };
    for (cur c = top; c.p < c.end;) {
        field f;
        if (!rd_field(&c, &f)) return invalid(r, "PrimitiveBlock");
        switch (f.num) {
        case 1: if (f.wire == 2) strtab = f.sub; break;
        case 2: if (f.wire == 2 && !any) any = group_wanted(r, f.sub); break;
        case 17: if (f.wire == 0) r->granularity = (int64_t)f.v; break;
        case 19: if (f.wire == 0) r->lat_off = (int64_t)f.v; break;
        case 20: if (f.wire == 0) r->lon_off = (int64_t)f.v; break;
        default: break;
        }
    }
    if (!any) return CB_OK;
    if (r->granularity <= 0 || r->granularity > 1000000000 || r->lat_off < -400000000000LL || r->lat_off > 400000000000LL ||
        r->lon_off < -400000000000LL || r->lon_off > 400000000000LL)
        return invalid(r, "a block's coordinate frame is off the planet");
    int rc = CB_OK;
    if (strtab.p && (rc = string_table(r, strtab)) != CB_OK) return rc;
    if (!strtab.p) r->strings.n = 0;
    for (cur c = top; c.p < c.end;) {
        field f;
        if (!rd_field(&c, &f)) return invalid(r, "PrimitiveBlock");
        if (f.num == 2 && f.wire == 2 && (rc = do_group(r, f.sub)) != CB_OK) return rc;
    }
    return CB_OK;
}

// ---- the blobs -----------------------------------------------------------------------------------

typedef struct {
    FILE          *f;
    const uint8_t *mem;
    size_t         mem_n, at;
} src;

// Exactly n bytes, or how many there were (short at EOF).
static size_t src_read(src *s, uint8_t *dst, size_t n) {
    if (s->f) return fread(dst, 1, n, s->f);
    size_t left = s->mem_n - s->at, k = n < left ? n : left;
    if (k) memcpy(dst, s->mem + s->at, k);
    s->at += k;
    return k;
}

static int do_blob(rd *r, const uint8_t *p, size_t n, bool want) {
    cur c = { p, p + n };
    cur raw = { NULL, NULL }, zdata = { NULL, NULL };
    int64_t raw_size = -1;
    while (c.p < c.end) {
        field f;
        if (!rd_field(&c, &f)) return invalid(r, "Blob");
        switch (f.num) {
        case 1: if (f.wire == 2) raw = f.sub; break;
        case 2: if (f.wire == 0) raw_size = (int64_t)f.v; break;
        case 3: if (f.wire == 2) zdata = f.sub; break;
        case 4: case 5: case 6: case 7:
            if (f.wire == 2) return invalid(r, "a blob compressed with something other than zlib");
            break;
        default: break;
        }
    }
    if (!want) return CB_OK;   // a header block or an unknown type: framed, never inflated
    if (raw.p) return do_block(r, raw.p, (size_t)(raw.end - raw.p));
    if (!zdata.p) return invalid(r, "a blob with no data");
    if (raw_size < 0 || raw_size > (int64_t)PBF_MAX_BLOB) return invalid(r, "a blob's raw_size");
    if ((size_t)raw_size > r->raw_cap) {
        uint8_t *np = realloc(r->raw, (size_t)raw_size);
        if (!np) return CB_E_NOMEM;
        r->raw = np;
        r->raw_cap = (size_t)raw_size;
    }
    uLongf dlen = (uLongf)raw_size;
    int z = uncompress(r->raw, &dlen, zdata.p, (uLong)(zdata.end - zdata.p));
    if (z != Z_OK || dlen != (uLongf)raw_size) return invalid(r, "a zlib stream that does not inflate to its raw_size");
    return do_block(r, r->raw, (size_t)raw_size);
}

static int read_all(rd *r, src *s) {
    uint8_t *hdr = malloc(PBF_MAX_BLOBHEADER);
    uint8_t *blob = NULL;
    size_t blob_cap = 0;
    int rc = CB_OK;
    if (!hdr) return CB_E_NOMEM;
    for (;;) {
        uint8_t lenb[4];
        size_t got = src_read(s, lenb, 4);
        if (got == 0) break;   // a clean end
        if (got != 4) { rc = invalid(r, "truncated inside a blob header length"); break; }
        uint32_t hlen = (uint32_t)lenb[0] << 24 | (uint32_t)lenb[1] << 16 | (uint32_t)lenb[2] << 8 | lenb[3];
        if (hlen == 0 || hlen > PBF_MAX_BLOBHEADER) { rc = invalid(r, "a blob header length past the cap"); break; }
        if (src_read(s, hdr, hlen) != hlen) { rc = invalid(r, "truncated inside a blob header"); break; }
        // BlobHeader: type (1) string, indexdata (2) bytes, datasize (3) int32.
        cur c = { hdr, hdr + hlen };
        bool is_data = false, has_type = false;
        int64_t datasize = -1;
        while (c.p < c.end) {
            field f;
            if (!rd_field(&c, &f)) { rc = invalid(r, "BlobHeader"); break; }
            if (f.num == 1 && f.wire == 2) {
                has_type = true;
                size_t n = (size_t)(f.sub.end - f.sub.p);
                is_data = n == 7 && memcmp(f.sub.p, "OSMData", 7) == 0;
            } else if (f.num == 3 && f.wire == 0) {
                datasize = (int64_t)f.v;
            }
        }
        if (rc != CB_OK) break;
        if (!has_type || datasize < 0 || datasize > (int64_t)PBF_MAX_BLOB) { rc = invalid(r, "a blob header without a type or with a datasize past the cap"); break; }
        if ((size_t)datasize > blob_cap) {
            uint8_t *np = realloc(blob, (size_t)datasize);
            if (!np) { rc = CB_E_NOMEM; break; }
            blob = np;
            blob_cap = (size_t)datasize;
        }
        if (src_read(s, blob, (size_t)datasize) != (size_t)datasize) { rc = invalid(r, "truncated inside a blob"); break; }
        if ((rc = do_blob(r, blob, (size_t)datasize, is_data)) != CB_OK) break;
    }
    if (rc == CB_OK && s->f && ferror(s->f)) {
        if (r->cap) snprintf(r->err, r->cap, "pbf: read: %s", strerror(errno));
        rc = CB_E_IO;
    }
    free(hdr);
    free(blob);
    return rc;
}

static void rd_init(rd *r, unsigned which, const pbf_cb *cb, void *ud, char *err, size_t cap) {
    memset(r, 0, sizeof *r);
    r->which = which;
    r->cb = cb;
    r->ud = ud;
    r->err = err;
    r->cap = cap;
    // Each scratch's element size and its cap. The string arrays are bounded by the blob size
    // (a string is at least two bytes of field, so there are fewer strings than bytes).
    r->strings.each = sizeof(const char *); r->strings.max = PBF_MAX_BLOB;
    r->sbytes.each = 1;                     r->sbytes.max = PBF_MAX_BLOB * 2;
    r->tags.each = sizeof(pbf_tag);         r->tags.max = PBF_MAX_TAGS;
    r->refs.each = sizeof(int64_t);         r->refs.max = PBF_MAX_REFS;
    r->members.each = sizeof(pbf_member);   r->members.max = PBF_MAX_MEMBERS;
    r->ids.each = r->lats.each = r->lons.each = sizeof(int64_t);
    r->ids.max = r->lats.max = r->lons.max = PBF_MAX_DENSE;
    r->kv.each = sizeof(uint32_t);          r->kv.max = PBF_MAX_DENSE * 2;
    r->keys.each = r->vals.each = sizeof(uint32_t);
    r->keys.max = r->vals.max = PBF_MAX_TAGS;
    r->roles.each = r->types.each = sizeof(uint32_t); r->memids.each = sizeof(int64_t);
    r->roles.max = r->types.max = r->memids.max = PBF_MAX_MEMBERS;
}

static void rd_free(rd *r) {
    free(r->raw);
    arr *all[] = { &r->strings, &r->sbytes, &r->tags, &r->refs, &r->members, &r->ids, &r->lats, &r->lons, &r->kv,
                   &r->keys, &r->vals, &r->roles, &r->memids, &r->types };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) free(all[i]->p);
}

int pbf_read_file(const char *path, unsigned which, const pbf_cb *cb, void *ud, char *err, size_t cap) {
    static const pbf_cb none = { 0 };
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (cap) snprintf(err, cap, "pbf: %s: %s", path, strerror(errno));
        return CB_E_IO;
    }
    rd r;
    rd_init(&r, which, cb ? cb : &none, ud, err, cap);
    src s = { f, NULL, 0, 0 };
    int rc = read_all(&r, &s);
    rd_free(&r);
    fclose(f);
    if (rc == CB_E_NOMEM && cap) snprintf(err, cap, "pbf: out of memory");
    return rc;
}

int pbf_read_mem(const uint8_t *data, size_t n, unsigned which, const pbf_cb *cb, void *ud, char *err, size_t cap) {
    static const pbf_cb none = { 0 };
    rd r;
    rd_init(&r, which, cb ? cb : &none, ud, err, cap);
    src s = { NULL, data, n, 0 };
    int rc = read_all(&r, &s);
    rd_free(&r);
    if (rc == CB_E_NOMEM && cap) snprintf(err, cap, "pbf: out of memory");
    return rc;
}
