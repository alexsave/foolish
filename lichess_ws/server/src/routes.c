// routes.c - the request-pool half of the gateway: the WebSocket endpoints of lila-ws's Router,
// what each checks before the upgrade (Controller, Auth, Mongo), and the upgrade itself, which
// is pure (net/ws.h cb_ws_upgrade) so the loop takes the socket over once the 101 is written.
//
// The MongoDB reads block only this pool thread (one connection per thread, mongo.h); the caches
// lila-ws keeps in front of them (the game's players for 10 minutes, the troll mark for 15) are
// kept here too, behind one mutex, so the race pays the same reads as the real thing does.
#include "lws.h"

#include "json_wire.h"
#include "mongo.h"
#include "cbase/app.h"
#include "net/router.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "util/uuid.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOD "lws"

typedef struct {
    char csrf_origin[256];
    char cookie[32];
} routes_cfg;
static routes_cfg CFG = { .csrf_origin = "http://localhost:9663", .cookie = "lila2" };

void lws_routes_configure(const char *csrf_origin, const char *cookie_name) {
    if (csrf_origin) cb_strlcpy(CFG.csrf_origin, csrf_origin, sizeof CFG.csrf_origin);
    if (cookie_name) cb_strlcpy(CFG.cookie, cookie_name, sizeof CFG.cookie);
}

// ---- the caches (Mongo.scala's gameCache, troll cache; LightUserApi) ----

typedef struct {
    char    player_id[2][5];
    char    user[2][LWS_USER_MAX];   // "" anonymous
    char    ext_kind;                // 't', 'w', 'm' or 0
    char    ext_id[16];
    int64_t at_ms;
    bool    missing;                 // no such game (cached too)
} game_rec;
typedef struct { bool troll; int64_t at_ms; } troll_rec;
typedef struct { char name[LWS_USER_MAX + 8]; int64_t at_ms; } name_rec;

static pthread_mutex_t cache_mu = PTHREAD_MUTEX_INITIALIZER;
static smap game_cache, troll_cache, name_cache;
static bool caches_init;
#define GAME_TTL_MS (10 * 60 * 1000)
#define TROLL_TTL_MS (15 * 60 * 1000)
#define NAME_TTL_MS (10 * 60 * 1000)
#define GAME_CACHE_MAX 250000

static void caches_ensure(void) {
    if (caches_init) return;
    smap_init(&game_cache); smap_init(&troll_cache); smap_init(&name_cache);
    caches_init = true;
}

static bool free_each(const char *key, void *val, void *ud) { free(val); return true; }
void lws_routes_close(void) {
    pthread_mutex_lock(&cache_mu);
    if (caches_init) {
        smap_each(&game_cache, free_each, NULL); smap_free(&game_cache);
        smap_each(&troll_cache, free_each, NULL); smap_free(&troll_cache);
        smap_each(&name_cache, free_each, NULL); smap_free(&name_cache);
        caches_init = false;
    }
    pthread_mutex_unlock(&cache_mu);
}

// ---- the reads ----

static int find_one(const char *coll, const cb_buf *filter, const cb_buf *proj, cb_buf *doc) {
    mongo_conn *m = mongo_thread_conn();
    if (!m) return CB_E_IO;
    int rc = mongo_find_one(m, coll, filter, proj, doc);
    if (rc == CB_E_IO) {
        mongo_thread_drop();
        m = mongo_thread_conn();   // one retry on a fresh connection
        if (m) rc = mongo_find_one(m, coll, filter, proj, doc);
        if (rc == CB_E_IO) mongo_thread_drop();
    }
    return rc;
}

// security: {_id: sid, up: true} -> user
static int session_user(const char *sid, char *user, size_t cap) {
    cb_buf f, p, doc;
    cb_buf_init(&f); cb_buf_init(&p); cb_buf_init(&doc);
    bson_w w;
    bson_w_init(&w, &f); bson_w_begin(&w); bson_w_str(&w, "_id", sid); bson_w_bool(&w, "up", true); bson_w_end(&w);
    bson_w_init(&w, &p); bson_w_begin(&w); bson_w_bool(&w, "_id", false); bson_w_bool(&w, "user", true); bson_w_end(&w);
    int rc = find_one("security", &f, &p, &doc);
    if (rc == CB_OK) {
        bson_doc d; bson_el el; const char *s; size_t n;
        rc = CB_E_NOT_FOUND;
        if (bson_doc_init(&d, doc.p, doc.len) && bson_find(&d, "user", &el) && bson_el_str(&el, &s, &n) && n > 0 && n < cap) {
            memcpy(user, s, n); user[n] = 0;
            rc = CB_OK;
        }
    }
    cb_buf_free(&f); cb_buf_free(&p); cb_buf_free(&doc);
    return rc;
}

