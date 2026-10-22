// graph.h - THE ROAD GRAPH: one flat file, mmap'd, read in place by the server, the matcher,
// the tile renderer and the fake-upload generator.
//
// A SEGMENT is a run of road between two intersections (or a dead end), the unit coverage is
// counted in: 16 mask bits per segment, bit i the i-th sixteenth of its length. Segments are
// ordered along a Hilbert curve over their midpoints at the cell zoom, so a tile's or a matcher's
// neighbourhood is a few runs of adjacent records. Shape points are int32 for the first and
// int16 deltas after it (the importer splits an edge whose delta would not fit, and a segment
// longer than ADV_SEG_MAX_LEN_M or ADV_SEG_MAX_PTS points, so every cap below is a fact of the
// file and not a hope). A NODE is an intersection, with the segments that meet there (CSR), for
// the matcher's transitions. The CELL INDEX is a dense grid of Web Mercator tiles at
// ADV_CELL_ZOOM over the file's bounding box; each cell lists (ascending) every segment whose
// bounding box touches it, so a point's candidates are its cell and the eight around it, and a
// z14 tile is 16 cells. A REGION is a named area (an OSM administrative boundary); a segment
// belongs to the region its midpoint falls in, and the region carries its road length per
// activity type, the denominators of the explorer achievements.
//
// FILE LAYOUT (little-endian, every section 8-byte aligned): adv_graph_hdr, then the sections at
// the header's offsets. The CRC-32C in the header is over every byte after the header; open
// checks it and every offset, count and index against the file's size, since a wrong file must
// refuse, never fault.
#ifndef ADV_GRAPH_H
#define ADV_GRAPH_H

#include "geo.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ADV_GRAPH_MAGIC   0x46524741u   // "AGRF"
#define ADV_GRAPH_VERSION 1
#define ADV_CELL_ZOOM     16
#define ADV_MASK_BITS     16
#define ADV_SEG_MAX_PTS   256      // shape points of one segment (the importer splits longer runs)
#define ADV_SEG_MAX_LEN_M 2000.0f  // and longer ones
#define ADV_SEG_MAX_PER_CELL 4096  // what one cell may list (a cap a reader may size scratch by)
#define ADV_NODE_MAX_ADJ  32       // segments meeting at one node
#define ADV_REGION_NAME   48
#define ADV_NO_REGION     0xFFFFu
#define ADV_MAX_SEGS      (1u << 24)   // 16M segments: a continent-sized extract still indexes in u32
#define ADV_MAX_REGIONS   4096

// Access bits (which activity types score a segment) and the activity types, as indices into
// the per-type arrays everywhere: 0 bike, 1 foot, 2 combined (either).
enum { ADV_T_BIKE = 0, ADV_T_FOOT = 1, ADV_T_ALL = 2, ADV_T_N = 3 };
#define ADV_ACC_BIKE 0x1
#define ADV_ACC_FOOT 0x2
// Highway classes, in order of importance (the low zooms draw the small numbers only).
enum {
    ADV_HC_MOTORWAY = 0, ADV_HC_TRUNK, ADV_HC_PRIMARY, ADV_HC_SECONDARY, ADV_HC_TERTIARY,
    ADV_HC_RESIDENTIAL, ADV_HC_UNCLASSIFIED, ADV_HC_SERVICE, ADV_HC_LIVING, ADV_HC_TRACK,
    ADV_HC_CYCLEWAY, ADV_HC_FOOTWAY, ADV_HC_PATH, ADV_HC_PEDESTRIAN, ADV_HC_STEPS, ADV_HC_OTHER,
    ADV_HC_N
};
#define ADV_SEGF_ONEWAY 0x1

typedef struct {
    int32_t  lat, lon;                              // the first shape point
    int32_t  min_lat, min_lon, max_lat, max_lon;    // the shape's bounding box
    uint32_t shape_off;                             // index of its first delta in deltas[] (n_pts-1 of them)
    uint32_t node_a, node_b;                        // the node at the first point, at the last
    float    len_m;
    uint16_t n_pts;                                 // 2..ADV_SEG_MAX_PTS
    uint16_t region;                                // or ADV_NO_REGION
    uint8_t  access;                                // ADV_ACC_*
    uint8_t  hclass;                                // ADV_HC_*
    uint8_t  flags;                                 // ADV_SEGF_*
    uint8_t  pad;
} adv_seg;

typedef struct {
    int16_t dlat, dlon;
} adv_delta;

typedef struct {
    int32_t  lat, lon;
    uint32_t adj_off;   // into adj[]: n_adj segment ids
    uint16_t n_adj;
    uint16_t pad;
} adv_node;

typedef struct {
    uint32_t off;   // into cell_segs[]
    uint32_t n;
} adv_cell;

