// pbf.h - a streaming reader of OSM PBF extracts: entities out through callbacks, nothing kept.
//
// The file is untrusted bytes from the internet, so every length is checked against what is
// left before it is used, every varint stops at its buffer, every count is bounded by the blob
// it came from and by a hard cap, and a malformed file returns CB_E_INVALID, never a fault or a
// hang. The only dependency is zlib (every writer compresses blobs with it; a blob compressed
// any other way is refused). Protobuf is decoded by hand: the format is five messages deep and
// the decoder is a loop over (field, wire type) pairs, so no library and no recursion.
//
// A pass names what it wants (PBF_NODES, PBF_WAYS, PBF_RELATIONS, or-ed) and only those groups
// are decoded: a blob whose groups are all unwanted costs its inflate and a scan of the group
// headers, nothing per entity. PBF_SKIP_TAGS leaves every entity's tag list empty without
// resolving a string, for a pass that wants coordinates only.
#ifndef ADV_PBF_H
#define ADV_PBF_H

#include <stddef.h>
#include <stdint.h>

#define PBF_NODES     0x1
#define PBF_WAYS      0x2
#define PBF_RELATIONS 0x4
#define PBF_SKIP_TAGS 0x8

// The caps a file may not exceed (a real extract is far inside each).
#define PBF_MAX_BLOBHEADER (64u << 10)      // bytes of one BlobHeader
#define PBF_MAX_BLOB       (32u << 20)      // bytes of one Blob, compressed and inflated
#define PBF_MAX_TAGS       (1u << 16)       // tags on one entity
#define PBF_MAX_REFS       (1u << 20)       // node refs of one way
#define PBF_MAX_MEMBERS    (1u << 20)       // members of one relation
#define PBF_MAX_DENSE      (1u << 22)       // nodes in one DenseNodes group

typedef struct {
    const char *k, *v;   // NUL-terminated, valid until the callback returns
} pbf_tag;

enum { PBF_M_NODE = 0, PBF_M_WAY = 1, PBF_M_RELATION = 2 };

typedef struct {
    int64_t     id;
    const char *role;   // NUL-terminated, "" when none
    uint8_t     type;   // PBF_M_*
} pbf_member;

// Each returns CB_OK to continue or a CB_E_* to stop the read with that code. Any may be NULL.
// Coordinates are geo.h's unit (int32 1e-7 degrees), rounded from the file's nanodegrees.
typedef struct {
    int (*node)(void *ud, int64_t id, int32_t lat, int32_t lon, const pbf_tag *tags, int n_tags);
    int (*way)(void *ud, int64_t id, const pbf_tag *tags, int n_tags, const int64_t *refs, int n_refs);
    int (*relation)(void *ud, int64_t id, const pbf_tag *tags, int n_tags, const pbf_member *members, int n_members);
} pbf_cb;

// Reads the whole file (or the bytes), calling back for every entity of the wanted kinds in file
// order. CB_OK, CB_E_IO (the file), CB_E_INVALID (malformed, with why in err), CB_E_NOMEM, or
// the code a callback returned.
int pbf_read_file(const char *path, unsigned which, const pbf_cb *cb, void *ud, char *err, size_t cap);
int pbf_read_mem(const uint8_t *data, size_t n, unsigned which, const pbf_cb *cb, void *ud, char *err, size_t cap);

#endif