// user4: {_id: user, enabled: true} exists
static int user_enabled(const char *user) {
    cb_buf f, p, doc;
    cb_buf_init(&f); cb_buf_init(&p); cb_buf_init(&doc);
    bson_w w;
    bson_w_init(&w, &f); bson_w_begin(&w); bson_w_str(&w, "_id", user); bson_w_bool(&w, "enabled", true); bson_w_end(&w);
    bson_w_init(&w, &p); bson_w_begin(&w); bson_w_bool(&w, "_id", true); bson_w_end(&w);
    int rc = find_one("user4", &f, &p, &doc);
    cb_buf_free(&f); cb_buf_free(&p); cb_buf_free(&doc);
    return rc;
}

// user4: {_id: user, marks: "troll"} exists, cached 15 minutes
static bool user_troll(const char *user) {
    int64_t now = cb_now_ms();
    pthread_mutex_lock(&cache_mu);
    caches_ensure();
    troll_rec *t = smap_get(&troll_cache, user);
    if (t && now - t->at_ms < TROLL_TTL_MS) { bool v = t->troll; pthread_mutex_unlock(&cache_mu); return v; }
    pthread_mutex_unlock(&cache_mu);
    cb_buf f, p, doc;
    cb_buf_init(&f); cb_buf_init(&p); cb_buf_init(&doc);
    bson_w w;
    bson_w_init(&w, &f); bson_w_begin(&w); bson_w_str(&w, "_id", user); bson_w_str(&w, "marks", "troll"); bson_w_end(&w);
    bson_w_init(&w, &p); bson_w_begin(&w); bson_w_bool(&w, "_id", true); bson_w_end(&w);
    int rc = find_one("user4", &f, &p, &doc);
    cb_buf_free(&f); cb_buf_free(&p); cb_buf_free(&doc);
    bool v = rc == CB_OK;
    if (rc == CB_OK || rc == CB_E_NOT_FOUND) {
        pthread_mutex_lock(&cache_mu);
        t = smap_get(&troll_cache, user);
        if (!t && (t = calloc(1, sizeof *t)) && !smap_put(&troll_cache, user, t)) { free(t); t = NULL; }
        if (t) { t->troll = v; t->at_ms = now; }
        pthread_mutex_unlock(&cache_mu);
    }
    return v;
}

// user4: {username, title} -> "Title Name", cached
static void user_name(const char *user, char *out, size_t cap) {
    int64_t now = cb_now_ms();
    out[0] = 0;
    pthread_mutex_lock(&cache_mu);
    caches_ensure();
    name_rec *r = smap_get(&name_cache, user);
    if (r && now - r->at_ms < NAME_TTL_MS) { cb_strlcpy(out, r->name, cap); pthread_mutex_unlock(&cache_mu); return; }
    pthread_mutex_unlock(&cache_mu);
    cb_buf f, p, doc;
    cb_buf_init(&f); cb_buf_init(&p); cb_buf_init(&doc);
    bson_w w;
    bson_w_init(&w, &f); bson_w_begin(&w); bson_w_str(&w, "_id", user); bson_w_end(&w);
    bson_w_init(&w, &p); bson_w_begin(&w); bson_w_bool(&w, "_id", false); bson_w_bool(&w, "username", true); bson_w_bool(&w, "title", true); bson_w_end(&w);
    int rc = find_one("user4", &f, &p, &doc);
    if (rc == CB_OK) {
        bson_doc d; bson_el el; const char *s; size_t n;
        char name[LWS_USER_MAX] = "", title[16] = "";
        if (bson_doc_init(&d, doc.p, doc.len)) {
            if (bson_find(&d, "username", &el) && bson_el_str(&el, &s, &n) && n < sizeof name) { memcpy(name, s, n); name[n] = 0; }
            if (bson_find(&d, "title", &el) && bson_el_str(&el, &s, &n) && n < sizeof title) { memcpy(title, s, n); title[n] = 0; }
        }
        if (title[0]) snprintf(out, cap, "%s %s", title, name); else cb_strlcpy(out, name, cap);
        pthread_mutex_lock(&cache_mu);
        r = smap_get(&name_cache, user);
        if (!r && (r = calloc(1, sizeof *r)) && !smap_put(&name_cache, user, r)) { free(r); r = NULL; }
        if (r) { cb_strlcpy(r->name, out, sizeof r->name); r->at_ms = now; }
        pthread_mutex_unlock(&cache_mu);
    }
    cb_buf_free(&f); cb_buf_free(&p); cb_buf_free(&doc);
}

