// routes.c - routes.h. Every answer is a fixed little-endian layout written by memcpy of the C
// struct; the hash in each head is be_wire_hash(). The handlers run on the request pool: they
// take the world's lock, and the session route runs bcrypt. The numbers are the platform's
// GET /metrics (be_metrics.h), not a route of this file.
#include "routes.h"

#include "auth/auth.h"
#include "store/game_id.h"
#include "net/router.h"
#include "util/err.h"
#include "util/log.h"
#include "wire.h"
#include "world.h"
#include "league.h"

#include <stdlib.h>
#include <string.h>

#define OCTETS "application/octet-stream"

static void answer(cb_http_res *res, const void *bytes, size_t len) {
    cb_buf_append(&res->body, bytes, len);
    if (res->body.oom) {
        cb_router_error(res, 500, "no memory");
        return;
    }
    res->status = 200;
    cb_res_header(res, "Content-Type", OCTETS);
    cb_res_header(res, "Cache-Control", "no-store");
}

// ---- GET /be/tables --------------------------------------------------------------------------

static void tables_get(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    void *buf = NULL;
    size_t n = be_world_list(&be_world_g, &buf);
    if (!n) {
        cb_router_error(res, 500, "no memory");
        return;
    }
    answer(res, buf, n);
    free(buf);
}

// ---- GET /be/table/<gid> ---------------------------------------------------------------------

static void table_get(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    const char *gid = req->path + strlen("/be/table/");
    size_t len = strlen(gid);
    if (!cb_game_id_ok(gid, len)) {
        cb_router_error(res, 400, "not a game id");
        return;
    }
    be_w_detail *d = malloc(sizeof *d);
    if (!d) {
        cb_router_error(res, 500, "no memory");
        return;
    }
    if (!be_world_detail(&be_world_g, gid, (int)len, d)) {
        free(d);
        cb_router_error(res, 404, "no such table");
        return;
    }
    answer(res, d, sizeof *d);
    free(d);
}

// ---- GET /be/league --------------------------------------------------------------------------



static void league_get(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    void *buf = NULL;
    size_t n = be_league_snapshot(&buf, be_mode);
    if (!n) {
        cb_router_error(res, 500, "no memory");
        return;
    }
    answer(res, buf, n);
    free(buf);
}

// ---- GET /be/tournament ----------------------------------------------------------------------

static void tournament_get(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    void *buf = NULL;
    size_t n = be_tour_snapshot(&buf);
    if (!n) {
        cb_router_error(res, 500, "no memory");
        return;
    }
    answer(res, buf, n);
    free(buf);
}

// ---- POST /be/session (dev mode) --------------------------------------------------------------
//
// The site's token for the stream's HELLO: a session of one fixed account on this process's own
// store, made through the platform's auth exactly as a sign-in is. Dev mode only: outside it the
// route is 404, as if it were not there. TEMPORARY until the platform's anonymous public-family
// subscription lands (then the site subscribes to s/ with no token and this route goes).

#define SITE_EMAIL    "site@birdseye.local"
#define SITE_NAME     "BIRDSEYE"
#define SITE_PASSWORD "birdseye-site-reads-the-room"

static void session_post(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!ctx->cfg || !ctx->cfg->dev || !ctx->auth) {
        cb_router_error(res, 404, "not found");
        return;
    }
    cb_session *s = calloc(1, sizeof *s);
    if (!s) {
        cb_router_error(res, 500, "no memory");
        return;
    }
    int rc = cb_auth_signin(ctx->auth, SITE_EMAIL, SITE_PASSWORD, s);
    if (rc == CB_AUTH_E_BAD_LOGIN) rc = cb_auth_signup(ctx->auth, SITE_EMAIL, SITE_PASSWORD, SITE_NAME, s);
    if (rc != CB_OK) {
        CB_WARN("birdseye", "the site's session: %s (%d)", cb_strerror(rc), rc);
        free(s);
        cb_router_error(res, 503, "no session");
        return;
    }
    be_w_session w;
    memset(&w, 0, sizeof w);
    w.hash = be_wire_hash();
    size_t n = strlen(s->access_token);
    if (n > BE_TOKEN_MAX) n = BE_TOKEN_MAX;
    w.token_len = (uint16_t)n;
    memcpy(w.token, s->access_token, n);
    free(s);
    answer(res, &w, sizeof w);
}

static const cb_route ROUTES[] = {
    { "GET", "/be/tables", CB_ROUTE_POOL, tables_get, 0 },
    { "GET", "/be/table/*", CB_ROUTE_POOL, table_get, 0 },
    { "POST", "/be/session", CB_ROUTE_POOL, session_post, 0 },
    { "GET", "/be/league", CB_ROUTE_POOL, league_get, 0 },
    { "GET", "/be/tournament", CB_ROUTE_POOL, tournament_get, 0 },
};
const cb_route_list be_route_list = { ROUTES, 5 };
