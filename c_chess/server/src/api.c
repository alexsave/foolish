/* api.c - c_chess's routes (wire.h has the table). The game routes run on the loop thread when it
 * can take them (CB_ROUTE_INLINE|HOME): they touch only memory, under cc_lock, and hand their pushes
 * and journal records off. The reads that may need the store answer to the pool when they find
 * themselves on the loop (cb_http_res.to_pool). Sign-up and sign-in are HTTP on the pool (bcrypt). */
#include "api.h"
#include "cc_server.h"
#include "journal.h"
#include "pgn.h"
#include "pushes.h"
#include "ai.h"
#include "engine.h"
#include "arenas.h"

#include "auth/auth.h"
#include "net/loop.h"
#include "util/err.h"
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OCTETS "application/octet-stream"

void cc_api_answer(cb_http_res *res, int status, const void *p, size_t n) {
    cb_res_send(res, status, OCTETS, p, n);
    cb_res_header(res, "Cache-Control", "no-store");
}

void cc_api_refuse(cb_http_res *res, int status, int code, int rc) {
    cw_err e = { (uint8_t)code, (int8_t)rc, { 0, 0 } };
    cc_api_answer(res, status, &e, sizeof e);
}
#define answer cc_api_answer
#define refuse cc_api_refuse

static bool on_loop(cb_ctx *ctx) { return ctx->loop && cb_loop_on_thread(ctx->loop); }

/* The verified caller and its player (made on first sight), or false with 401 answered. */
bool cc_api_caller(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res, cb_user *u) {
    if (!ctx->auth) { refuse(res, 503, CW_E_BUSY, 0); return false; }
    memset(u, 0, sizeof *u);
    int rc = cb_auth_verify_bearer(ctx->auth, req, u);
    if (rc == CB_E_AUTH) { refuse(res, 401, CW_E_EXPIRED, 0); return false; }
    if (rc != CB_OK) { refuse(res, 503, CW_E_BUSY, 0); return false; }
    return true;
}

#define me cc_api_caller
static cc_player *player(const cb_user *u) { return cc_player_of(u->id, u->username); }

/* The number after the route's prefix: "/c/game/123" -> 123; 0 for none. */
uint32_t cc_api_tail_id(const char *path, const char *prefix) {
    size_t n = strlen(prefix);
    if (strncmp(path, prefix, n) != 0) return 0;
    uint64_t v = 0;
    const char *s = path + n;
    if (!*s) return 0;
    for (; *s && *s != '?'; s++) {
        if (*s < '0' || *s > '9') return 0;
        v = v * 10 + (uint64_t)(*s - '0');
        if (v > UINT32_MAX) return 0;
    }
    return (uint32_t)v;
}

/* ---- sign-up, sign-in, refresh ---- */

