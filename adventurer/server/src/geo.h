// geo.h - coordinates, distances, Web Mercator tiles and the Hilbert curve: the arithmetic every
// module shares, so each spells it once.
//
// A point is OSM's unit: int32 degrees times 1e7 (1 unit = 1.1 cm; the whole planet fits). All
// distances are metres; at city scale the equirectangular approximation (a flat frame scaled by
// cos(lat)) is within 0.1% of the geodesic and costs two multiplies, which is what the matcher
// needs per candidate.
#ifndef ADV_GEO_H
#define ADV_GEO_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define ADV_DEG 10000000               // int32 units per degree
#define ADV_LAT_MAX (85 * ADV_DEG)     // Web Mercator's edge; a point beyond it is garbage
#define ADV_LON_MAX (180 * ADV_DEG)
#define ADV_EARTH_M 6371008.8

typedef struct {
    int32_t lat, lon;
} adv_pt;

static inline bool adv_pt_valid(adv_pt p) {
    return p.lat >= -ADV_LAT_MAX && p.lat <= ADV_LAT_MAX && p.lon >= -ADV_LON_MAX && p.lon <= ADV_LON_MAX;
}

// A LOCAL FRAME: metres per unit of latitude and of longitude at one reference latitude, so a
// batch of distances is (dlat*my)^2 + (dlon*mx)^2 with no trigonometry inside the loop.
typedef struct {
    double mx, my;   // metres per int32 unit of lon, of lat
} adv_frame;

static inline adv_frame adv_frame_at(int32_t lat) {
    adv_frame f;
    f.my = ADV_EARTH_M * (M_PI / 180.0) / ADV_DEG;
    f.mx = f.my * cos((double)lat / ADV_DEG * M_PI / 180.0);
    return f;
}

static inline double adv_dist2_m(const adv_frame *f, adv_pt a, adv_pt b) {
    double dx = ((double)b.lon - a.lon) * f->mx, dy = ((double)b.lat - a.lat) * f->my;
    return dx * dx + dy * dy;
}
static inline double adv_dist_m(const adv_frame *f, adv_pt a, adv_pt b) { return sqrt(adv_dist2_m(f, a, b)); }

// The distance from p to the segment ab, and in *t where the nearest point is on it (0 at a, 1
// at b), in the frame f.
double adv_point_seg_m(const adv_frame *f, adv_pt p, adv_pt a, adv_pt b, double *t);

// ---- Web Mercator tiles ---------------------------------------------------------------------

#define ADV_MVT_EXTENT 4096

// The tile at zoom z holding p (x, y in 0..2^z-1).
void adv_tile_of(adv_pt p, int z, uint32_t *x, uint32_t *y);
// The same as fractions: floor is the tile, the rest where in it (a point near an edge).
void adv_tile_frac(adv_pt p, int z, double *fx, double *fy);
// The tile's bounds in int32 units: *min is its south-west corner, *max its north-east.
void adv_tile_bounds(int z, uint32_t x, uint32_t y, adv_pt *min, adv_pt *max);
// p in the tile's extent coordinates (0..ADV_MVT_EXTENT inside the tile; may fall outside it).
void adv_tile_project(int z, uint32_t x, uint32_t y, adv_pt p, double *px, double *py);
// A TILE FRAME: Web Mercator about the tile's middle latitude, so a point projects with
// multiplies and adds and no transcendental: x is linear in longitude (exact), y is the series
// to the third derivative of asinh(tan) at the middle. From ADV_TILE_FRAME_MIN_ZOOM up the
// remainder over the tile, its grown edge and a 2 km segment past it stays under 0.002 extent
// units at 41 N and at 70 N alike (unit_mvt measures it; the fourth derivative grows as sec^5
// tan, which a smaller zoom's wider tile meets first); below, the caller projects exactly.
#define ADV_TILE_FRAME_MIN_ZOOM 8
typedef struct {
    double lat0, lon0;         // the middle, in int32 units
    double px0, py0;           // its extent coordinates
    double ux, rad, c1, c2, c3;
} adv_tile_frame;
void adv_tile_frame_init(adv_tile_frame *f, int z, uint32_t x, uint32_t y);
static inline void adv_tile_frame_project(const adv_tile_frame *f, adv_pt p, double *px, double *py) {
    double d = ((double)p.lat - f->lat0) * f->rad;
    *px = f->px0 + ((double)p.lon - f->lon0) * f->ux;
    *py = f->py0 + d * (f->c1 + d * (f->c2 + d * f->c3));
}
// Metres per extent unit at this tile (its height over 4096).
double adv_tile_m_per_unit(int z, uint32_t y);

// ---- the Hilbert curve ----------------------------------------------------------------------
// The index of (x, y) on the curve over an n by n grid (n a power of two), and back.
uint64_t adv_hilbert_xy2d(uint32_t n, uint32_t x, uint32_t y);
void     adv_hilbert_d2xy(uint32_t n, uint64_t d, uint32_t *x, uint32_t *y);

#endif