// game5: {is, us, tid, sid, iid}, cached 10 minutes (a missing game too). CB_OK, CB_E_NOT_FOUND, CB_E_IO.
static int game_lookup(const char *game_id, game_rec *out) {
    int64_t now = cb_now_ms();
    pthread_mutex_lock(&cache_mu);
    caches_ensure();
    game_rec *g = smap_get(&game_cache, game_id);
    if (g && now - g->at_ms < GAME_TTL_MS) { *out = *g; pthread_mutex_unlock(&cache_mu); return out->missing ? CB_E_NOT_FOUND : CB_OK; }
    pthread_mutex_unlock(&cache_mu);
    cb_buf f, p, doc;
    cb_buf_init(&f); cb_buf_init(&p); cb_buf_init(&doc);
    bson_w w;
    bson_w_init(&w, &f); bson_w_begin(&w); bson_w_str(&w, "_id", game_id); bson_w_end(&w);
    bson_w_init(&w, &p); bson_w_begin(&w);
    bson_w_bool(&w, "is", true); bson_w_bool(&w, "us", true); bson_w_bool(&w, "tid", true); bson_w_bool(&w, "sid", true); bson_w_bool(&w, "iid", true);
    bson_w_end(&w);
    int rc = find_one("game5", &f, &p, &doc);
    game_rec rec;
    memset(&rec, 0, sizeof rec);
    rec.at_ms = now;
    if (rc == CB_OK) {
        bson_doc d; bson_el el; const char *s; size_t n;
        rc = CB_E_NOT_FOUND;
        if (bson_doc_init(&d, doc.p, doc.len) && bson_find(&d, "is", &el) && bson_el_str(&el, &s, &n) && n == 8) {
            memcpy(rec.player_id[0], s, 4); memcpy(rec.player_id[1], s + 4, 4);
            bson_doc us;
            if (bson_find(&d, "us", &el) && bson_el_doc(&el, &us)) {
                size_t it = 0; int i = 0;
                bson_el ue;
                while (i < 2 && bson_next(&us, &it, &ue)) {
                    if (bson_el_str(&ue, &s, &n) && n < LWS_USER_MAX) { memcpy(rec.user[i], s, n); rec.user[i][n] = 0; }
                    i++;
                }
            }
            if (bson_find(&d, "tid", &el) && bson_el_str(&el, &s, &n) && n < sizeof rec.ext_id) { rec.ext_kind = 't'; memcpy(rec.ext_id, s, n); rec.ext_id[n] = 0; }
            else if (bson_find(&d, "iid", &el) && bson_el_str(&el, &s, &n) && n < sizeof rec.ext_id) { rec.ext_kind = 'w'; memcpy(rec.ext_id, s, n); rec.ext_id[n] = 0; }
            else if (bson_find(&d, "sid", &el) && bson_el_str(&el, &s, &n) && n < sizeof rec.ext_id) { rec.ext_kind = 'm'; memcpy(rec.ext_id, s, n); rec.ext_id[n] = 0; }
            rc = CB_OK;
        }
    }
    cb_buf_free(&f); cb_buf_free(&p); cb_buf_free(&doc);
    if (rc == CB_E_NOT_FOUND) rec.missing = true;
    if (rc == CB_OK || rc == CB_E_NOT_FOUND) {
        pthread_mutex_lock(&cache_mu);
        if (smap_size(&game_cache) < GAME_CACHE_MAX) {
            g = smap_get(&game_cache, game_id);
            if (!g && (g = calloc(1, sizeof *g)) && !smap_put(&game_cache, game_id, g)) { free(g); g = NULL; }
            if (g) *g = rec;
        }
        pthread_mutex_unlock(&cache_mu);
        *out = rec;
    }
    return rc;
}

// ---- the request ----

