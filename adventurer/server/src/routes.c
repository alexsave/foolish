// routes.c - the product's HTTP surface (wire.h lists it). Every answer is a fixed little-endian
// layout written by memcpy of the C struct; the hash in each head is adv_wire_hash().
//
// WHERE EACH RUNS. Uploads, lists and stats run on the request pool: they take locks and the
// upload copies its body into the queue. Tiles are CB_ROUTE_INLINE | CB_ROUTE_HOME: rendered on
// the loop thread (the loop's CPU, never the workers') when the user's lock is free, and sent to
// the pool when a merge holds it, so a backfill in progress never adds to a tile's latency.
#include "server.h"
#include "../../../shared/c/proc.h"

#include "feed.h"
#include "journal.h"
#include "mvt.h"
#include "net/loop.h"
#include "trace.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "util/str.h"
#include "wire.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../shared/c/num_parse.h"

#define OCTETS "application/octet-stream"

static long env_mb(const char *name, long dflt) {
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    int64_t n;
    return num_parse_i64(v, strlen(v), 0, 65536, &n) ? (long)n : dflt;
}
#define MVT    "application/vnd.mapbox-vector-tile"

static bool ready(cb_http_res *res) {
    if (adv_srv.open) return true;
    cb_router_error(res, 503, "adventurer: not open");
    return false;
}

// A decimal path element: digits only, into *out; false otherwise. *rest points past it.
static bool path_u32(const char *s, uint32_t *out, const char **rest) {
    if (*s < '0' || *s > '9') return false;
    uint64_t v = 0;
    int n = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint64_t)(*s - '0');
        if (++n > 10 || v > UINT32_MAX) return false;
        s++;
    }
    *out = (uint32_t)v;
    *rest = s;
    return true;
}

// ---- POST /adv/users ------------------------------------------------------------------------

static void users_post(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!ready(res)) return;
    if (req->body_len < sizeof(adv_w_list)) { cb_router_error(res, 400, "short body"); return; }
    adv_w_list head;
    memcpy(&head, req->body, sizeof head);
    if (head.hash != adv_wire_hash()) { cb_router_error(res, 400, "wire hash mismatch"); return; }
    if (head.n > 4096 || req->body_len != sizeof head + (size_t)head.n * sizeof(adv_w_user_def)) {
        cb_router_error(res, 400, "count does not match body");
        return;
    }
    uint32_t ok = 0;
    for (uint32_t i = 0; i < head.n; i++) {
        adv_w_user_def d;
        memcpy(&d, req->body + sizeof head + (size_t)i * sizeof d, sizeof d);
        if (adv_world_user_define(&adv_srv.world, &d) != CB_OK) continue;
        if (ctx->store) adv_journal_user_put(ctx->store, &d);
        ok++;
    }
    if (ok != head.n) CB_WARN("adv", "users: %u of %u records refused", head.n - ok, head.n);
    cb_res_status(res, 204);
}

// ---- GET /adv/users -------------------------------------------------------------------------

static void users_get(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!ready(res)) return;
    adv_world *w = &adv_srv.world;
    cb_buf *b = &res->body;
    adv_w_list head = { adv_wire_hash(), 0 };
    cb_buf_append(b, &head, sizeof head);
    uint32_t n = 0;
    for (uint32_t id = 1; id < w->max_users; id++) {
        adv_user *u = &w->users[id];
        if (!u->id) continue;
        pthread_mutex_t *l = adv_world_user_lock(w, id);
        pthread_mutex_lock(l);
        adv_w_user r;
        memset(&r, 0, sizeof r);
        r.id = u->id;
        r.kind = u->kind;
        r.name_len = u->name_len;
        r.n_activities = u->n_activities;
        r.version = u->version;
        for (int t = 0; t < ADV_T_N; t++) r.unique_m[t] = u->unique_m[t];
        r.home_lat = u->home.lat;
        r.home_lon = u->home.lon;
        memcpy(r.name, u->name, ADV_W_NAME);
        pthread_mutex_unlock(l);
        cb_buf_append(b, &r, sizeof r);
        n++;
    }
    if (b->oom) { cb_router_error(res, 500, "no memory"); return; }
    head.n = n;
    memcpy(b->p, &head, sizeof head);
    res->status = 200;
    cb_res_header(res, "Content-Type", OCTETS);
    cb_res_header(res, "Cache-Control", "no-store");
}

