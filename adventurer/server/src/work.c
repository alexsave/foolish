// work.c - work.h.
#include "work.h"

#include "feed.h"
#include "journal.h"
#include "match.h"
#include "metrics.h"
#include "trace.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "wire.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct item {
    struct item *next;
    uint32_t     activity;
    int64_t      arrival_us;
    size_t       len;
    bool         backfill;
    uint8_t      body[];
} item;

typedef struct {
    item   *head, *tail;
    size_t  bytes, cap;
    uint32_t n;
} lane;

#define MAX_WORKERS 64

struct adv_work {
    cb_ctx         *ctx;
    adv_world      *w;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    lane            live, backfill;
    bool            stopping;
    int             n_threads;
    pthread_t       th[MAX_WORKERS];
    int             cpu_budget_us;
    atomic_uint     q_live, q_backfill;   // mirrors of the lanes' n, read without the lock
};

static void lane_push(lane *l, item *it) {
    it->next = NULL;
    if (l->tail) l->tail->next = it;
    else l->head = it;
    l->tail = it;
    l->bytes += it->len;
    l->n++;
}

static item *lane_pop(lane *l) {
    item *it = l->head;
    if (!it) return NULL;
    l->head = it->next;
    if (!l->head) l->tail = NULL;
    l->bytes -= it->len;
    l->n--;
    return it;
}

int adv_work_enqueue(adv_work *wk, const uint8_t *body, size_t len, bool backfill, int64_t arrival_us,
                     uint32_t *activity, uint32_t *q_live, uint32_t *q_backfill) {
    item *it = malloc(sizeof *it + len);
    if (!it) return CB_E_NOMEM;
    it->len = len;
    it->backfill = backfill;
    it->arrival_us = arrival_us;
    memcpy(it->body, body, len);
    pthread_mutex_lock(&wk->mu);
    lane *l = backfill ? &wk->backfill : &wk->live;
    if (wk->stopping || l->bytes + len > l->cap) {
        pthread_mutex_unlock(&wk->mu);
        free(it);
        return CB_E_FULL;
    }
    it->activity = atomic_fetch_add(&wk->w->next_activity, 1);
    lane_push(l, it);
    *activity = it->activity;
    *q_live = wk->live.n;
    *q_backfill = wk->backfill.n;
    atomic_store_explicit(&wk->q_live, wk->live.n, memory_order_relaxed);
    atomic_store_explicit(&wk->q_backfill, wk->backfill.n, memory_order_relaxed);
    pthread_cond_signal(&wk->cv);
    pthread_mutex_unlock(&wk->mu);
    return CB_OK;
}

void adv_work_depths(const adv_work *wk, uint32_t *q_live, uint32_t *q_backfill) {
    *q_live = atomic_load_explicit(&wk->q_live, memory_order_relaxed);
    *q_backfill = atomic_load_explicit(&wk->q_backfill, memory_order_relaxed);
}

int adv_work_threads(const adv_work *wk) { return wk->n_threads; }

