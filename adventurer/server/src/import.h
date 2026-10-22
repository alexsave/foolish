// import.h - an OSM PBF extract in, our road graph file (graph.h) out.
//
// Two passes over the file, because a way names its nodes by id and the nodes come first: pass
// 1 reads the ways and the relations and decides which are roads (osm_tags.h) and which are
// regions, and so which node ids are needed; pass 2 reads the nodes and keeps the coordinates
// of those. Then the builder is fed: every road way whole (it splits at intersections), every
// boundary relation's member ways assembled into closed rings by matching their end nodes. No
// hash maps: sorted arrays and binary search, so memory is a few words per kept way and node,
// bounded by the caps below (an error past them, never a swap storm).
#ifndef ADV_IMPORT_H
#define ADV_IMPORT_H

#include "graph.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ADV_IMPORT_MAX_REFS  (64u << 20)   // node refs kept across every road and boundary way
#define ADV_IMPORT_MAX_NODES (64u << 20)   // distinct nodes needed
#define ADV_IMPORT_MAX_WAYS  (32u << 20)
#define ADV_IMPORT_MAX_MEMBERS (16u << 20) // way members across every wanted relation

typedef struct {
    uint32_t    admin_levels;      // bit L set: boundaries at admin_level L become regions (a segment
                                   // belongs to the innermost, graph.h); 0 means bit 8 alone (towns)
    const char *fallback_region;   // when no region closes, one named this holds every road;
                                   // NULL means the pbf's basename ("rhode-island")
    // THE FENCE: when within_level is set, only the regions inside one of the boundaries at that
    // level named in within_names (comma-separated, name:en or name) are kept - a region is inside
    // when most of its ring vertices are - and so are only the roads inside a kept region
    // (adv_build_only_regioned), so an extract cut around the fence with a margin gives exactly
    // the fenced area: nine counties, say, and nothing of the towns across their lines.
    int         within_level;
    const char *within_names;
} adv_import_opts;

typedef struct {
    uint64_t ways_read;          // every way in the file
    uint64_t roads_kept;         // ways osm_way_road accepted
    uint64_t member_ways;        // ways kept only because a wanted relation names them
    uint64_t ways_dropped;       // roads not built: a node the file lacks, or fewer than two
    uint64_t nodes_read;         // every node in the file
    uint64_t nodes_needed;       // distinct ids the kept ways name
    uint64_t nodes_kept;         // of those, found in the file
    uint64_t relations_read;
    uint64_t relations_wanted;   // osm_relation_region accepted
    uint64_t fences;             // within_level boundaries named in within_names and closed
    uint64_t regions_outside;    // wanted relations dropped for lying outside every fence
    uint64_t rings_closed, rings_dropped;
    bool     fallback_used;      // no ring closed: the one bounding-box region was made
    double   s_pass1, s_pass2, s_build;   // wall seconds
    adv_build_stats build;
} adv_import_stats;

// CB_OK and the graph at graph_path, or a CB_E_* with why in err (pbf.h's for the file,
// graph.h's for the build, CB_E_FULL past a cap).
int adv_import(const char *pbf_path, const char *graph_path, const adv_import_opts *o, adv_import_stats *stats, char *err, size_t cap);

#endif