// ---- GET /adv/regions -----------------------------------------------------------------------

static void regions_get(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!ready(res)) return;
    const adv_graph *g = &adv_srv.graph;
    adv_w_list head = { adv_wire_hash(), g->h->n_regions };
    cb_buf_append(&res->body, &head, sizeof head);
    for (uint32_t i = 0; i < g->h->n_regions; i++) {
        const adv_region *rg = &g->regions[i];
        adv_w_region r;
        memset(&r, 0, sizeof r);
        r.id = (uint16_t)i;
        r.name_len = (uint8_t)strnlen(rg->name, ADV_REGION_NAME - 1);
        r.level = (uint8_t)rg->level;
        for (int t = 0; t < ADV_T_N; t++) r.len_m[t] = rg->len_m[t];
        r.c_lat = rg->c_lat; r.c_lon = rg->c_lon;
        r.min_lat = rg->min_lat; r.min_lon = rg->min_lon; r.max_lat = rg->max_lat; r.max_lon = rg->max_lon;
        memcpy(r.name, rg->name, ADV_REGION_NAME);
        cb_buf_append(&res->body, &r, sizeof r);
    }
    res->status = 200;
    cb_res_header(res, "Content-Type", OCTETS);
    cb_res_header(res, "Cache-Control", "public, max-age=3600");
}

// ---- GET /adv/user/<id> ---------------------------------------------------------------------

static void user_get(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!ready(res)) return;
    uint32_t id;
    const char *rest;
    if (!path_u32(req->path + strlen("/adv/user/"), &id, &rest) || *rest) { cb_router_error(res, 404, "no such user"); return; }
    adv_world *w = &adv_srv.world;
    adv_user *u = adv_world_user(w, id);
    if (!u) { cb_router_error(res, 404, "no such user"); return; }
    adv_w_user_stats s;
    memset(&s, 0, sizeof s);
    static _Thread_local adv_rprog rp[ADV_MAX_REGIONS];
    pthread_mutex_t *l = adv_world_user_lock(w, id);
    pthread_mutex_lock(l);
    s.hash = adv_wire_hash();
    s.id = u->id;
    s.version = u->version;
    s.n_activities = u->n_activities;
    s.n_regions = u->n_rp;
    for (int t = 0; t < ADV_T_N; t++) s.unique_m[t] = u->unique_m[t];
    s.last_activity_ms = u->last_ms;
    s.first_activity_ms = u->first_ms;
    adv_world_super_unique(w, u, &s.n_super_unique_segs, &s.super_unique_m);
    memcpy(rp, u->rp, (size_t)u->n_rp * sizeof *rp);
    pthread_mutex_unlock(l);
    cb_buf_append(&res->body, &s, sizeof s);
    uint32_t mk = adv_month_key(cb_now_ms()), yk = adv_year_key(cb_now_ms());
    pthread_mutex_lock(&w->boards_lock);
    for (uint32_t i = 0; i < s.n_regions; i++) {
        adv_w_region_progress p;
        memset(&p, 0, sizeof p);
        p.region = rp[i].region;
        for (int t = 0; t < ADV_T_N; t++) {
            p.achievements[t] = rp[i].ach[t];
            p.len_m[t] = rp[i].len_m[t];
            p.month_m[t] = rp[i].month_key == mk ? rp[i].month_m[t] : 0;
            p.year_m[t] = rp[i].year_key == yk ? rp[i].year_m[t] : 0;
            p.rank_month[t] = (uint8_t)adv_world_rank(w, p.region, false, t, id);
            p.rank_year[t] = (uint8_t)adv_world_rank(w, p.region, true, t, id);
        }
        cb_buf_append(&res->body, &p, sizeof p);
    }
    pthread_mutex_unlock(&w->boards_lock);
    res->status = 200;
    cb_res_header(res, "Content-Type", OCTETS);
    cb_res_header(res, "Cache-Control", "no-store");
}

// ---- GET /adv/leaders/<region> --------------------------------------------------------------

