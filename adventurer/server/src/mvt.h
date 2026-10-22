// mvt.h - Mapbox Vector Tiles, written straight from the graph and a user's coverage: no
// tippecanoe, no pregenerated archives, one tile per request.
//
// THE WRITER is a protobuf encoder for the one message shape MVT needs (Tile > Layer > Feature,
// LINESTRING geometries, integer tag values), over a caller-owned scratch: nothing is allocated
// per tile. THE RENDERER walks the tile's cells of the graph once and emits up to four layers:
//   roads       every segment (the base map: grey); below ADV_TILE_FULL_ZOOM the major classes
//               only, and shapes reduced to their end points below ADV_TILE_SHAPE_ZOOM
//   traveled    the sixteenths the user's coverage (of the requested type) holds
//   untraveled  the sixteenths it does not, of segments the type may score; only at or above
//               ADV_TILE_UNTRAVELED_ZOOM (below it the whole region's roads would be red)
//   unique      the segments only this user has travelled (super-unique)
// Each feature carries tag "c" (the highway class) for the style's line widths, and the
// traveled/untraveled features "t" (0/1) so a style may vary by type later.
#ifndef ADV_MVT_H
#define ADV_MVT_H

#include "coverage.h"
#include "graph.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ADV_TILE_FULL_ZOOM       11   // every class drawn from here up (majors only below)
#define ADV_TILE_SHAPE_ZOOM      13   // full shapes from here up (end points only below)
#define ADV_TILE_UNTRAVELED_ZOOM 11
#define ADV_TILE_MAX_ZOOM        18
#define ADV_TILE_SCRATCH         (2u << 20)   // bytes one tile's writer may use
#define ADV_MVT_MAX_TAGS         4

// ---- the writer -----------------------------------------------------------------------------
typedef struct {
    uint8_t *buf;          // the tile being written
    size_t   cap, len;
    uint8_t *lbuf;         // the current layer's features, moved into buf at layer_end
    size_t   lcap, llen;
    const char *const *keys;
    int      n_keys;
    int      n_values;     // integer values 0..n_values-1 declared so far (value i encodes int i)
    uint32_t n_features;
    bool     full;         // a write did not fit: the tile is refused (CB_E_FULL at finish)
} adv_mvt;

// scratch[0..cap) is split between the tile and the layer in progress.
void adv_mvt_init(adv_mvt *w, uint8_t *scratch, size_t cap);
// keys[0..n_keys) are the layer's tag names; a feature's tag i is the integer value tags[i]
// (0..ADV_MVT_MAX_VALUE), or a negative value for "no such tag".
#define ADV_MVT_MAX_VALUE 255
void adv_mvt_layer_begin(adv_mvt *w, const char *name, const char *const *keys, int n_keys);
// One LINESTRING of n >= 2 points in extent coordinates (geo.h adv_tile_project, rounded).
void adv_mvt_feature_line(adv_mvt *w, uint64_t id, const int *tags, const int32_t *xy, int n);
void adv_mvt_layer_end(adv_mvt *w);
// The tile's bytes: its length, or CB_E_FULL when it did not fit the scratch.
int adv_mvt_finish(adv_mvt *w, const uint8_t **out);

// ---- the renderer ---------------------------------------------------------------------------
// What a tile asks for.
typedef struct {
    int      z;
    uint32_t x, y;
    int      type;            // ADV_T_BIKE, ADV_T_FOOT or ADV_T_ALL: which coverage view
    bool     roads;           // the base layer
    bool     traveled, untraveled, unique;
} adv_tile_req;

// Per-thread scratch for the renderer: the writer's bytes and a visited stamp per segment (a
// segment listed by several cells is emitted once).
typedef struct {
    uint8_t  *bytes;
    size_t    cap;
    uint32_t *stamp;      // n_segs of them
    uint32_t  n_segs;
    uint32_t  gen;        // the current visit
    adv_pt    pts[ADV_SEG_MAX_PTS];
    int32_t   xy[ADV_SEG_MAX_PTS * 2];
} adv_tile_scratch;
int  adv_tile_scratch_init(adv_tile_scratch *s, const adv_graph *g);   // CB_OK or CB_E_NOMEM
void adv_tile_scratch_free(adv_tile_scratch *s);

// The owner of each segment for the unique layer: 0 nobody, a user id, or ADV_OWNER_MANY.
#define ADV_OWNER_MANY 0xFFFFFFFFu

// Renders the tile for `user` (its coverage `cov`, NULL for the base layers alone; `owners`
// the super-unique table, NULL for no unique layer) into s. The tile's length into *out, or a
// CB_E_* (CB_E_INVALID for a tile out of range, CB_E_FULL for one that does not fit).
int adv_tile_render(const adv_graph *g, const adv_tile_req *req, uint32_t user, const adv_cov *cov,
                    const uint32_t *owners, adv_tile_scratch *s, const uint8_t **out);

// A tile path "<z>/<x>/<y>" (digits only, each in range for its zoom): false when it is not one.
bool adv_tile_parse_zxy(const char *s, int *z, uint32_t *x, uint32_t *y);

#endif
