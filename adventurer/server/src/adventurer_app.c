// adventurer_app.c - adventurer as the platform's app (cbase/app.h): its routes, its journal
// tables, and serve_open, where the graph is mapped, the world rebuilt from the journal and the
// matcher workers started, all before the first request is served.
//
// Settings, beside the platform's CBASE_* (config.h):
//   ADV_GRAPH             the graph file (graph.h); required
//   ADV_WORKERS           matcher threads (default: the CPUs minus two, at least 1)
//   ADV_QUEUE_LIVE_MB     the live lane's cap (64); ADV_QUEUE_BACKFILL_MB the backfill lane's (256)
//   ADV_MATCH_BUDGET_MS   the matcher's CPU budget per activity (200)
#include "cbase/app.h"

#include "feed.h"
#include "journal.h"
#include "server.h"
#include "store/store.h"
#include "store/store_sql.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "../../../shared/c/num_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

adv_server adv_srv;

extern const char adv_schema_sql[];

static const cb_store_app ADV_STORE = {
    .sql_schema = adv_schema_sql,
    .sql_stmts = adv_sql_stmts,
    .n_sql_stmts = ADV_SQL_N,
};

static long env_long(const char *name, long dflt) {
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    int64_t n;
    return num_parse_i64(v, strlen(v), 0, INT32_MAX, &n) ? (long)n : dflt;
}

static int adv_serve_open(cb_ctx *ctx, char *err, size_t cap) {
    const char *path = getenv("ADV_GRAPH");
    if (!path || !*path) { snprintf(err, cap, "adventurer: ADV_GRAPH names no graph file (make -C adventurer/server graph)"); return CB_E_INVALID; }
    adv_srv.ctx = ctx;
    adv_srv.started_ms = cb_now_ms();
    int rc = adv_graph_open(path, &adv_srv.graph, err, cap);
    if (rc != CB_OK) return rc;
    const adv_graph_hdr *h = adv_srv.graph.h;
    CB_INFO("adv", "graph %s: %u segments, %u nodes, %u regions, %.0f km of road (%s)", path, h->n_segs, h->n_nodes, h->n_regions,
            h->total_len_m[ADV_T_ALL] / 1000, h->source);
    if ((rc = adv_world_init(&adv_srv.world, &adv_srv.graph)) != CB_OK) { snprintf(err, cap, "adventurer: no memory for the world"); return rc; }
    if (ctx->cfg) snprintf(adv_srv.db_path, sizeof adv_srv.db_path, "%s", ctx->cfg->db_path);
    if (ctx->store && cb_store_is_sql(ctx->store)) {
        int64_t t0 = cb_mono_ms();
        if ((rc = adv_journal_recover(ctx->store, &adv_srv.world, &adv_srv.recovered)) != CB_OK) {
            snprintf(err, cap, "adventurer: the journal does not replay: %s", cb_strerror(rc));
            return rc;
        }
        CB_INFO("adv", "journal: %u users, %u activities replayed in %lld ms", atomic_load(&adv_srv.world.n_users), adv_srv.recovered,
                (long long)(cb_mono_ms() - t0));
    }
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    adv_work_opts o = {
        .workers = (int)env_long("ADV_WORKERS", ncpu - 2 >= 1 ? ncpu - 2 : 1),
        .live_cap_bytes = (size_t)env_long("ADV_QUEUE_LIVE_MB", 64) << 20,
        .backfill_cap_bytes = (size_t)env_long("ADV_QUEUE_BACKFILL_MB", 256) << 20,
        .cpu_budget_us = (int)env_long("ADV_MATCH_BUDGET_MS", 200) * 1000,
    };
    if ((rc = adv_work_start(&adv_srv.work, ctx, &adv_srv.world, &o)) != CB_OK) { snprintf(err, cap, "adventurer: the workers did not start"); return rc; }
    CB_INFO("adv", "%d matcher worker(s), queues %zu MiB live / %zu MiB backfill, budget %d ms", adv_work_threads(adv_srv.work),
            o.live_cap_bytes >> 20, o.backfill_cap_bytes >> 20, o.cpu_budget_us / 1000);
    adv_feed_start_ops(ctx, adv_ops_fill);
    adv_srv.open = true;
    return CB_OK;
}

static void adv_serve_close(cb_ctx *ctx) {
    adv_srv.open = false;
    adv_work_stop(adv_srv.work);
    adv_srv.work = NULL;
    adv_world_free(&adv_srv.world);
    adv_graph_close(&adv_srv.graph);
}

static const cb_route_list *const ADV_ROUTES[] = { &adv_route_list, &adv_feed_route_list };

// server.h's ADV_M_* rows, in that order.
static const cb_metric_def ADV_METRICS[ADV_M_N] = {
    [ADV_M_ACT_CPU] = { "act_cpu_us", CB_METRIC_HIST },  [ADV_M_TILE] = { "tile_us", CB_METRIC_HIST },
    [ADV_M_E2E] = { "e2e_us", CB_METRIC_HIST },          [ADV_M_ACTS] = { "activities", CB_METRIC_RATE },
    [ADV_M_TILES] = { "tiles", CB_METRIC_RATE },         [ADV_M_TILES_304] = { "tiles_304", CB_METRIC_COUNTER },
    [ADV_M_FEED_CONNS] = { "feed_conns", CB_METRIC_GAUGE },
};

static const cb_app ADV_APP = {
    .name = "adventurer",
    .routes = ADV_ROUTES,
    .n_route_lists = 2,
    .store = &ADV_STORE,
    .serve_open = adv_serve_open,
    .serve_close = adv_serve_close,
    .metrics = ADV_METRICS,
    .n_metrics = ADV_M_N,
};

const cb_app *const cb_main_app = &ADV_APP;