typedef struct {
    char     name[ADV_REGION_NAME];   // NUL-terminated
    float    len_m[ADV_T_N];          // road length a type can score here
    uint32_t n_segs;
    int32_t  c_lat, c_lon;            // a point inside (its label)
    int32_t  min_lat, min_lon, max_lat, max_lon;
} adv_region;

typedef struct {
    uint32_t magic, version;
    uint32_t n_segs, n_deltas, n_nodes, n_adj, n_cell_segs, n_regions;
    uint32_t cell_zoom, cell_x0, cell_y0, cells_w, cells_h;   // the grid: tile (x0+i, y0+j) is cells[j*w+i]
    uint32_t crc;                                            // CRC-32C of everything after this header
    int32_t  min_lat, min_lon, max_lat, max_lon;
    uint64_t off_segs, off_deltas, off_nodes, off_adj, off_cells, off_cell_segs, off_regions;
    uint64_t file_size;
    double   total_len_m[ADV_T_N];
    char     source[64];   // what it was built from, for the logs and the page
} adv_graph_hdr;

typedef struct {
    const adv_graph_hdr *h;
    const adv_seg       *segs;
    const adv_delta     *deltas;
    const adv_node      *nodes;
    const uint32_t      *adj;
    const adv_cell      *cells;
    const uint32_t      *cell_segs;
    const adv_region    *regions;
    void  *map;
    size_t map_len;
} adv_graph;

// Maps the file read-only and checks it whole (header, sizes, CRC, every index in range). CB_OK,
// or a CB_E_* with why in err. The mapping lives until close; nothing is copied.
int  adv_graph_open(const char *path, adv_graph *g, char *err, size_t cap);
void adv_graph_close(adv_graph *g);
// The same checks over bytes already in memory (a test's, a builder's): CB_OK or CB_E_INVALID.
int  adv_graph_check(const void *p, size_t n, char *err, size_t cap);

// The segment's shape points into out (cap >= n_pts; ADV_SEG_MAX_PTS always suffices). n_pts.
int  adv_seg_points(const adv_graph *g, uint32_t seg, adv_pt *out, int cap);
// The point `frac` (0..1) of the way along the segment, by length.
adv_pt adv_seg_at(const adv_graph *g, uint32_t seg, double frac);
static inline float adv_mask_len_m(uint16_t mask, float len_m) {
    return (float)__builtin_popcount(mask) * len_m / ADV_MASK_BITS;
}
// The mask bits of the run from fraction a to fraction b (either order) along a segment: every
// sixteenth the run touches by more than ADV_MASK_SLOP of its length, and at least one bit.
#define ADV_MASK_SLOP 0.15
uint16_t adv_mask_of_run(double a, double b);

// The cell (grid coordinates i, j) of a point: false when outside the grid.
bool adv_cell_of(const adv_graph *g, adv_pt p, uint32_t *i, uint32_t *j);
// The segments cell (i, j) lists, ascending: NULL and *n 0 outside the grid.
const uint32_t *adv_cell_list(const adv_graph *g, uint32_t i, uint32_t j, uint32_t *n);
// The grid's cell range a bounding box covers (clipped to the grid; false when disjoint from it).
bool adv_cells_of_box(const adv_graph *g, adv_pt min, adv_pt max, uint32_t *i0, uint32_t *j0, uint32_t *i1, uint32_t *j1);

// ---- THE BUILDER (the importer, the tests' synthetic graphs) --------------------------------
// Ways in, a file out: adv_build_add_way takes an OSM way's node ids and coordinates; finish
// splits ways at shared nodes into segments, orders them on the Hilbert curve, assigns regions
// by midpoint, builds the nodes and the cell index and writes the file. Regions are rings (a
// region may have several: islands); a point is inside a region when inside an odd number of
// its rings (holes are rings too).
typedef struct adv_build adv_build;
adv_build *adv_build_new(const char *source);
void       adv_build_free(adv_build *b);
// A way of n >= 2 points. Consecutive duplicate nodes are dropped. CB_OK, CB_E_NOMEM, CB_E_INVALID.
int adv_build_add_way(adv_build *b, const int64_t *node_ids, const adv_pt *pts, int n, uint8_t access, uint8_t hclass, uint8_t flags);
// A region by name (the same name twice is the same region): its index, or a CB_E_*.
int adv_build_region(adv_build *b, const char *name);
int adv_build_region_ring(adv_build *b, int region, const adv_pt *ring, int n);
// Everything into the file at path (written whole, then renamed into place). CB_OK or a CB_E_*.
int adv_build_finish(adv_build *b, const char *path, char *err, size_t cap);
// What finish built, for the logs.
typedef struct {
    uint32_t segs, nodes, deltas, cells, regions, cell_segs;
    double   total_len_m[ADV_T_N];
} adv_build_stats;
void adv_build_stats_get(const adv_build *b, adv_build_stats *out);

#endif