static void leaders_get(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!ready(res)) return;
    uint32_t id;
    const char *rest;
    if (!path_u32(req->path + strlen("/adv/leaders/"), &id, &rest) || *rest || id >= adv_srv.graph.h->n_regions) {
        cb_router_error(res, 404, "no such region");
        return;
    }
    adv_board b;
    adv_world_board_copy(&adv_srv.world, (uint16_t)id, &b);
    adv_w_leaders out;
    memset(&out, 0, sizeof out);
    out.hash = adv_wire_hash();
    out.region = (uint16_t)id;
    uint32_t mk = adv_month_key(cb_now_ms()), yk = adv_year_key(cb_now_ms());
    if (b.month_key == mk) memcpy(out.month, b.month, sizeof out.month);
    if (b.year_key == yk) memcpy(out.year, b.year, sizeof out.year);
    cb_res_send(res, 200, OCTETS, &out, sizeof out);
    cb_res_header(res, "Cache-Control", "no-store");
}

// ---- GET /adv/ops ---------------------------------------------------------------------------

void adv_ops_fill(adv_w_ops *o) {
    memset(o, 0, sizeof *o);
    o->kind = ADV_W_KIND_OPS;
    o->hash = adv_wire_hash();
    int64_t now = cb_now_ms();
    o->at_ms = now;
    o->uptime_s = (uint32_t)((now - adv_srv.started_ms) / 1000);
    adv_world *w = &adv_srv.world;
    o->users = atomic_load(&w->n_users);
    o->activities = (uint32_t)atomic_load(&w->activities);
    o->rejected = (uint32_t)atomic_load(&w->rejected);
    if (adv_srv.work) adv_work_depths(adv_srv.work, &o->queue_live, &o->queue_backfill);
    const cb_metrics *m = adv_srv.ctx ? adv_srv.ctx->metrics : NULL;
    o->act_per_s = cb_metrics_per_s(m, ADV_M_ACTS, now);
    o->tiles_per_s = cb_metrics_per_s(m, ADV_M_TILES, now);
    o->act_cpu_p50_us = cb_metrics_pct(m, ADV_M_ACT_CPU, 50);
    o->act_cpu_p99_us = cb_metrics_pct(m, ADV_M_ACT_CPU, 99);
    o->tile_p50_us = cb_metrics_pct(m, ADV_M_TILE, 50);
    o->tile_p99_us = cb_metrics_pct(m, ADV_M_TILE, 99);
    o->e2e_p50_us = cb_metrics_pct(m, ADV_M_E2E, 50);
    o->e2e_p99_us = cb_metrics_pct(m, ADV_M_E2E, 99);
    o->rss_mb = proc_rss_mb(0);
    o->feed_conns = adv_feed_conns();
    o->http_conns = (uint32_t)atomic_load(&adv_srv.ctx->stats.http_conns);
    o->journal_bytes = adv_journal_bytes(adv_srv.db_path);
    pthread_mutex_lock(&w->acc_lock);
    o->accuracy_avg = w->acc_n ? (float)(w->acc_sum / (double)w->acc_n) : 0;
    pthread_mutex_unlock(&w->acc_lock);
    o->segs = adv_srv.graph.h ? adv_srv.graph.h->n_segs : 0;
    o->regions = adv_srv.graph.h ? adv_srv.graph.h->n_regions : 0;
    for (int t = 0; t < ADV_T_N; t++) o->total_len_km[t] = adv_srv.graph.h ? (float)(adv_srv.graph.h->total_len_m[t] / 1000) : 0;
    o->threads = (uint32_t)(adv_srv.work ? adv_work_threads(adv_srv.work) : 0) + 2;
    o->recovered = adv_srv.recovered;
}

static void ops_get(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!ready(res)) return;
    adv_w_ops o;
    adv_ops_fill(&o);
    cb_res_send(res, 200, OCTETS, &o, sizeof o);
    cb_res_header(res, "Cache-Control", "no-store");
}

// ---- POST /adv/upload -----------------------------------------------------------------------