// The session id from the lila2 cookie ("sessionId=<\w+>" anywhere in its value) or ?sessionId=.
static bool session_id(const cb_http_req *req, char *out, size_t cap) {
    const char *cookie = cb_http_header_get(req, "cookie");
    if (cookie) {
        const char *c = cookie;
        size_t cl = strlen(CFG.cookie);
        while ((c = strstr(c, CFG.cookie)) != NULL) {
            bool start = c == cookie || c[-1] == ' ' || c[-1] == ';';
            c += cl;
            if (!start || *c != '=') continue;
            c++;
            const char *end = strchr(c, ';');
            size_t vlen = end ? (size_t)(end - c) : strlen(c);
            const char *s = c;
            while (s + 10 <= c + vlen) {
                if (memcmp(s, "sessionId=", 10) == 0) {
                    s += 10;
                    size_t i = 0;
                    while (s + i < c + vlen && (isalnum((unsigned char)s[i]) || s[i] == '_') && i + 1 < cap) { out[i] = s[i]; i++; }
                    out[i] = 0;
                    if (i) return true;
                    break;
                }
                s++;
            }
            break;
        }
    }
    return cb_http_query_get(req, "sessionId", out, cap) && out[0];
}

// Auth.apply: the user behind the request, "" when anonymous. CB_E_IO when MongoDB is away.
static int auth_user(const cb_http_req *req, const char *flag, char *user, size_t cap) {
    user[0] = 0;
    if (strcmp(flag, "api") == 0 || strcmp(flag, "embed") == 0) return CB_OK;
    char sid[128];
    if (!session_id(req, sid, sizeof sid)) return CB_OK;   // bearer tokens (the mobile app) are not served
    if (cb_starts_with(sid, "appeal:") || cb_starts_with(sid, "TOK-")) return CB_OK;
    int rc = session_user(sid, user, cap);
    if (rc == CB_E_NOT_FOUND) { user[0] = 0; return CB_OK; }
    if (rc != CB_OK) return rc;
    rc = user_enabled(user);
    if (rc == CB_E_NOT_FOUND) { user[0] = 0; return CB_OK; }
    return rc;
}

static bool csrf_ok(const cb_http_req *req) {
    const char *origin = cb_http_header_get(req, "origin");
    if (!origin || !*origin) return true;
    static const char *const APPS[] = { "ionic://localhost", "capacitor://localhost", "http://localhost", "http://localhost:8080" };
    if (strcmp(origin, CFG.csrf_origin) == 0) return true;
    for (size_t i = 0; i < 4; i++) if (strcmp(origin, APPS[i]) == 0) return true;
    CB_INFO(MOD, "CSRF origin: \"%s\" %s", origin, req->path);
    return false;
}

static bool sri_of(const cb_http_req *req, char *out, size_t cap) {
    if (!cb_http_query_get(req, "sri", out, cap) || !out[0]) return false;
    return strchr(out, ' ') == NULL;
}

static void flag_of(const cb_http_req *req, char *out, size_t cap) {
    out[0] = 0;
    char f[16];
    if (!cb_http_query_get(req, "flag", f, sizeof f)) return;
    if (strcmp(f, "simul") == 0 || strcmp(f, "tournament") == 0 || strcmp(f, "api") == 0 || strcmp(f, "embed") == 0) cb_strlcpy(out, f, cap);
}

// The common front: CSRF, the sri, auth. Fills c (not yet registered) or answers the refusal.
static lws_conn *front(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res, lws_kind kind, int credits, int interval_s, bool api) {
    if (!csrf_ok(req)) { cb_router_error(res, 403, "forbidden"); return NULL; }
    char sri[LWS_SRI_MAX], flag[12], user[LWS_USER_MAX];
    if (api) {
        uint8_t rnd[9];   // Sri.random(): 12 characters of our own
        cb_random_bytes(rnd, sizeof rnd);
        static const char AL[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
        for (int i = 0; i < 12; i++) sri[i] = AL[(rnd[i % 9] + i * 7) & 63];
        sri[12] = 0;
        cb_strlcpy(flag, "api", sizeof flag);
        user[0] = 0;
    } else {
        if (!sri_of(req, sri, sizeof sri)) { cb_router_error(res, 400, "bad request"); return NULL; }
        flag_of(req, flag, sizeof flag);
        int rc = auth_user(req, flag, user, sizeof user);
        if (rc != CB_OK) { cb_router_error(res, 503, "database unavailable"); return NULL; }
    }
    lws_conn *c = lws_conn_new(kind, sri, user, req->peer, flag, credits, interval_s);
    if (!c) { cb_router_error(res, 503, "out of memory"); return NULL; }
    return c;
}

static void upgrade(const cb_http_req *req, cb_http_res *res, lws_conn *c) {
    if (cb_ws_upgrade(req, res, &lws_ws_handlers, c) != CB_OK) lws_conn_free(c);
}

static void h_site(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    lws_conn *c = front(ctx, req, res, K_SITE, 50, 20, false);
    if (c) upgrade(req, res, c);
}

static void h_api(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    lws_conn *c = front(ctx, req, res, K_API, 50, 30, true);
    if (c) upgrade(req, res, c);
}

static void h_lobby(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    lws_conn *c = front(ctx, req, res, K_LOBBY, 30, 30, false);
    if (c) upgrade(req, res, c);
}

static bool id_ok(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) if (!isalnum((unsigned char)s[i])) return false;
    return true;
}

