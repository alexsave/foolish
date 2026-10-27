/* api_arena.c - the arena routes (wire.h has the table; arenas.h the arenas). Like the game routes
 * they run on the loop when it can take them and touch only memory under cc_lock; the read of an
 * arena no longer in memory goes to the pool (the journal). */
#include "api.h"
#include "arenas.h"
#include "journal.h"

#include "net/loop.h"

#include <string.h>

#define answer cc_api_answer
#define refuse cc_api_refuse

static bool on_loop(cb_ctx *ctx) { return ctx->loop && cb_loop_on_thread(ctx->loop); }

/* A refusal's HTTP status by its code. */
static int status_of(int code) {
    switch (code) {
    case CW_E_ARENA_NONE: return 404;
    case CW_E_ARENA_OVER: case CW_E_ARENA_JOINED: case CW_E_ARENA_NOT_JOINED: return 409;
    case CW_E_ARENA_GUEST: return 403;
    case CW_E_ARENA_TOO_MANY: return 429;
    case CW_E_ARENA_NAME: case CW_E_BAD: return 400;
    default: return 503;
    }
}

static void h_create(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_user u;
    if (!cc_api_caller(ctx, req, res, &u)) return;
    cw_arena_req q;
    if (req->body_len != sizeof q) { refuse(res, 400, CW_E_BAD, 0); return; }
    memcpy(&q, req->body, sizeof q);
    uint8_t buf[CW_ARENA_FRAME_MAX];
    size_t n = 0;
    pthread_mutex_lock(&cc_lock);
    int64_t wall = cc_wall();
    cc_player *p = cc_player_of(u.id, u.username);
    cc_arena *a = NULL;
    int code = p ? cc_arena_create(p, &q, wall, &a) : CW_E_BUSY;
    if (a) n = cc_arena_fill(a, p, wall, buf);
    pthread_mutex_unlock(&cc_lock);
    if (code != CW_E_NONE) refuse(res, status_of(code), code, 0);
    else answer(res, 200, buf, n);
}

/* POST /c/arena/join and /c/arena/withdraw: the arena's frame with the caller's place after it. */
static void join_or_withdraw(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res, bool join) {
    cb_user u;
    if (!cc_api_caller(ctx, req, res, &u)) return;
    cw_id q;
    if (req->body_len != sizeof q) { refuse(res, 400, CW_E_BAD, 0); return; }
    memcpy(&q, req->body, sizeof q);
    uint8_t buf[CW_ARENA_FRAME_MAX];
    size_t n = 0;
    pthread_mutex_lock(&cc_lock);
    int64_t wall = cc_wall();
    cc_player *p = cc_player_of(u.id, u.username);
    cc_arena *a = cc_arena_get(q.id);
    int code = !p ? CW_E_BUSY : !a ? CW_E_ARENA_NONE : join ? cc_arena_join(a, p) : cc_arena_withdraw(a, p);
    if (code == CW_E_NONE) n = cc_arena_fill(a, p, wall, buf);
    pthread_mutex_unlock(&cc_lock);
    if (code != CW_E_NONE) refuse(res, status_of(code), code, 0);
    else answer(res, 200, buf, n);
}

static void h_join(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) { join_or_withdraw(ctx, req, res, true); }
static void h_withdraw(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) { join_or_withdraw(ctx, req, res, false); }

static void h_arena(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    uint32_t id = cc_api_tail_id(req->path, "/c/arena/");
    if (!id) { refuse(res, 404, CW_E_ARENA_NONE, 0); return; }
    cb_user u;   /* the asker's own place when it is signed in; anyone may read */
    memset(&u, 0, sizeof u);
    bool signed_in = ctx->auth && cb_auth_verify_bearer(ctx->auth, req, &u) == CB_OK;
    uint8_t buf[CW_ARENA_FRAME_MAX];
    size_t n = 0;
    pthread_mutex_lock(&cc_lock);
    int64_t wall = cc_wall();
    cc_arena *a = cc_arena_get(id);
    if (a) n = cc_arena_fill(a, signed_in ? cc_player_get(u.id) : NULL, wall, buf);
    pthread_mutex_unlock(&cc_lock);
    if (!a) {
        if (on_loop(ctx)) { res->to_pool = true; return; }
        int rc = ctx->store ? cc_db_arena(ctx->store, id, signed_in ? u.id : NULL, wall, buf) : 0;
        if (rc < 0) { refuse(res, 503, CW_E_BUSY, 0); return; }
        if (rc == 0) { refuse(res, 404, CW_E_ARENA_NONE, 0); return; }
        n = (size_t)rc;
    }
    answer(res, 200, buf, n);
}

static void h_arenas(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    static cw_arena_list list;   /* under cc_lock */
    uint8_t buf[sizeof list];
    pthread_mutex_lock(&cc_lock);
    size_t n = cc_arena_list_fill(&list, cc_wall());
    memcpy(buf, &list, n);
    pthread_mutex_unlock(&cc_lock);
    answer(res, 200, buf, n);
}

#define GAME_ROUTE (CB_ROUTE_POOL | CB_ROUTE_INLINE | CB_ROUTE_HOME)

static const cb_route ROUTES[] = {
    { "POST", "/c/arena", GAME_ROUTE, h_create, 500 },
    { "POST", "/c/arena/join", GAME_ROUTE, h_join, 500 },
    { "POST", "/c/arena/withdraw", GAME_ROUTE, h_withdraw, 500 },
    { "GET", "/c/arena/*", GAME_ROUTE, h_arena, 500 },
    { "GET", "/c/arenas", GAME_ROUTE, h_arenas, 500 },
};

const cb_route_list cc_arena_routes = { ROUTES, (int)(sizeof ROUTES / sizeof *ROUTES) };