static bool valid_name(const char *s, size_t n) {
    if (n < 3 || n > CW_NAME) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

static void send_session(cb_http_res *res, const cb_session *s) {
    size_t an = strlen(s->access_token);
    uint8_t *buf = malloc(sizeof(cw_session) + an);
    if (!buf) { refuse(res, 503, CW_E_BUSY, 0); return; }
    cw_session out;
    memset(&out, 0, sizeof out);
    out.expires_in = (uint32_t)s->expires_in;
    out.access_len = (uint16_t)an;
    memcpy(out.user_id, s->user.id, CW_UUID);
    memcpy(out.name, s->user.username, strnlen(s->user.username, CW_NAME));
    memcpy(out.refresh, s->refresh_token, strnlen(s->refresh_token, CW_REFRESH));
    memcpy(buf, &out, sizeof out);
    memcpy(buf + sizeof out, s->access_token, an);
    answer(res, 200, buf, sizeof out + an);
    free(buf);
}

static void auth_refused(cb_http_res *res, int rc) {
    switch (rc) {
    case CB_AUTH_E_EXISTS: refuse(res, 409, CW_E_NAME_TAKEN, 0); return;
    case CB_AUTH_E_BAD_LOGIN: refuse(res, 401, CW_E_BAD_LOGIN, 0); return;
    case CB_AUTH_E_WEAK_PASSWORD: case CB_AUTH_E_LONG_PASSWORD: refuse(res, 400, CW_E_WEAK_PASSWORD, 0); return;
    case CB_AUTH_E_BAD_USERNAME: case CB_AUTH_E_BAD_EMAIL: refuse(res, 400, CW_E_BAD_NAME, 0); return;
    case CB_AUTH_E_TOO_MANY: refuse(res, 429, CW_E_TOO_MANY, 0); return;
    case CB_AUTH_E_BAD_REFRESH: case CB_AUTH_E_REFRESH_USED: refuse(res, 401, CW_E_EXPIRED, 0); return;
    default: CB_ERROR("cc", "auth: %s", cb_strerror(rc)); refuse(res, 503, CW_E_BUSY, 0); return;
    }
}

static void h_auth(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cw_auth_req q;
    if (req->body_len != sizeof q || !ctx->auth) { refuse(res, 400, CW_E_BAD, 0); return; }
    memcpy(&q, req->body, sizeof q);
    char name[CW_NAME + 1], pw[CW_PASSWORD + 1];
    size_t nn = strnlen(q.name, CW_NAME), pn = strnlen(q.password, CW_PASSWORD);
    memcpy(name, q.name, nn); name[nn] = 0;
    memcpy(pw, q.password, pn); pw[pn] = 0;
    if (!valid_name(name, nn)) { refuse(res, 400, CW_E_BAD_NAME, 0); return; }
    /* the account's email is the name, lowercased, at a domain nobody mails */
    char email[CW_NAME + 32];
    size_t i = 0;
    for (; i < nn; i++) email[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    snprintf(email + i, sizeof email - i, "@c-chess.invalid");
    cb_session *s = calloc(1, sizeof *s);
    if (!s) { refuse(res, 503, CW_E_BUSY, 0); return; }
    int rc = q.mode == CW_AUTH_SIGNUP ? cb_auth_signup(ctx->auth, email, pw, name, s)
           : q.mode == CW_AUTH_SIGNIN ? cb_auth_signin(ctx->auth, email, pw, s) : CB_E_INVALID;
    if (rc == CB_E_INVALID) refuse(res, 400, CW_E_BAD, 0);
    else if (rc != CB_OK) auth_refused(res, rc);
    else {
        pthread_mutex_lock(&cc_lock);
        player(&s->user);
        pthread_mutex_unlock(&cc_lock);
        send_session(res, s);
    }
    memset(pw, 0, sizeof pw);
    free(s);
}

static void h_refresh(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cw_refresh_req q;
    if (req->body_len != sizeof q || !ctx->auth) { refuse(res, 400, CW_E_BAD, 0); return; }
    memcpy(&q, req->body, sizeof q);
    char tok[CW_REFRESH + 1];
    size_t n = strnlen(q.refresh, CW_REFRESH);
    memcpy(tok, q.refresh, n); tok[n] = 0;
    cb_session *s = calloc(1, sizeof *s);
    if (!s) { refuse(res, 503, CW_E_BUSY, 0); return; }
    int rc = cb_auth_refresh(ctx->auth, tok, s);
    if (rc != CB_OK) auth_refused(res, rc);
    else send_session(res, s);
    free(s);
}

/* ---- play ---- */

static void h_seek(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_user u;
    if (!me(ctx, req, res, &u)) return;
    cw_seek tc;
    if (req->body_len != sizeof tc) { refuse(res, 400, CW_E_BAD, 0); return; }
    memcpy(&tc, req->body, sizeof tc);
    if (!cc_tc_ok(&tc)) { refuse(res, 400, CW_E_BAD, 0); return; }
    pthread_mutex_lock(&cc_lock);
    cc_player *p = player(&u);
    cc_live *g = NULL;
    int code = p ? cc_seek_add(p, &tc, cc_mono(), &g) : CW_E_BUSY;
    cw_seek_ans a = { g ? g->id : 0, (uint8_t)(g ? cc_color_of(g, p) : 0), { 0, 0, 0 } };
    pthread_mutex_unlock(&cc_lock);
    if (code != CW_E_NONE) refuse(res, 503, code, 0);
    else answer(res, 200, &a, sizeof a);
}

static void h_unseek(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_user u;
    if (!me(ctx, req, res, &u)) return;
    pthread_mutex_lock(&cc_lock);
    cc_player *p = cc_player_get(u.id);
    if (p) cc_seek_cancel(p, cc_mono());
    pthread_mutex_unlock(&cc_lock);
    answer(res, 200, NULL, 0);
}

static void h_challenge(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_user u;
    if (!me(ctx, req, res, &u)) return;
    cw_seek tc;
    if (req->body_len != sizeof tc) { refuse(res, 400, CW_E_BAD, 0); return; }
    memcpy(&tc, req->body, sizeof tc);
    if (!cc_tc_ok(&tc)) { refuse(res, 400, CW_E_BAD, 0); return; }
    pthread_mutex_lock(&cc_lock);
    cc_player *p = player(&u);
    cc_challenge *c = p ? cc_challenge_add(p, &tc, cc_mono()) : NULL;
    cw_id a = { c ? c->id : 0 };
    pthread_mutex_unlock(&cc_lock);
    if (!a.id) refuse(res, 503, CW_E_BUSY, 0);
    else answer(res, 200, &a, sizeof a);
}

static void h_accept(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_user u;
    if (!me(ctx, req, res, &u)) return;
    cw_id q;
    if (req->body_len != sizeof q) { refuse(res, 400, CW_E_BAD, 0); return; }
    memcpy(&q, req->body, sizeof q);
    pthread_mutex_lock(&cc_lock);
    int64_t now = cc_mono();
    cc_player *p = player(&u);
    cc_challenge *c = cc_challenge_get(q.id, now);
    int status = 200, code = CW_E_NONE;
    cw_id a = { 0 };
    if (!p) { status = 503; code = CW_E_BUSY; }
    else if (!c) { status = 404; code = CW_E_NO_GAME; }
    else if (c->who == p) { status = 409; code = CW_E_SELF; }
    else {
        bool creator_white = (q.id & 1) == 0;   /* a coin the link already holds */
        cc_live *g = cc_live_start(creator_white ? c->who : p, creator_white ? p : c->who, &c->tc, now);
        if (!g) { status = 503; code = CW_E_BUSY; }
        else { a.id = g->id; cc_challenge_drop(q.id); }
    }
    pthread_mutex_unlock(&cc_lock);
    if (status != 200) refuse(res, status, code, 0);
    else answer(res, 200, &a, sizeof a);
}

static void h_ai(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_user u;
    if (!me(ctx, req, res, &u)) return;
    cw_ai_req q;
    if (req->body_len != sizeof q) { refuse(res, 400, CW_E_BAD, 0); return; }
    memcpy(&q, req->body, sizeof q);
    cw_seek tc = { q.base_s, q.inc_s, 0 };
    if (!cc_tc_ok(&tc) || q.level < 1 || q.level > CC_AI_LEVELS || q.color > 2) { refuse(res, 400, CW_E_BAD, 0); return; }
    if (!cc_engine_available()) { refuse(res, 503, CW_E_BUSY, 0); return; }
    pthread_mutex_lock(&cc_lock);
    int64_t now = cc_mono();
    cc_player *p = player(&u), *ai = cc_ai_player(q.level);
    int color = q.color == 2 ? (int)(now & 1) : q.color;
    cc_live *g = p && ai ? cc_live_start(color == 0 ? p : ai, color == 0 ? ai : p, &tc, now) : NULL;
    cw_id a = { g ? g->id : 0 };
    pthread_mutex_unlock(&cc_lock);
    if (!a.id) refuse(res, 503, CW_E_BUSY, 0);
    else answer(res, 200, &a, sizeof a);
}

static void h_move(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_user u;
    if (!me(ctx, req, res, &u)) return;
    cw_move_req q;
    if (req->body_len != sizeof q) { refuse(res, 400, CW_E_BAD, 0); return; }
    memcpy(&q, req->body, sizeof q);
    pthread_mutex_lock(&cc_lock);
    int64_t now = cc_mono();
    cc_live *l = cc_live_get(q.game);
    int color = -1, rc = CC_OK, status = 200, code = CW_E_NONE;
    if (!l) { status = 404; code = CW_E_NO_GAME; }
    else if ((color = cc_color_of(l, cc_player_get(u.id))) < 0) { status = 403; code = CW_E_NOT_PLAYER; }
    else {
        bool was_over = cc_game_over(&l->g);
        int32_t think = q.think_ms > 3600000u ? -1 : (int32_t)q.think_ms;   /* past an hour: unknown */
        rc = cc_game_move_lag(&l->g, color, q.ply, (ch_move)q.move, now, think);
        if (rc == CC_OK) { cc_journal_move(l); cc_live_changed(l, now); }
        else {
            if (!was_over && cc_game_over(&l->g)) cc_live_changed(l, now);   /* the move came after the flag */
            status = 409; code = CW_E_RULES;
        }
    }
    pthread_mutex_unlock(&cc_lock);
    if (status != 200) refuse(res, status, code, rc);
    else answer(res, 200, NULL, 0);
}

static void h_act(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_user u;
    if (!me(ctx, req, res, &u)) return;
    cw_act_req q;
    if (req->body_len != sizeof q) { refuse(res, 400, CW_E_BAD, 0); return; }
    memcpy(&q, req->body, sizeof q);
    pthread_mutex_lock(&cc_lock);
    int64_t now = cc_mono();
    cc_live *l = cc_live_get(q.game);
    int color = -1, rc = CC_OK, status = 200, code = CW_E_NONE;
    if (!l) { status = 404; code = CW_E_NO_GAME; }
    else if ((color = cc_color_of(l, cc_player_get(u.id))) < 0) { status = 403; code = CW_E_NOT_PLAYER; }
    else {
        bool was_over = cc_game_over(&l->g);
        switch (q.act) {
        case CW_A_RESIGN: rc = cc_game_resign(&l->g, color, now); break;
        case CW_A_ABORT: rc = l->arena ? CC_E_STATE : cc_game_abort(&l->g, color, now); break;   /* an arena game: play or resign */
        case CW_A_DRAW: rc = cc_game_draw(&l->g, color, now); break;
        case CW_A_DRAW_DECLINE: rc = cc_game_draw_decline(&l->g, color); break;
        case CW_A_REMATCH:
            if (!cc_game_over(&l->g) || l->next_game || l->arena) rc = CC_E_STATE;   /* the arena pairs its own */
            else if ((l->rematch_offer & (1u << (color ^ 1))) || cc_ai_level(l->p[color ^ 1])) {   /* the computer agrees at once */
                cw_seek tc = { (uint16_t)(l->g.base_ms / 1000), (uint8_t)(l->g.inc_ms / 1000), l->rated, };
                cc_live *n = cc_live_start(l->p[1], l->p[0], &tc, now);   /* colors swap */
                if (!n) rc = CC_E_STATE;
                else { l->next_game = n->id; l->rematch_offer = 0; }
            } else l->rematch_offer |= (uint8_t)(1u << color);
            break;
        case CW_A_REMATCH_DECLINE:
            if (!cc_game_over(&l->g)) rc = CC_E_STATE;
            else l->rematch_offer = 0;
            break;
        default: status = 400; code = CW_E_BAD; break;
        }
        if (status == 200 && (rc == CC_OK || (!was_over && cc_game_over(&l->g)))) cc_live_changed(l, now);
        if (status == 200 && rc != CC_OK) { status = 409; code = CW_E_RULES; }
    }
    pthread_mutex_unlock(&cc_lock);
    if (status != 200) refuse(res, status, code, rc);
    else answer(res, 200, NULL, 0);
}

/* ---- reads ---- */

/* A live game's snapshot into out: its length. */
static size_t snap_of(const cc_live *l, uint8_t *out, int64_t now) {
    cw_snap s;
    memset(&s, 0, sizeof s);
    s.v = CW_V;
    s.kind = CW_SNAP;
    s.status = l->g.status;
    s.winner = l->g.winner;
    s.game = l->id;
    s.ply = l->g.ply;
    s.base_s = (uint16_t)(l->g.base_ms / 1000);
    s.inc_s = (uint8_t)(l->g.inc_ms / 1000);
    s.rated = l->rated;
    s.draw_offer = l->g.draw_offer;
    s.rematch_offer = l->rematch_offer;
    for (int c = 0; c < 2; c++) {
        s.left_ms[c] = (uint32_t)l->g.left_ms[c];
        s.rating[c] = cc_game_over(&l->g) ? (int16_t)cc_rating_int(&l->p[c]->r[l->speed]) : l->rating0[c];
        s.diff[c] = l->diff[c];
        memcpy(s.name[c], l->p[c]->name, strnlen(l->p[c]->name, CW_NAME));
    }
    s.turn_ms = cc_game_clock_running(&l->g) ? (uint32_t)(now - l->g.turn_at) : 0;
    s.next_game = l->next_game;
    s.clock_running = cc_game_clock_running(&l->g);
    s.started_at = l->started_at;
    s.arena = l->arena;
    memcpy(out, &s, sizeof s);
    memcpy(out + sizeof s, l->g.moves, (size_t)l->g.ply * 2);
    return sizeof s + (size_t)l->g.ply * 2;
}

#define SNAP_MAX (sizeof(cw_snap) + CC_MAX_PLY * 2)

static void h_game(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    uint32_t id = cc_api_tail_id(req->path, "/c/game/");
    if (!id) { refuse(res, 400, CW_E_BAD, 0); return; }
    uint8_t buf[SNAP_MAX];
    size_t n = 0;
    pthread_mutex_lock(&cc_lock);
    cc_live *l = cc_live_get(id);
    if (l) n = snap_of(l, buf, cc_mono());
    pthread_mutex_unlock(&cc_lock);
    if (!n) {
        if (on_loop(ctx)) { res->to_pool = true; return; }
        int rc = ctx->store ? cc_db_game(ctx->store, id, buf, sizeof buf) : 0;
        if (rc < 0) { refuse(res, 503, CW_E_BUSY, 0); return; }
        if (rc == 0) { refuse(res, 404, CW_E_NO_GAME, 0); return; }
        n = (size_t)rc;
    }
    answer(res, 200, buf, n);
}

static void h_lobby(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    static cw_lobby lb;   /* under cc_lock */
    pthread_mutex_lock(&cc_lock);
    int n = cc_lobby_fill(&lb, cc_mono());
    uint8_t buf[sizeof lb];
    memcpy(buf, &lb, (size_t)n);
    pthread_mutex_unlock(&cc_lock);
    answer(res, 200, buf, (size_t)n);
}

static void h_tv(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    static cw_tv tv;   /* under cc_lock */
    uint8_t buf[sizeof tv];
    pthread_mutex_lock(&cc_lock);
    int n = cc_tv_fill(&tv, CW_TV_MAX, cc_mono());
    memcpy(buf, &tv, (size_t)n);
    pthread_mutex_unlock(&cc_lock);
    answer(res, 200, buf, (size_t)n);
}

/* "/c/top/1" -> CC_BLITZ; -1 for anything but one speed's digit. */
static int top_speed(const char *path) {
    const char *s = path + strlen("/c/top/");
    if (strncmp(path, "/c/top/", 7) != 0 || s[0] < '0' || s[0] >= '0' + CC_SPEEDS || (s[1] && s[1] != '?')) return -1;
    return s[0] - '0';
}

static void h_top(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    static cw_top top;   /* under cc_lock */
    uint8_t buf[sizeof top];
    int speed = top_speed(req->path);
    if (speed < 0) { refuse(res, 404, CW_E_BAD, 0); return; }
    pthread_mutex_lock(&cc_lock);
    int n = cc_top_fill(&top, speed);
    memcpy(buf, &top, (size_t)n);
    pthread_mutex_unlock(&cc_lock);
    answer(res, 200, buf, (size_t)n);
}

static void tail_name(const char *path, const char *prefix, char *out) {
    size_t n = strlen(prefix), i = 0;
    out[0] = 0;
    if (strncmp(path, prefix, n) != 0) return;
    for (const char *s = path + n; *s && *s != '?' && *s != '/' && i < CW_NAME; s++) out[i++] = *s;
    out[i] = 0;
}

static void h_user(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (on_loop(ctx)) { res->to_pool = true; return; }
    char name[CW_NAME + 1];
    tail_name(req->path, "/c/user/", name);
    cw_profile pr;
    memset(&pr, 0, sizeof pr);
    cc_player snap;
    pthread_mutex_lock(&cc_lock);
    const cc_player *p = name[0] ? cc_player_by_name(name) : NULL;
    if (p) snap = *p;
    pthread_mutex_unlock(&cc_lock);
    if (!p) { refuse(res, 404, CW_E_NO_GAME, 0); return; }
    memcpy(pr.name, snap.name, strnlen(snap.name, CW_NAME));
    for (int k = 0; k < CC_SPEEDS; k++) {
        pr.rating[k] = (int16_t)cc_rating_int(&snap.r[k]);
        pr.rd[k] = (uint16_t)(snap.r[k].deviation + 0.5);
        pr.rated[k] = snap.rated[k];
    }
    pr.puzzle = (int16_t)cc_rating_int(&snap.pz);
    pr.puzzle_rd = (uint16_t)(snap.pz.deviation + 0.5);
    pr.puzzles_tried = snap.pz_tried;
    pr.puzzles_solved = snap.pz_solved;
    pr.games = snap.games;
    pr.wins = snap.wins;
    pr.losses = snap.losses;
    pr.draws = snap.draws;
    uint8_t *buf = malloc(sizeof pr + CW_POINTS_MAX * sizeof(cw_point));
    if (!buf) { refuse(res, 503, CW_E_BUSY, 0); return; }
    int np = ctx->store ? cc_db_points_of(ctx->store, &snap, (cw_point *)(void *)(buf + sizeof pr), CW_POINTS_SPEED) : 0;
    if (np < 0) np = 0;
    pr.n_points = (uint16_t)np;
    memcpy(buf, &pr, sizeof pr);
    answer(res, 200, buf, sizeof pr + (size_t)np * sizeof(cw_point));
    free(buf);
}

static void h_games(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (on_loop(ctx)) { res->to_pool = true; return; }
    char name[CW_NAME + 1];
    tail_name(req->path, "/c/games/", name);
    cc_player snap;
    pthread_mutex_lock(&cc_lock);
    const cc_player *p = name[0] ? cc_player_by_name(name) : NULL;
    if (p) snap = *p;
    pthread_mutex_unlock(&cc_lock);
    if (!p) { refuse(res, 404, CW_E_NO_GAME, 0); return; }
    cw_game_list *list = calloc(1, sizeof *list);
    if (!list) { refuse(res, 503, CW_E_BUSY, 0); return; }
    int rc = ctx->store ? cc_db_games_of(ctx->store, &snap, list) : CB_OK;
    if (rc != CB_OK) refuse(res, 503, CW_E_BUSY, 0);
    else answer(res, 200, list, 8 + (size_t)list->n * sizeof(cw_game_row));
    free(list);
}

static void h_pgn(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (on_loop(ctx)) { res->to_pool = true; return; }
    uint32_t id = cc_api_tail_id(req->path, "/c/pgn/");
    if (!id) { cb_res_text(res, 400, "no such game\n"); return; }
    uint8_t buf[SNAP_MAX];
    int n = 0;
    pthread_mutex_lock(&cc_lock);
    cc_live *l = cc_live_get(id);
    if (l) n = (int)snap_of(l, buf, cc_mono());
    pthread_mutex_unlock(&cc_lock);
    if (!n && ctx->store) n = cc_db_game(ctx->store, id, buf, sizeof buf);
    if (n <= 0) { cb_res_text(res, 404, "no such game\n"); return; }
    cw_snap s;
    memcpy(&s, buf, sizeof s);
    char wn[CW_NAME + 1] = { 0 }, bn[CW_NAME + 1] = { 0 }, date[16], site[64];
    memcpy(wn, s.name[0], CW_NAME);
    memcpy(bn, s.name[1], CW_NAME);
    time_t t = (time_t)(s.started_at / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(date, sizeof date, "%Y.%m.%d", &tm);
    snprintf(site, sizeof site, "/g/%u", id);
    ch_move moves[CC_MAX_PLY];
    memcpy(moves, buf + sizeof s, (size_t)s.ply * 2);
    /* a snapshot's ratings are after the game once it is over (diff then set): Elo is before it */
    cc_pgn_info in = { wn, bn, site, date, s.rated ? s.rating[0] - s.diff[0] : 0, s.rated ? s.rating[1] - s.diff[1] : 0,
                       s.base_s, s.inc_s, s.status, s.winner };
    char *out = malloc(16384);
    if (!out) { cb_res_text(res, 503, "busy\n"); return; }
    size_t len = cc_pgn(&in, moves, s.ply, out, 16384);
    if (len >= 16384) len = 16383;
    cb_res_send(res, 200, "application/x-chess-pgn; charset=utf-8", out, len);
    char disp[64];
    snprintf(disp, sizeof disp, "attachment; filename=\"c-chess-%u.pgn\"", id);
    cb_res_header(res, "Content-Disposition", disp);
    free(out);
}

static void h_stats(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    uint64_t q, c, k;
    cc_journal_counts(&q, &c, &k);
    pthread_mutex_lock(&cc_lock);
    uint32_t live = cc_games_live();
    pthread_mutex_unlock(&cc_lock);
    uint64_t es, en, aa, ar, ah, at, atu, atm, ap, apb;
    uint32_t eq, eb;
    cc_engine_counts(&es, &en, &eq, &eb);
    cc_ai_counts(&aa, &ar, &ah);
    pthread_mutex_lock(&cc_lock);
    cc_arena_counts(&at, &atu, &atm, &ap, &apb);
    pthread_mutex_unlock(&cc_lock);
    char out[768];
    snprintf(out, sizeof out, "games_live %u\njournal_queued %llu\njournal_committed %llu\njournal_commits %llu\n"
             "engine_searches %llu\nengine_nodes %llu\nengine_queued %u\nengine_busy %u\nai_asked %llu\nai_refused %llu\nai_cache_hits %llu\n"
             "arena_passes %llu\narena_pass_us %llu\narena_pass_max_us %llu\narena_pushes %llu\narena_push_bytes %llu\n", live,
             (unsigned long long)q, (unsigned long long)c, (unsigned long long)k, (unsigned long long)es, (unsigned long long)en, eq, eb,
             (unsigned long long)aa, (unsigned long long)ar, (unsigned long long)ah, (unsigned long long)at, (unsigned long long)atu,
             (unsigned long long)atm, (unsigned long long)ap, (unsigned long long)apb);
    cb_res_text(res, 200, out);
}

#define GAME_ROUTE (CB_ROUTE_POOL | CB_ROUTE_INLINE | CB_ROUTE_HOME)

static const cb_route ROUTES[] = {
    { "POST", "/c/auth", CB_ROUTE_POOL, h_auth, 0 },
    { "POST", "/c/refresh", CB_ROUTE_POOL, h_refresh, 0 },
    { "POST", "/c/seek", GAME_ROUTE, h_seek, 500 },
    { "POST", "/c/unseek", GAME_ROUTE, h_unseek, 500 },
    { "POST", "/c/challenge", GAME_ROUTE, h_challenge, 500 },
    { "POST", "/c/accept", GAME_ROUTE, h_accept, 500 },
    { "POST", "/c/move", GAME_ROUTE, h_move, 500 },
    { "POST", "/c/ai", GAME_ROUTE, h_ai, 500 },
    { "POST", "/c/act", GAME_ROUTE, h_act, 500 },
    { "GET", "/c/game/*", GAME_ROUTE, h_game, 500 },
    { "GET", "/c/lobby", GAME_ROUTE, h_lobby, 500 },
    { "GET", "/c/tv", GAME_ROUTE, h_tv, 500 },
    { "GET", "/c/top/*", GAME_ROUTE, h_top, 500 },
    { "GET", "/c/user/*", CB_ROUTE_POOL | CB_ROUTE_INLINE, h_user, 500 },
    { "GET", "/c/games/*", CB_ROUTE_POOL | CB_ROUTE_INLINE, h_games, 500 },
    { "GET", "/c/pgn/*", CB_ROUTE_POOL, h_pgn, 0 },
    { "GET", "/c/stats", CB_ROUTE_POOL, h_stats, 0 },
};

const cb_route_list cc_api_routes = { ROUTES, (int)(sizeof ROUTES / sizeof *ROUTES) };
