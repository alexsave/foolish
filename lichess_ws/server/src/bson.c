// bson.c - bson.h.
#include "bson.h"

#include <string.h>

static void put_le32(cb_buf *b, size_t at, uint32_t v) {
    if (at + 4 > b->len) return;
    b->p[at] = (uint8_t)v; b->p[at + 1] = (uint8_t)(v >> 8); b->p[at + 2] = (uint8_t)(v >> 16); b->p[at + 3] = (uint8_t)(v >> 24);
}
static void app_le32(cb_buf *b, uint32_t v) { uint8_t p[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; cb_buf_append(b, p, 4); }
static void app_le64(cb_buf *b, uint64_t v) { app_le32(b, (uint32_t)v); app_le32(b, (uint32_t)(v >> 32)); }
static uint32_t get_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get_le64(const uint8_t *p) { return (uint64_t)get_le32(p) | (uint64_t)get_le32(p + 4) << 32; }

void bson_w_init(bson_w *w, cb_buf *b) { w->b = b; w->depth = 0; }

static void open_doc(bson_w *w) {
    if (w->depth >= BSON_W_DEPTH) { w->depth = BSON_W_DEPTH + 1; return; }
    w->start[w->depth++] = w->b->len;
    app_le32(w->b, 0);
}
static void elem(bson_w *w, uint8_t type, const char *key) {
    cb_buf_putc(w->b, (char)type);
    cb_buf_append(w->b, key, strlen(key) + 1);
}

void bson_w_begin(bson_w *w) { open_doc(w); }
void bson_w_end(bson_w *w) {
    if (w->depth < 1 || w->depth > BSON_W_DEPTH) { w->depth = BSON_W_DEPTH + 1; return; }
    cb_buf_putc(w->b, 0);
    size_t at = w->start[--w->depth];
    put_le32(w->b, at, (uint32_t)(w->b->len - at));
}
void bson_w_doc(bson_w *w, const char *key) { elem(w, BSON_DOC, key); open_doc(w); }
void bson_w_array(bson_w *w, const char *key) { elem(w, BSON_ARRAY, key); open_doc(w); }
void bson_w_strn(bson_w *w, const char *key, const char *s, size_t n) {
    elem(w, BSON_STRING, key);
    app_le32(w->b, (uint32_t)(n + 1));
    cb_buf_append(w->b, s, n);
    cb_buf_putc(w->b, 0);
}
void bson_w_str(bson_w *w, const char *key, const char *s) { bson_w_strn(w, key, s, strlen(s)); }
void bson_w_bool(bson_w *w, const char *key, bool v) { elem(w, BSON_BOOL, key); cb_buf_putc(w->b, v ? 1 : 0); }
void bson_w_int32(bson_w *w, const char *key, int32_t v) { elem(w, BSON_INT32, key); app_le32(w->b, (uint32_t)v); }
void bson_w_int64(bson_w *w, const char *key, int64_t v) { elem(w, BSON_INT64, key); app_le64(w->b, (uint64_t)v); }
void bson_w_date(bson_w *w, const char *key, int64_t ms) { elem(w, BSON_DATE, key); app_le64(w->b, (uint64_t)ms); }
bool bson_w_ok(const bson_w *w) { return w->depth == 0 && !w->b->oom; }

// ---- reader ----

bool bson_doc_init(bson_doc *d, const uint8_t *p, size_t len) {
    if (len < 5) return false;
    uint32_t n = get_le32(p);
    if (n < 5 || n > len || p[n - 1] != 0) return false;
    d->p = p;
    d->len = n;
    return true;
}

bool bson_next(const bson_doc *d, size_t *it, bson_el *el) {
    size_t i = *it ? *it : 4;
    if (i + 1 >= d->len) return false;   // only the terminator left
    uint8_t type = d->p[i];
    if (type == 0) return false;
    const char *key = (const char *)d->p + i + 1;
    size_t klen = strnlen(key, d->len - (i + 1));
    if (i + 1 + klen >= d->len) return false;
    size_t v = i + 1 + klen + 1, vlen;
    const uint8_t *vp = d->p + v;
    size_t left = d->len - 1 - v;   // bytes before the terminator
    switch (type) {
    case BSON_DOUBLE: case BSON_INT64: case BSON_DATE: vlen = 8; break;
    case BSON_INT32: vlen = 4; break;
    case BSON_BOOL: vlen = 1; break;
    case BSON_NULL: vlen = 0; break;
    case BSON_OID: vlen = 12; break;
    case BSON_STRING:
        if (left < 4) return false;
        vlen = 4 + get_le32(vp);
        if (vlen < 5 || vlen > left || vp[vlen - 1] != 0) return false;
        break;
    case BSON_DOC: case BSON_ARRAY:
        if (left < 5) return false;
        vlen = get_le32(vp);
        if (vlen < 5 || vlen > left) return false;
        break;
    case BSON_BINARY:
        if (left < 5) return false;
        vlen = 5 + get_le32(vp);
        break;
    default: return false;   // a type the gateway never reads (regex, code, decimal128, ...)
    }
    if (vlen > left) return false;
    el->type = type;
    el->key = key;
    el->val = vp;
    el->vlen = vlen;
    *it = v + vlen;
    return true;
}

bool bson_find(const bson_doc *d, const char *key, bson_el *el) {
    size_t it = 0;
    while (bson_next(d, &it, el))
        if (strcmp(el->key, key) == 0) return true;
    return false;
}

bool bson_el_str(const bson_el *el, const char **s, size_t *n) {
    if (el->type != BSON_STRING) return false;
    *s = (const char *)el->val + 4;
    *n = el->vlen - 5;
    return true;
}
bool bson_el_bool(const bson_el *el, bool *v) {
    if (el->type != BSON_BOOL) return false;
    *v = el->val[0] != 0;
    return true;
}
bool bson_el_int(const bson_el *el, int64_t *v) {
    if (el->type == BSON_INT32) { *v = (int32_t)get_le32(el->val); return true; }
    if (el->type == BSON_INT64 || el->type == BSON_DATE) { *v = (int64_t)get_le64(el->val); return true; }
    if (el->type == BSON_DOUBLE) {
        uint64_t bits = get_le64(el->val);
        double d;
        memcpy(&d, &bits, 8);
        if (d != (double)(int64_t)d) return false;
        *v = (int64_t)d;
        return true;
    }
    return false;
}
bool bson_el_doc(const bson_el *el, bson_doc *d) {
    if (el->type != BSON_DOC && el->type != BSON_ARRAY) return false;
    return bson_doc_init(d, el->val, el->vlen);
}
