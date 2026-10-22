// geo.c - geo.h.
#include "geo.h"

double adv_point_seg_m(const adv_frame *f, adv_pt p, adv_pt a, adv_pt b, double *t) {
    double ax = 0, ay = 0;
    double bx = ((double)b.lon - a.lon) * f->mx, by = ((double)b.lat - a.lat) * f->my;
    double px = ((double)p.lon - a.lon) * f->mx, py = ((double)p.lat - a.lat) * f->my;
    double len2 = bx * bx + by * by;
    double u = 0;
    if (len2 > 0) {
        u = (px * bx + py * by) / len2;
        if (u < 0) u = 0;
        else if (u > 1) u = 1;
    }
    double cx = ax + u * bx, cy = ay + u * by;
    double dx = px - cx, dy = py - cy;
    if (t) *t = u;
    return sqrt(dx * dx + dy * dy);
}

// Mercator: x in 0..1 across the world, y in 0..1 from north to south.
static void mercator(adv_pt p, double *mx, double *my) {
    double lon = (double)p.lon / ADV_DEG, lat = (double)p.lat / ADV_DEG;
    if (lat > 85.05112878) lat = 85.05112878;
    if (lat < -85.05112878) lat = -85.05112878;
    double s = sin(lat * M_PI / 180.0);
    *mx = (lon + 180.0) / 360.0;
    *my = 0.5 - log((1 + s) / (1 - s)) / (4 * M_PI);
}

void adv_tile_of(adv_pt p, int z, uint32_t *x, uint32_t *y) {
    double mx, my;
    mercator(p, &mx, &my);
    double n = (double)(1u << z);
    double fx = floor(mx * n), fy = floor(my * n);
    if (fx < 0) fx = 0;
    if (fy < 0) fy = 0;
    if (fx > n - 1) fx = n - 1;
    if (fy > n - 1) fy = n - 1;
    *x = (uint32_t)fx;
    *y = (uint32_t)fy;
}

void adv_tile_frac(adv_pt p, int z, double *fx, double *fy) {
    double mx, my;
    mercator(p, &mx, &my);
    double n = (double)(1u << z);
    *fx = mx * n;
    *fy = my * n;
}

static double tile_lat(double my) {   // mercator y (0..1) to degrees
    double n = M_PI - 2.0 * M_PI * my;
    return 180.0 / M_PI * atan(0.5 * (exp(n) - exp(-n)));
}

void adv_tile_bounds(int z, uint32_t x, uint32_t y, adv_pt *min, adv_pt *max) {
    double n = (double)(1u << z);
    double lon0 = (double)x / n * 360.0 - 180.0, lon1 = (double)(x + 1) / n * 360.0 - 180.0;
    double lat1 = tile_lat((double)y / n), lat0 = tile_lat((double)(y + 1) / n);
    min->lon = (int32_t)floor(lon0 * ADV_DEG);
    max->lon = (int32_t)ceil(lon1 * ADV_DEG);
    min->lat = (int32_t)floor(lat0 * ADV_DEG);
    max->lat = (int32_t)ceil(lat1 * ADV_DEG);
}

void adv_tile_project(int z, uint32_t x, uint32_t y, adv_pt p, double *px, double *py) {
    double mx, my;
    mercator(p, &mx, &my);
    double n = (double)(1u << z);
    *px = (mx * n - (double)x) * ADV_MVT_EXTENT;
    *py = (my * n - (double)y) * ADV_MVT_EXTENT;
}

double adv_tile_m_per_unit(int z, uint32_t y) {
    double n = (double)(1u << z);
    double lat = tile_lat(((double)y + 0.5) / n);
    double tile_m = 2 * M_PI * ADV_EARTH_M * cos(lat * M_PI / 180.0) / n;
    return tile_m / ADV_MVT_EXTENT;
}

static void hilbert_rot(uint32_t n, uint32_t *x, uint32_t *y, uint32_t rx, uint32_t ry) {
    if (ry == 0) {
        if (rx == 1) {
            *x = n - 1 - *x;
            *y = n - 1 - *y;
        }
        uint32_t t = *x;
        *x = *y;
        *y = t;
    }
}

uint64_t adv_hilbert_xy2d(uint32_t n, uint32_t x, uint32_t y) {
    uint64_t d = 0;
    for (uint32_t s = n / 2; s > 0; s /= 2) {
        uint32_t rx = (x & s) > 0, ry = (y & s) > 0;
        d += (uint64_t)s * s * ((3 * rx) ^ ry);
        hilbert_rot(n, &x, &y, rx, ry);
    }
    return d;
}

void adv_hilbert_d2xy(uint32_t n, uint64_t d, uint32_t *x, uint32_t *y) {
    uint32_t px = 0, py = 0;
    uint64_t t = d;
    for (uint32_t s = 1; s < n; s *= 2) {
        uint32_t rx = 1 & (uint32_t)(t / 2), ry = 1 & (uint32_t)(t ^ rx);
        hilbert_rot(s, &px, &py, rx, ry);
        px += s * rx;
        py += s * ry;
        t /= 4;
    }
    *x = px;
    *y = py;
}
