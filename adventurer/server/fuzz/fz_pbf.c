// fz_pbf.c - the PBF reader over arbitrary bytes: the one boundary where a file from the internet
// meets our decoder. The callbacks touch every string and every array they are handed, so a
// pointer past a buffer is an ASan report and not a silent pass.
#include "pbf.h"
#include "util/err.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint64_t sink;

static void touch_tags(const pbf_tag *tags, int n) {
    for (int i = 0; i < n; i++) sink += strlen(tags[i].k) + strlen(tags[i].v);
}

static int on_node(void *ud, int64_t id, int32_t lat, int32_t lon, const pbf_tag *tags, int n_tags) {
    (void)ud;
    sink += (uint64_t)id + (uint64_t)lat + (uint64_t)lon;
    touch_tags(tags, n_tags);
    return CB_OK;
}

static int on_way(void *ud, int64_t id, const pbf_tag *tags, int n_tags, const int64_t *refs, int n_refs) {
    (void)ud;
    sink += (uint64_t)id;
    touch_tags(tags, n_tags);
    for (int i = 0; i < n_refs; i++) sink += (uint64_t)refs[i];
    return CB_OK;
}

static int on_relation(void *ud, int64_t id, const pbf_tag *tags, int n_tags, const pbf_member *members, int n_members) {
    (void)ud;
    sink += (uint64_t)id;
    touch_tags(tags, n_tags);
    for (int i = 0; i < n_members; i++) sink += (uint64_t)members[i].id + members[i].type + strlen(members[i].role);
    return CB_OK;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    pbf_cb cb = { on_node, on_way, on_relation };
    char err[128];
    // The first byte picks the pass shape: every kind, or ways + relations, or nodes without
    // tags (the importer's two passes), so the skip paths are fuzzed as much as the decode.
    unsigned which = PBF_NODES | PBF_WAYS | PBF_RELATIONS;
    if (size > 0) {
        switch (data[0] & 3) {
        case 1: which = PBF_WAYS | PBF_RELATIONS; break;
        case 2: which = PBF_NODES | PBF_SKIP_TAGS; break;
        case 3: which = 0; break;
        default: break;
        }
    }
    int rc = pbf_read_mem(data, size, which, &cb, NULL, err, sizeof err);
    if (rc != CB_OK && rc != CB_E_INVALID && rc != CB_E_NOMEM) abort();   // no other code may come out
    return 0;
}
