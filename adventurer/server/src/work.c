// work.c - work.h.
#include "work.h"

#include "feed.h"
#include "journal.h"
#include "match.h"
#include "server.h"
#include "trace.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/lanes.h"
#include "util/log.h"
#include "wire.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

// THE QUEUE IS THE PLATFORM'S (c_base util/lanes.h): two lanes, live (0) and backfill (1), each
// bounded by its bytes, over a pool of worker threads; this file is what a worker does with one
// upload. The lanes number the items in put order from the journal's next id, so an item's
// sequence number IS its activity id.
#define LANE_LIVE     0
#define LANE_BACKFILL 1

struct adv_work {
    cb_ctx    *ctx;
    adv_world *w;
    cb_lanes  *lanes;
    int        cpu_budget_us;
};

static void process(adv_work *wk, adv_match_scratch *scratch, const uint8_t *body, size_t len, uint32_t activity, bool backfill,
                    int64_t arrival_us);

// The matcher's scratch, one per worker thread, made on its first upload and freed when the
// thread ends (the pool's threads end when the lanes are freed).
static pthread_key_t g_scratch_key;
static pthread_once_t g_scratch_once = PTHREAD_ONCE_INIT;
static void scratch_free(void *p) { adv_match_scratch_free(p); }
static void scratch_key(void) { pthread_key_create(&g_scratch_key, scratch_free); }
static adv_match_scratch *thread_scratch(void) {
    pthread_once(&g_scratch_once, scratch_key);
    adv_match_scratch *s = pthread_getspecific(g_scratch_key);
    if (!s && (s = adv_match_scratch_new())) pthread_setspecific(g_scratch_key, s);
    return s;
}

static void run_one(void *ud, int lane, const uint8_t *bytes, size_t len, uint64_t seq, int64_t put_us) {
    adv_work *wk = ud;
    adv_match_scratch *scratch = thread_scratch();
    if (!scratch) {
        CB_ERROR("adv", "activity %llu: no memory for the matcher's scratch", (unsigned long long)seq);
        atomic_fetch_add(&wk->w->rejected, 1);
        return;
    }
    process(wk, scratch, bytes, len, (uint32_t)seq, lane == LANE_BACKFILL, put_us);
}

int adv_work_enqueue(adv_work *wk, const uint8_t *body, size_t len, bool backfill, uint32_t *activity, uint32_t *q_live,
                     uint32_t *q_backfill) {
    uint64_t seq = 0;
    int rc = cb_lanes_put(wk->lanes, backfill ? LANE_BACKFILL : LANE_LIVE, body, len, &seq);
    if (rc != CB_OK) return rc;
    *activity = (uint32_t)seq;
    // The world's next id follows the lanes' (a display reads it; puts race, so it only grows).
    uint32_t next = (uint32_t)seq + 1, cur = atomic_load(&wk->w->next_activity);
    while (cur < next && !atomic_compare_exchange_weak(&wk->w->next_activity, &cur, next)) {}
    adv_work_depths(wk, q_live, q_backfill);
    return CB_OK;
}

void adv_work_depths(const adv_work *wk, uint32_t *q_live, uint32_t *q_backfill) {
    *q_live = cb_lanes_depth(wk->lanes, LANE_LIVE);
    *q_backfill = cb_lanes_depth(wk->lanes, LANE_BACKFILL);
}

int adv_work_threads(const adv_work *wk) { return cb_lanes_threads(wk->lanes); }