static void upload_post(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!ready(res)) return;
    int64_t arrival = cb_mono_us();
    adv_trace t;
    if (adv_trace_parse(req->body, req->body_len, &t) != CB_OK) { cb_router_error(res, 400, "not a trace"); return; }
    if (!adv_world_user(&adv_srv.world, t.hdr.user)) { cb_router_error(res, 404, "no such user"); return; }
    adv_w_accepted a = { adv_wire_hash(), 0, 0, 0 };
    int rc = adv_work_enqueue(adv_srv.work, req->body, req->body_len, (t.hdr.flags & ADV_TRF_BACKFILL) != 0, arrival,
                              &a.activity, &a.queue_live, &a.queue_backfill);
    if (rc == CB_E_FULL) {
        cb_res_header(res, "Retry-After", "1");
        cb_router_error(res, 503, "queue full");
        return;
    }
    if (rc != CB_OK) { cb_router_error(res, 500, cb_strerror(rc)); return; }
    cb_res_send(res, 202, OCTETS, &a, sizeof a);
}

// ---- tiles ----------------------------------------------------------------------------------

static _Thread_local adv_tile_scratch *g_ts;

static adv_tile_scratch *tile_scratch(void) {
    if (!g_ts) {
        g_ts = calloc(1, sizeof *g_ts);
        if (g_ts && adv_tile_scratch_init(g_ts, &adv_srv.graph) != CB_OK) { free(g_ts); g_ts = NULL; }
    }
    return g_ts;
}

static int type_of_query(const cb_http_req *req) {
    char t[16];
    if (!cb_http_query_get(req, "t", t, sizeof t)) return ADV_T_ALL;
    if (strcmp(t, "bike") == 0) return ADV_T_BIKE;
    if (strcmp(t, "foot") == 0) return ADV_T_FOOT;
    return ADV_T_ALL;
}

// "<z>/<x>/<y>.mvt" at s: the three numbers, or false.
static bool parse_zxy_mvt(const char *s, int *z, uint32_t *x, uint32_t *y) {
    size_t n = strlen(s);
    if (n < 9 || n > 40 || strcmp(s + n - 4, ".mvt") != 0) return false;
    char buf[48];
    memcpy(buf, s, n - 4);
    buf[n - 4] = 0;
    return adv_tile_parse_zxy(buf, z, x, y);
}

static void tile_answer(cb_http_res *res, const uint8_t *bytes, int len, const char *etag, const char *cache) {
    cb_res_send(res, 200, MVT, bytes, (size_t)len);
    cb_res_header(res, "ETag", etag);
    cb_res_header(res, "Cache-Control", cache);
}

static bool not_modified(const cb_http_req *req, cb_http_res *res, const char *etag, const char *cache) {
    const char *inm = cb_http_header_get(req, "If-None-Match");
    if (!inm || strcmp(inm, etag) != 0) return false;
    cb_res_status(res, 304);
    cb_res_header(res, "ETag", etag);
    cb_res_header(res, "Cache-Control", cache);
    cb_metrics_add(adv_srv.ctx->metrics, ADV_M_TILES_304, 1);
    return true;
}

// A render that left features out for want of scratch is logged (once in a while, not per tile).
static void note_dropped(const adv_tile_scratch *s, const adv_tile_req *tr) {
    static atomic_uint logged;
    if (s->dropped && atomic_fetch_add(&logged, 1) % 100 == 0)
        CB_WARN("adv", "tile %d/%u/%u: %u features left out (scratch %u MiB)", tr->z, tr->x, tr->y, s->dropped, (unsigned)(ADV_TILE_SCRATCH >> 20));
}

// THE BASE TILE CACHE: the roads layer depends on the graph alone, so a base tile is rendered
// once and kept; a z11 city tile is half a megabyte and 10 ms, and the map asks for the same
// low-zoom tiles from every visitor. Open addressing over the tile's key, bounded by bytes
// (ADV_BASE_CACHE_MB, 64): past the cap a rover frees entries until the new one fits. The
// lock is tried on the loop thread (a miss renders on the pool).
#define BT_SLOTS 8192
#define BT_PROBE 8
typedef struct {
    uint64_t key;    // 1 + (z << 58 | x << 29 | y); 0: empty
    uint8_t *bytes;
    uint32_t len;
} bt_entry;
static bt_entry g_bt[BT_SLOTS];
static size_t g_bt_bytes, g_bt_cap;
static uint32_t g_bt_rover;
static pthread_mutex_t g_bt_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_ullong g_bt_hits, g_bt_misses;