// /play/<gameId+playerId>/v<n>: a player's socket
static void h_play(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    const char *p = req->path + 6;   // after "/play/"
    const char *slash = strchr(p, '/');
    size_t n = slash ? (size_t)(slash - p) : strlen(p);
    if (n != 12 || !id_ok(p, 12)) { cb_router_error(res, 404, "not found"); return; }
    char game_id[9], player_id[5];
    memcpy(game_id, p, 8); game_id[8] = 0;
    memcpy(player_id, p + 8, 4); player_id[4] = 0;
    lws_conn *c = front(ctx, req, res, K_ROUND, 100, 20, false);
    if (!c) return;
    game_rec g;
    int rc = game_lookup(game_id, &g);
    if (rc == CB_E_IO) { lws_conn_free(c); cb_router_error(res, 503, "database unavailable"); return; }
    int color = -1;
    for (int i = 0; i < 2 && rc == CB_OK; i++)
        if (strcmp(g.player_id[i], player_id) == 0 && strcmp(g.user[i], c->user) == 0) color = i;
    if (color < 0) { lws_conn_free(c); cb_router_error(res, 404, "not found"); return; }
    c->is_player = true;
    c->color = color;
    cb_strlcpy(c->player_id, player_id, sizeof c->player_id);
    c->ext_kind = g.ext_kind;
    cb_strlcpy(c->ext_id, g.ext_id, sizeof c->ext_id);
    if (c->user[0]) c->troll = user_troll(c->user);
    char v[16];
    if (cb_http_query_get(req, "v", v, sizeof v) && v[0]) { c->has_from = true; c->from_version = atoi(v); }
    cb_strlcpy(c->room_id, game_id, sizeof c->room_id);   // the room itself is the loop's (on_open)
    upgrade(req, res, c);
}

// /watch/<gameId>/<color>/v<n>: a spectator's socket
static void h_watch(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    const char *p = req->path + 7;   // after "/watch/"
    const char *slash = strchr(p, '/');
    size_t n = slash ? (size_t)(slash - p) : strlen(p);
    if (n != 8 || !id_ok(p, 8)) { cb_router_error(res, 404, "not found"); return; }
    char game_id[9];
    memcpy(game_id, p, 8); game_id[8] = 0;
    lws_conn *c = front(ctx, req, res, K_ROUND, 50, 20, false);
    if (!c) return;
    game_rec g;
    int rc = game_lookup(game_id, &g);
    if (rc == CB_E_IO) { lws_conn_free(c); cb_router_error(res, 503, "database unavailable"); return; }
    if (rc != CB_OK) { lws_conn_free(c); cb_router_error(res, 404, "not found"); return; }
    if (c->user[0]) {
        c->troll = user_troll(c->user);
        user_name(c->user, c->name, sizeof c->name);
    }
    char v[16];
    if (cb_http_query_get(req, "v", v, sizeof v) && v[0]) { c->has_from = true; c->from_version = atoi(v); }
    cb_strlcpy(c->room_id, game_id, sizeof c->room_id);
    upgrade(req, res, c);
}

static const cb_route ROUTES[] = {
    { "GET", "/socket", CB_ROUTE_POOL, h_site, 0 },
    { "GET", "/socket/*", CB_ROUTE_POOL, h_site, 0 },
    { "GET", "/analysis/socket", CB_ROUTE_POOL, h_site, 0 },
    { "GET", "/analysis/socket/*", CB_ROUTE_POOL, h_site, 0 },
    { "GET", "/api/socket", CB_ROUTE_POOL, h_api, 0 },
    { "GET", "/lobby/socket", CB_ROUTE_POOL, h_lobby, 0 },
    { "GET", "/lobby/socket/*", CB_ROUTE_POOL, h_lobby, 0 },
    { "GET", "/play/*", CB_ROUTE_POOL, h_play, 0 },
    { "GET", "/watch/*", CB_ROUTE_POOL, h_watch, 0 },
};
const cb_route_list lws_route_list = { ROUTES, (int)(sizeof ROUTES / sizeof ROUTES[0]) };
