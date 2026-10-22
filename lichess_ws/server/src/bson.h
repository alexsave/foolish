// bson.h - just enough BSON for the MongoDB reads lila-ws makes: a writer for a find command's
// filter and projection, a reader that walks a reply document for a few fields. Little-endian
// lengths, NUL-terminated keys and strings, the type bytes of the BSON spec.
//
// BSON is confined to this file and mongo.c: the gateway sees C strings and bools.
#ifndef LWS_BSON_H
#define LWS_BSON_H

#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BSON_DOUBLE 0x01
#define BSON_STRING 0x02
#define BSON_DOC    0x03
#define BSON_ARRAY  0x04
#define BSON_BINARY 0x05
#define BSON_OID    0x07
#define BSON_BOOL   0x08
#define BSON_DATE   0x09
#define BSON_NULL   0x0A
#define BSON_INT32  0x10
#define BSON_INT64  0x12

// ---- the writer: appends a document to a cb_buf; nested documents by begin/end -----------------

#define BSON_W_DEPTH 8
typedef struct {
    cb_buf *b;
    size_t  start[BSON_W_DEPTH];   // offset of the int32 length of each open document
    int     depth;
} bson_w;

void bson_w_init(bson_w *w, cb_buf *b);
void bson_w_begin(bson_w *w);                        // the top-level document
void bson_w_end(bson_w *w);                          // closes the innermost document
void bson_w_doc(bson_w *w, const char *key);         // an embedded document: then fields, then bson_w_end
void bson_w_array(bson_w *w, const char *key);       // an embedded array: keys "0", "1", ...
void bson_w_str(bson_w *w, const char *key, const char *s);
void bson_w_strn(bson_w *w, const char *key, const char *s, size_t n);
void bson_w_bool(bson_w *w, const char *key, bool v);
void bson_w_int32(bson_w *w, const char *key, int32_t v);
void bson_w_int64(bson_w *w, const char *key, int64_t v);
void bson_w_date(bson_w *w, const char *key, int64_t ms);
bool bson_w_ok(const bson_w *w);                     // depth back to 0 and no OOM

// ---- the reader ------------------------------------------------------------------------------

typedef struct {
    const uint8_t *p;     // the document (its length prefix first)
    size_t         len;   // bytes available from p; the document's own length must fit
} bson_doc;

typedef struct {
    uint8_t        type;
    const char    *key;
    const uint8_t *val;   // the value bytes
    size_t         vlen;  // their length
} bson_el;

// Validates the length prefix: false for a document that does not fit in len or is malformed at
// the top level (elements are validated as they are walked).
bool bson_doc_init(bson_doc *d, const uint8_t *p, size_t len);
// Walks elements: *it starts at 0. false at the end or on a malformed element.
bool bson_next(const bson_doc *d, size_t *it, bson_el *el);
// The element named key, or false.
bool bson_find(const bson_doc *d, const char *key, bson_el *el);
// Typed reads of an element; false when the type does not match. Strings are returned without
// their terminating NUL counted (they are NUL-terminated in the buffer).
bool bson_el_str(const bson_el *el, const char **s, size_t *n);
bool bson_el_bool(const bson_el *el, bool *v);
bool bson_el_int(const bson_el *el, int64_t *v);     // int32, int64 or a double with no fraction
bool bson_el_doc(const bson_el *el, bson_doc *d);    // a document or an array

#endif
