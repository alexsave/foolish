// trace.c - trace.h: the upload's reader and writer.
#include "trace.h"

#include "util/err.h"

#include <string.h>

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

#define HDR_BYTES 32
#define PT_BYTES  12
#define TRUTH_BYTES 8
_Static_assert(sizeof(adv_trace_hdr) == HDR_BYTES, "adv_trace_hdr");
_Static_assert(sizeof(adv_trace_pt) == PT_BYTES, "adv_trace_pt");
_Static_assert(sizeof(adv_truth) == TRUTH_BYTES, "adv_truth");

int adv_trace_parse(const uint8_t *body, size_t len, adv_trace *out) {
    memset(out, 0, sizeof *out);
    if (!body || len < HDR_BYTES) return CB_E_INVALID;
    adv_trace_hdr h;
    h.magic = rd32(body);
    h.version = rd16(body + 4);
    h.type = body[6];
    h.flags = body[7];
    h.user = rd32(body + 8);
    h.n_points = rd32(body + 12);
    h.start_ms = (int64_t)rd64(body + 16);
    h.gen_id = rd32(body + 24);
    h.reserved = rd32(body + 28);
    if (h.magic != ADV_TRACE_MAGIC || h.version != ADV_TRACE_VERSION) return CB_E_INVALID;
    if (h.type < ADV_ACT_BIKE || h.type > ADV_ACT_WALK) return CB_E_INVALID;
    if (h.flags & ~(ADV_TRF_BACKFILL | ADV_TRF_TRUTH)) return CB_E_INVALID;
    if (h.user == 0 || h.n_points > ADV_TRACE_MAX_PTS) return CB_E_INVALID;
    size_t at = HDR_BYTES;
    size_t pts_bytes = (size_t)h.n_points * PT_BYTES;
    if (len - at < pts_bytes) return CB_E_INVALID;
    out->pts = (const adv_trace_pt *)(body + at);
    at += pts_bytes;
    if (h.flags & ADV_TRF_TRUTH) {
        if (len - at < 4) return CB_E_INVALID;
        uint32_t n = rd32(body + at);
        at += 4;
        if (n > ADV_TRACE_MAX_TRUTH || len - at < (size_t)n * TRUTH_BYTES) return CB_E_INVALID;
        out->truth = (const adv_truth *)(body + at);
        out->n_truth = n;
        at += (size_t)n * TRUTH_BYTES;
        uint32_t prev = 0;
        for (uint32_t i = 0; i < n; i++) {
            adv_truth t = adv_trace_truth_at(out, i);
            if (i > 0 && t.seg <= prev) return CB_E_INVALID;   // sorted, unique
            if (t.mask == 0) return CB_E_INVALID;
            prev = t.seg;
        }
    }
    if (at != len) return CB_E_INVALID;
    out->hdr = h;
    return CB_OK;
}

adv_trace_pt adv_trace_pt_at(const adv_trace *t, uint32_t i) {
    const uint8_t *p = (const uint8_t *)t->pts + (size_t)i * PT_BYTES;
    adv_trace_pt r;
    r.lat = (int32_t)rd32(p);
    r.lon = (int32_t)rd32(p + 4);
    r.t_ms = rd32(p + 8);
    return r;
}

adv_truth adv_trace_truth_at(const adv_trace *t, uint32_t i) {
    const uint8_t *p = (const uint8_t *)t->truth + (size_t)i * TRUTH_BYTES;
    adv_truth r;
    r.seg = rd32(p);
    r.mask = rd16(p + 4);
    r.pad = rd16(p + 6);
    return r;
}

size_t adv_trace_size(uint32_t n_points, uint32_t n_truth) {
    if (n_points > ADV_TRACE_MAX_PTS || n_truth > ADV_TRACE_MAX_TRUTH) return 0;
    return HDR_BYTES + (size_t)n_points * PT_BYTES + (n_truth ? 4 + (size_t)n_truth * TRUTH_BYTES : 0);
}

size_t adv_trace_write(uint8_t *out, size_t cap, const adv_trace_hdr *h, const adv_trace_pt *pts,
                       const adv_truth *truth, uint32_t n_truth) {
    size_t need = adv_trace_size(h->n_points, n_truth);
    if (!need || need > cap) return 0;
    uint8_t *p = out;
    wr32(p, ADV_TRACE_MAGIC);
    wr16(p + 4, ADV_TRACE_VERSION);
    p[6] = h->type;
    p[7] = (uint8_t)((h->flags & ADV_TRF_BACKFILL) | (n_truth ? ADV_TRF_TRUTH : 0));
    wr32(p + 8, h->user);
    wr32(p + 12, h->n_points);
    wr64(p + 16, (uint64_t)h->start_ms);
    wr32(p + 24, h->gen_id);
    wr32(p + 28, 0);
    p += HDR_BYTES;
    for (uint32_t i = 0; i < h->n_points; i++, p += PT_BYTES) {
        wr32(p, (uint32_t)pts[i].lat);
        wr32(p + 4, (uint32_t)pts[i].lon);
        wr32(p + 8, pts[i].t_ms);
    }
    if (n_truth) {
        wr32(p, n_truth);
        p += 4;
        for (uint32_t i = 0; i < n_truth; i++, p += TRUTH_BYTES) {
            wr32(p, truth[i].seg);
            wr16(p + 4, truth[i].mask);
            wr16(p + 6, 0);
        }
    }
    return need;
}
