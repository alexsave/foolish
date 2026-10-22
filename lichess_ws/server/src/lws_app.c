// lws_app.c - the gateway as the platform's app (cbase/app.h): its routes (routes.c), the hub
// opened on the loop at serve_open (hub.c), and a stats read for the smoke and the bench.
//
// Settings, beside the platform's CBASE_* (config.h; CBASE_LISTEN is the port, 9664 is lila-ws's):
//   LWS_REDIS_HOST, LWS_REDIS_PORT        127.0.0.1, 6379
//   LWS_MONGO_HOST, LWS_MONGO_PORT, LWS_MONGO_DB   127.0.0.1, 27017, lichess
//   LWS_CSRF_ORIGIN                       http://localhost:9663 (lila-ws's default; lila-docker sets LILA_URL)
//   LWS_COOKIE_NAME                       lila2
#include "cbase/app.h"

#include "lws.h"
#include "mongo.h"
#include "net/router.h"
#include "util/err.h"
#include "util/log.h"
#include "../../../shared/c/num_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const cb_route_list lws_route_list;
void lws_routes_configure(const char *csrf_origin, const char *cookie_name);
void lws_routes_close(void);

static const char *env_str(const char *name, const char *dflt) {
    const char *v = getenv(name);
    return v && *v ? v : dflt;
}
static int env_int(const char *name, int dflt) {
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    int64_t n;
    return num_parse_i64(v, strlen(v), 0, 65535, &n) ? (int)n : dflt;
}

static int lws_serve_open(cb_ctx *ctx, char *err, size_t cap) {
    mongo_cfg mc;
    memset(&mc, 0, sizeof mc);
    cb_strlcpy(mc.host, env_str("LWS_MONGO_HOST", "127.0.0.1"), sizeof mc.host);
    mc.port = env_int("LWS_MONGO_PORT", 27017);
    cb_strlcpy(mc.db, env_str("LWS_MONGO_DB", "lichess"), sizeof mc.db);
    mc.timeout_ms = 3000;
    mongo_thread_configure(&mc);
    lws_routes_configure(env_str("LWS_CSRF_ORIGIN", "http://localhost:9663"), env_str("LWS_COOKIE_NAME", "lila2"));
    const char *rh = env_str("LWS_REDIS_HOST", "127.0.0.1");
    int rp = env_int("LWS_REDIS_PORT", 6379);
    CB_INFO("lws", "redis %s:%d, mongodb %s:%d/%s", rh, rp, mc.host, mc.port, mc.db);
    return lws_hub_open(ctx, rh, rp, err, cap);
}

static void lws_serve_close(cb_ctx *ctx) {
    lws_hub_close();
    lws_routes_close();
}

// GET /lws/stats: one line of counters, on the loop (the hub's numbers are the loop's).
static void h_stats(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    lws_stats s;
    lws_hub_stats(&s);
    char line[256];
    snprintf(line, sizeof line, "conns=%ld users=%ld rooms=%ld rounds=%ld events=%ld published=%ld received=%ld dropped=%ld lila=%s redis=%s\n",
             s.conns, s.users, s.rooms, s.rounds_crowded, s.events, s.redis_published, s.redis_received, s.dropped_offline,
             s.lila_online ? "online" : "offline", s.redis_up ? "up" : "down");
    cb_res_text(res, 200, line);
}

static const cb_route STATS_ROUTES[] = { { "GET", "/lws/stats", CB_ROUTE_LOOP, h_stats, 0 } };
static const cb_route_list stats_route_list = { STATS_ROUTES, 1 };

static const cb_route_list *const LWS_ROUTES[] = { &lws_route_list, &stats_route_list };

static const cb_app LWS_APP = {
    .name = "lws",
    .routes = LWS_ROUTES,
    .n_route_lists = 2,
    .serve_open = lws_serve_open,
    .serve_close = lws_serve_close,
};

const cb_app *const cb_main_app = &LWS_APP;
