// coverage.c - coverage.h: sorted entries, a sorted merge.
#include "coverage.h"

#include "util/err.h"

#include <stdlib.h>
#include <string.h>

void adv_cov_init(adv_cov *c) { memset(c, 0, sizeof *c); }
void adv_cov_free(adv_cov *c) {
    free(c->e);
    memset(c, 0, sizeof *c);
}

// The index of seg, or the index it would be inserted at (with *found).
static uint32_t cov_find(const adv_cov *c, uint32_t seg, bool *found) {
    uint32_t lo = 0, hi = c->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (c->e[mid].seg < seg) lo = mid + 1;
        else hi = mid;
    }
    *found = lo < c->n && c->e[lo].seg == seg;
    return lo;
}

uint16_t adv_cov_mask(const adv_cov *c, uint32_t seg, int type) {
    bool found;
    uint32_t i = cov_find(c, seg, &found);
    if (!found) return 0;
    if (type == ADV_T_ALL) return (uint16_t)(c->e[i].mask[0] | c->e[i].mask[1]);
    return c->e[i].mask[type];
}

int adv_cov_merge(adv_cov *c, const adv_graph *g, const adv_hit *h, uint32_t n, int type,
                  adv_cov_delta *out, float new_m[ADV_T_N]) {
    if (type != ADV_T_BIKE && type != ADV_T_FOOT) return CB_E_INVALID;
    if (n == 0) return CB_OK;
    // Room for every hit to be new: one growth, then a backwards merge in place.
    if ((uint64_t)c->n + n > c->cap) {
        uint64_t want = (uint64_t)c->n + n;
        uint32_t cap = c->cap ? c->cap : 64;
        while (cap < want) cap = cap * 2;
        adv_cov_entry *e = realloc(c->e, (size_t)cap * sizeof *e);
        if (!e) return CB_E_NOMEM;
        c->e = e;
        c->cap = cap;
    }
    // Count the merged size first (the hits new to the table), then merge from the back so
    // nothing is overwritten before it is read.
    uint32_t i = 0, j = 0, extra = 0;
    while (i < c->n && j < n) {
        if (c->e[i].seg < h[j].seg) i++;
        else if (c->e[i].seg > h[j].seg) { j++; extra++; }
        else { i++; j++; }
    }
    extra += n - j;
    uint32_t total = c->n + extra;
    int32_t w = (int32_t)total - 1, a = (int32_t)c->n - 1, b = (int32_t)n - 1;
    while (b >= 0) {
        adv_cov_entry *dst = &c->e[w];
        if (a >= 0 && c->e[a].seg > h[b].seg) {
            *dst = c->e[a--];
        } else if (a >= 0 && c->e[a].seg == h[b].seg) {
            adv_cov_entry e = c->e[a--];
            uint16_t before_all = (uint16_t)(e.mask[0] | e.mask[1]);
            uint16_t nb = (uint16_t)(h[b].mask & ~e.mask[type]);
            uint16_t na = (uint16_t)(h[b].mask & ~before_all);
            e.mask[type] |= h[b].mask;
            float len = g->segs[h[b].seg].len_m;
            new_m[type] += adv_mask_len_m(nb, len);
            new_m[ADV_T_ALL] += adv_mask_len_m(na, len);
            if (out) {
                out[b].seg = h[b].seg;
                out[b].new_mask[type] = nb;
                out[b].new_mask[1 - type] = 0;
                out[b].new_mask[ADV_T_ALL] = na;
                out[b].pad = 0;
            }
            *dst = e;
            b--;
        } else {
            adv_cov_entry e = { h[b].seg, { 0, 0 } };
            e.mask[type] = h[b].mask;
            float len = g->segs[h[b].seg].len_m;
            float m = adv_mask_len_m(h[b].mask, len);
            new_m[type] += m;
            new_m[ADV_T_ALL] += m;
            if (out) {
                out[b].seg = h[b].seg;
                out[b].new_mask[type] = h[b].mask;
                out[b].new_mask[1 - type] = 0;
                out[b].new_mask[ADV_T_ALL] = h[b].mask;
                out[b].pad = 0;
            }
            *dst = e;
            b--;
        }
        w--;
    }
    // What is left of the old entries below w is already in place.
    c->n = total;
    return CB_OK;
}

void adv_cov_total_m(const adv_cov *c, const adv_graph *g, float out[ADV_T_N]) {
    double t[ADV_T_N] = { 0, 0, 0 };
    for (uint32_t i = 0; i < c->n; i++) {
        float len = g->segs[c->e[i].seg].len_m;
        t[0] += adv_mask_len_m(c->e[i].mask[0], len);
        t[1] += adv_mask_len_m(c->e[i].mask[1], len);
        t[2] += adv_mask_len_m((uint16_t)(c->e[i].mask[0] | c->e[i].mask[1]), len);
    }
    for (int k = 0; k < ADV_T_N; k++) out[k] = (float)t[k];
}

static int hit_cmp(const void *a, const void *b) {
    uint32_t x = ((const adv_hit *)a)->seg, y = ((const adv_hit *)b)->seg;
    return x < y ? -1 : x > y;
}

uint32_t adv_hits_finish(adv_hit *h, uint32_t n) {
    if (n == 0) return 0;
    qsort(h, n, sizeof *h, hit_cmp);
    uint32_t w = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (w > 0 && h[w - 1].seg == h[i].seg) h[w - 1].mask |= h[i].mask;
        else h[w++] = h[i];
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < w; i++)
        if (h[i].mask) { h[k] = h[i]; h[k].pad = 0; k++; }
    return k;
}

float adv_hits_len_m(const adv_graph *g, const adv_hit *h, uint32_t n) {
    double t = 0;
    for (uint32_t i = 0; i < n; i++) t += adv_mask_len_m(h[i].mask, g->segs[h[i].seg].len_m);
    return (float)t;
}

float adv_hits_common_m(const adv_graph *g, const adv_hit *a, uint32_t na, const adv_hit *b, uint32_t nb) {
    double t = 0;
    uint32_t i = 0, j = 0;
    while (i < na && j < nb) {
        if (a[i].seg < b[j].seg) i++;
        else if (a[i].seg > b[j].seg) j++;
        else {
            t += adv_mask_len_m((uint16_t)(a[i].mask & b[j].mask), g->segs[a[i].seg].len_m);
            i++;
            j++;
        }
    }
    return (float)t;
}