static uint64_t bt_key(const adv_tile_req *t) { return 1 + ((uint64_t)t->z << 58 | (uint64_t)t->x << 29 | (uint64_t)t->y); }
static uint32_t bt_slot(uint64_t key) { return (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 40) % BT_SLOTS; }

// Under the lock: the entry for key, or NULL.
static bt_entry *bt_find(uint64_t key) {
    uint32_t s = bt_slot(key);
    for (int i = 0; i < BT_PROBE; i++) {
        bt_entry *e = &g_bt[(s + i) % BT_SLOTS];
        if (e->key == key) return e;
    }
    return NULL;
}

static void bt_free(bt_entry *e) {
    g_bt_bytes -= e->len;
    free(e->bytes);
    memset(e, 0, sizeof *e);
}

// Under the lock: keeps a copy of the tile (best effort: no memory, no entry).
static void bt_put(uint64_t key, const uint8_t *bytes, uint32_t len) {
    if (!g_bt_cap) g_bt_cap = (size_t)env_mb("ADV_BASE_CACHE_MB", 64) << 20;
    if (len > g_bt_cap / 4) return;
    while (g_bt_bytes + len > g_bt_cap) {   // the rover frees whatever it meets
        bt_entry *e = &g_bt[g_bt_rover++ % BT_SLOTS];
        if (e->key) bt_free(e);
    }
    uint32_t s = bt_slot(key);
    bt_entry *at = NULL;
    for (int i = 0; i < BT_PROBE && !at; i++)
        if (!g_bt[(s + i) % BT_SLOTS].key) at = &g_bt[(s + i) % BT_SLOTS];
    if (!at) {   // every probe slot taken: the first gives way
        at = &g_bt[s];
        bt_free(at);
    }
    at->bytes = malloc(len ? len : 1);
    if (!at->bytes) { memset(at, 0, sizeof *at); return; }
    memcpy(at->bytes, bytes, len);
    at->key = key;
    at->len = len;
    g_bt_bytes += len;
}

static void base_tile(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!ready(res)) return;
    adv_tile_req tr = { .roads = true, .type = ADV_T_ALL };
    if (!parse_zxy_mvt(req->path + strlen("/adv/base/"), &tr.z, &tr.x, &tr.y)) { cb_router_error(res, 404, "not a tile"); return; }
    if (tr.z > ADV_TILE_MAX_ZOOM || tr.x >> tr.z || tr.y >> tr.z) { cb_router_error(res, 404, "not a tile"); return; }
    char etag[64];
    snprintf(etag, sizeof etag, "\"base.%08x\"", adv_srv.graph.h->crc);
    static const char CACHE[] = "public, max-age=3600";
    if (not_modified(req, res, etag, CACHE)) return;
    bool on_loop = ctx->loop && cb_loop_on_thread(ctx->loop);
    uint64_t key = bt_key(&tr);
    if (on_loop) {
        if (pthread_mutex_trylock(&g_bt_mu) != 0) { res->to_pool = true; return; }
    } else {
        pthread_mutex_lock(&g_bt_mu);
    }
    bt_entry *e = bt_find(key);
    if (e) {
        tile_answer(res, e->bytes, (int)e->len, etag, CACHE);
        pthread_mutex_unlock(&g_bt_mu);
        atomic_fetch_add(&g_bt_hits, 1);
        cb_metrics_rate(ctx->metrics, ADV_M_TILES, cb_now_ms(), 1);
        return;
    }
    pthread_mutex_unlock(&g_bt_mu);
    if (on_loop) { res->to_pool = true; return; }   // a render is the pool's: the loop keeps serving
    adv_tile_scratch *s = tile_scratch();
    if (!s) { cb_router_error(res, 500, "no memory"); return; }
    int64_t t0 = cb_mono_us();
    const uint8_t *bytes;
    int n = adv_tile_render(&adv_srv.graph, &tr, 0, NULL, NULL, s, &bytes);
    if (n < 0) { cb_router_error(res, n == CB_E_INVALID ? 404 : 500, cb_strerror(n)); return; }
    note_dropped(s, &tr);
    if (n > 0) cb_metrics_observe(ctx->metrics, ADV_M_TILE, (uint64_t)(cb_mono_us() - t0));   // an empty tile (the sea) is no measure of a render
    cb_metrics_rate(ctx->metrics, ADV_M_TILES, cb_now_ms(), 1);
    atomic_fetch_add(&g_bt_misses, 1);
    pthread_mutex_lock(&g_bt_mu);
    if (!bt_find(key)) bt_put(key, bytes, (uint32_t)n);
    pthread_mutex_unlock(&g_bt_mu);
    tile_answer(res, bytes, n, etag, CACHE);
}