// One upload, start to finish, on a worker thread.
static void process(adv_work *wk, adv_match_scratch *scratch, item *it) {
    adv_trace t;
    adv_w_activity ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = ADV_W_KIND_ACTIVITY;
    ev.activity = it->activity;
    ev.flags = it->backfill ? ADV_AF_BACKFILL : 0;
    ev.at_ms = cb_now_ms();
    int64_t cpu0 = cb_thread_cpu_us();
    if (adv_trace_parse(it->body, it->len, &t) != CB_OK) {
        // The route parsed it already; a disagreement here is a bug, logged and counted as rejected.
        CB_WARN("adv", "activity %u: queued bytes do not parse", it->activity);
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
        ev.e2e_us = (uint32_t)(cb_mono_us() - it->arrival_us);
        atomic_fetch_add(&wk->w->rejected, 1);
        adv_work_depths(wk, &ev.queue_live, &ev.queue_backfill);
        adv_hist_add(&adv_m.act_cpu, ev.cpu_us);
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
        .activity = it->activity, .user = t.hdr.user, .type = t.hdr.type, .flags = t.hdr.flags,
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
        CB_WARN("adv", "activity %u: apply failed: %s", it->activity, cb_strerror(rc));
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
        CB_WARN("adv", "activity %u: journal write failed: %s (the result stays in memory)", it->activity, cb_strerror(rc));
    for (int k = 0; k < ADV_T_N; k++) ev.new_m[k] = a.new_m[k];
    ev.cpu_us = r.cpu_us;
    ev.e2e_us = (uint32_t)(cb_mono_us() - it->arrival_us);
    adv_work_depths(wk, &ev.queue_live, &ev.queue_backfill);
    adv_hist_add(&adv_m.act_cpu, ev.cpu_us);
    // "Uploaded to visible" is a live upload's number: a backfill waits its turn by design.
    if (!it->backfill) adv_hist_add(&adv_m.e2e, ev.e2e_us);
    adv_rate_add(&adv_m.acts, cb_now_ms(), 1);
    adv_feed_publish(wk->ctx, &ev, sizeof ev);
}

static void *worker_main(void *ud) {
    adv_work *wk = ud;
    adv_match_scratch *scratch = adv_match_scratch_new();
    if (!scratch) {
        CB_ERROR("adv", "worker: no memory for the matcher's scratch");
        return NULL;
    }
    for (;;) {
        pthread_mutex_lock(&wk->mu);
        while (!wk->stopping && !wk->live.head && !wk->backfill.head) pthread_cond_wait(&wk->cv, &wk->mu);
        if (wk->stopping) {
            pthread_mutex_unlock(&wk->mu);
            break;
        }
        item *it = lane_pop(&wk->live);
        if (!it) it = lane_pop(&wk->backfill);
        atomic_store_explicit(&wk->q_live, wk->live.n, memory_order_relaxed);
        atomic_store_explicit(&wk->q_backfill, wk->backfill.n, memory_order_relaxed);
        pthread_mutex_unlock(&wk->mu);
        process(wk, scratch, it);
        free(it);
    }
    adv_match_scratch_free(scratch);
    return NULL;
}

int adv_work_start(adv_work **out, cb_ctx *ctx, adv_world *w, const adv_work_opts *o) {
    adv_work *wk = calloc(1, sizeof *wk);
    if (!wk) return CB_E_NOMEM;
    wk->ctx = ctx;
    wk->w = w;
    wk->live.cap = o->live_cap_bytes ? o->live_cap_bytes : 64u << 20;
    wk->backfill.cap = o->backfill_cap_bytes ? o->backfill_cap_bytes : 256u << 20;
    wk->cpu_budget_us = o->cpu_budget_us;
    pthread_mutex_init(&wk->mu, NULL);
    pthread_cond_init(&wk->cv, NULL);
    int n = o->workers < 1 ? 1 : o->workers > MAX_WORKERS ? MAX_WORKERS : o->workers;
    for (int i = 0; i < n; i++) {
        if (pthread_create(&wk->th[i], NULL, worker_main, wk) != 0) {
            wk->n_threads = i;
            adv_work_stop(wk);
            return CB_E_STATE;
        }
        wk->n_threads = i + 1;
    }
    *out = wk;
    return CB_OK;
}

void adv_work_stop(adv_work *wk) {
    if (!wk) return;
    pthread_mutex_lock(&wk->mu);
    wk->stopping = true;
    pthread_cond_broadcast(&wk->cv);
    pthread_mutex_unlock(&wk->mu);
    for (int i = 0; i < wk->n_threads; i++) pthread_join(wk->th[i], NULL);
    item *it;
    while ((it = lane_pop(&wk->live))) free(it);
    while ((it = lane_pop(&wk->backfill))) free(it);
    pthread_mutex_destroy(&wk->mu);
    pthread_cond_destroy(&wk->cv);
    free(wk);
}