// One upload, start to finish, on a worker thread.
static void process(adv_work *wk, adv_match_scratch *scratch, const uint8_t *body, size_t len, uint32_t activity, bool backfill,
                    int64_t arrival_us) {
    adv_trace t;
    adv_w_activity ev;
    memset(&ev, 0, sizeof ev);
    ev.v = ADV_W_V;
    ev.kind = ADV_W_KIND_ACTIVITY;
    ev.activity = activity;
    ev.flags = backfill ? ADV_AF_BACKFILL : 0;
    ev.at_ms = cb_now_ms();
    int64_t cpu0 = cb_thread_cpu_us();
    if (adv_trace_parse(body, len, &t) != CB_OK) {
        // The route parsed it already; a disagreement here is a bug, logged and counted as rejected.
        CB_WARN("adv", "activity %u: queued bytes do not parse", activity);
        ev.flags |= ADV_AF_REJECTED;
        atomic_fetch_add(&wk->w->rejected, 1);
        adv_feed_publish(wk->ctx, &ev, sizeof ev);
        return;
    }
    ev.user = t.hdr.user;
    ev.type = t.hdr.type;
    ev.gen_id = t.hdr.gen_id;
    ev.n_points = t.hdr.n_points;
    // The points, copied out of the unaligned body into the worker's own array.
    static _Thread_local adv_trace_pt pts[ADV_TRACE_MAX_PTS];
    for (uint32_t i = 0; i < t.hdr.n_points; i++) pts[i] = adv_trace_pt_at(&t, i);
    adv_match_opts mo = { .cpu_budget_us = wk->cpu_budget_us };
    adv_match_result mr;
    adv_match(wk->w->g, pts, t.hdr.n_points, t.hdr.type, &mo, scratch, &mr);
    if (mr.rc != ADV_MATCH_OK) {
        ev.flags |= ADV_AF_REJECTED;
        ev.reason = (uint8_t)mr.rc;
        ev.cpu_us = (uint32_t)(cb_thread_cpu_us() - cpu0);
        ev.e2e_us = (uint32_t)(cb_mono_us() - arrival_us);
        atomic_fetch_add(&wk->w->rejected, 1);
        adv_work_depths(wk, &ev.queue_live, &ev.queue_backfill);
        cb_metrics_observe(wk->ctx->metrics, ADV_M_ACT_CPU, ev.cpu_us);
        adv_feed_publish(wk->ctx, &ev, sizeof ev);
        return;
    }
    ev.matched_m = mr.matched_m;
    // Accuracy against the generator's truth, when it sent one.
    float accuracy = -1, false_m = 0;
    if (t.truth && t.n_truth) {
        static _Thread_local adv_hit truth[ADV_TRACE_MAX_TRUTH];
        for (uint32_t i = 0; i < t.n_truth; i++) {
            adv_truth x = adv_trace_truth_at(&t, i);
            truth[i].seg = x.seg;
            truth[i].mask = x.mask;
            truth[i].pad = 0;
        }
        // A truth naming a segment the graph does not have is the generator's bug; drop those.
        uint32_t k = 0;
        for (uint32_t i = 0; i < t.n_truth; i++) if (truth[i].seg < wk->w->g->h->n_segs) truth[k++] = truth[i];
        float truth_m = adv_hits_len_m(wk->w->g, truth, k);
        float common = adv_hits_common_m(wk->w->g, truth, k, mr.hits, mr.n_hits);
        accuracy = truth_m > 0 ? common / truth_m : 0;
        if (accuracy > 1) accuracy = 1;
        false_m = mr.matched_m - common;
        if (false_m < 0) false_m = 0;
        ev.flags |= ADV_AF_TRUTH;
        ev.accuracy = accuracy;
        ev.false_m = false_m;
    }
    adv_result r = {
        .activity = activity, .user = t.hdr.user, .type = t.hdr.type, .flags = t.hdr.flags,
        .start_ms = t.hdr.start_ms, .gen_id = t.hdr.gen_id, .n_points = t.hdr.n_points,
        .matched_m = mr.matched_m, .accuracy = accuracy, .cpu_us = mr.cpu_us, .hits = mr.hits, .n_hits = mr.n_hits,
    };
    adv_user *u = adv_world_user(wk->w, t.hdr.user);
    adv_applied a;
    int rc = CB_E_NOT_FOUND;
    if (u) {
        pthread_mutex_t *l = adv_world_user_lock(wk->w, t.hdr.user);
        pthread_mutex_lock(l);
        rc = adv_world_apply(wk->w, u, &r, &a);
        pthread_mutex_unlock(l);
    }
    if (rc != CB_OK) {
        CB_WARN("adv", "activity %u: apply failed: %s", activity, cb_strerror(rc));
        ev.flags |= ADV_AF_REJECTED;
        atomic_fetch_add(&wk->w->rejected, 1);
        adv_feed_publish(wk->ctx, &ev, sizeof ev);
        return;
    }
    r.cpu_us = (uint32_t)(cb_thread_cpu_us() - cpu0);   // match + merge: what the journal records
    if (accuracy >= 0) {
        pthread_mutex_lock(&wk->w->acc_lock);
        wk->w->acc_sum += accuracy;
        wk->w->acc_n++;
        pthread_mutex_unlock(&wk->w->acc_lock);
    }
    atomic_fetch_add(&wk->w->activities, 1);
    if (wk->ctx->store && (rc = adv_journal_act_put(wk->ctx->store, &r)) != CB_OK)
        CB_WARN("adv", "activity %u: journal write failed: %s (the result stays in memory)", activity, cb_strerror(rc));
    for (int k = 0; k < ADV_T_N; k++) ev.new_m[k] = a.new_m[k];
    ev.cpu_us = r.cpu_us;
    ev.e2e_us = (uint32_t)(cb_mono_us() - arrival_us);
    adv_work_depths(wk, &ev.queue_live, &ev.queue_backfill);
    cb_metrics_observe(wk->ctx->metrics, ADV_M_ACT_CPU, ev.cpu_us);
    // "Uploaded to visible" is a live upload's number: a backfill waits its turn by design.
    if (!backfill) cb_metrics_observe(wk->ctx->metrics, ADV_M_E2E, ev.e2e_us);
    cb_metrics_rate(wk->ctx->metrics, ADV_M_ACTS, cb_now_ms(), 1);
    adv_feed_publish(wk->ctx, &ev, sizeof ev);
}

int adv_work_start(adv_work **out, cb_ctx *ctx, adv_world *w, const adv_work_opts *o) {
    *out = NULL;
    adv_work *wk = calloc(1, sizeof *wk);
    if (!wk) return CB_E_NOMEM;
    wk->ctx = ctx;
    wk->w = w;
    wk->cpu_budget_us = o->cpu_budget_us;
    const size_t caps[2] = { o->live_cap_bytes ? o->live_cap_bytes : 64u << 20, o->backfill_cap_bytes ? o->backfill_cap_bytes : 256u << 20 };
    const cb_lanes_opts lo = {
        .threads = o->workers < 1 ? 1 : o->workers > ADV_WORK_MAX_WORKERS ? ADV_WORK_MAX_WORKERS : o->workers,
        .name = "adv-match", .n_lanes = 2, .caps = caps,
        .first_seq = atomic_load(&w->next_activity),   // the journal's next id (world.c starts it at 1)
        .run = run_one, .ud = wk,
    };
    int rc = cb_lanes_new(&wk->lanes, &lo);
    if (rc != CB_OK) { free(wk); return rc; }
    *out = wk;
    return CB_OK;
}

void adv_work_stop(adv_work *wk) {
    if (!wk) return;
    cb_lanes_free(wk->lanes);   // drops what is queued, waits for what runs
    free(wk);
}