static void user_tile(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!ready(res)) return;
    uint32_t id;
    const char *rest;
    if (!path_u32(req->path + strlen("/adv/tile/"), &id, &rest) || *rest != '/') { cb_router_error(res, 404, "not a tile"); return; }
    adv_tile_req tr = { .traveled = true, .untraveled = true, .unique = true };
    if (!parse_zxy_mvt(rest + 1, &tr.z, &tr.x, &tr.y)) { cb_router_error(res, 404, "not a tile"); return; }
    tr.type = type_of_query(req);
    adv_world *w = &adv_srv.world;
    adv_user *u = adv_world_user(w, id);
    if (!u) { cb_router_error(res, 404, "no such user"); return; }
    adv_tile_scratch *s = tile_scratch();
    if (!s) { cb_router_error(res, 500, "no memory"); return; }
    static const char CACHE[] = "private, max-age=2";
    pthread_mutex_t *l = adv_world_user_lock(w, id);
    bool on_loop = ctx->loop && cb_loop_on_thread(ctx->loop);
    if (on_loop) {
        if (pthread_mutex_trylock(l) != 0) { res->to_pool = true; return; }   // a merge holds it: the pool waits, not the loop
    } else {
        pthread_mutex_lock(l);
    }
    char etag[64];
    snprintf(etag, sizeof etag, "\"%u.%u.%d\"", id, u->version, tr.type);
    if (not_modified(req, res, etag, CACHE)) { pthread_mutex_unlock(l); return; }
    int64_t t0 = cb_mono_us();
    const uint8_t *bytes;
    int n = adv_tile_render(&adv_srv.graph, &tr, id, &u->cov, (const uint32_t *)w->owners, s, &bytes);
    pthread_mutex_unlock(l);
    if (n < 0) { cb_router_error(res, n == CB_E_INVALID ? 404 : 500, cb_strerror(n)); return; }
    note_dropped(s, &tr);
    if (n > 0) cb_metrics_observe(ctx->metrics, ADV_M_TILE, (uint64_t)(cb_mono_us() - t0));   // an empty tile (the sea) is no measure of a render
    cb_metrics_rate(ctx->metrics, ADV_M_TILES, cb_now_ms(), 1);
    tile_answer(res, bytes, n, etag, CACHE);
}

static const cb_route ROUTES[] = {
    { "POST", "/adv/users", CB_ROUTE_POOL, users_post, 0 },
    { "GET", "/adv/users", CB_ROUTE_POOL, users_get, 0 },
    { "GET", "/adv/regions", CB_ROUTE_POOL, regions_get, 0 },
    { "GET", "/adv/user/*", CB_ROUTE_POOL, user_get, 0 },
    { "GET", "/adv/leaders/*", CB_ROUTE_POOL, leaders_get, 0 },
    { "GET", "/adv/ops", CB_ROUTE_POOL, ops_get, 0 },
    { "POST", "/adv/upload", CB_ROUTE_POOL, upload_post, 0 },
    { "GET", "/adv/base/*", CB_ROUTE_INLINE | CB_ROUTE_HOME, base_tile, 4000 },
    { "GET", "/adv/tile/*", CB_ROUTE_INLINE | CB_ROUTE_HOME, user_tile, 4000 },
};

const cb_route_list adv_route_list = { ROUTES, (int)(sizeof ROUTES / sizeof ROUTES[0]) };
