/* api_chat.c - the chat routes (wire.h has the table; chats.h the chat) and the console's mute. A
 * line is said on the loop when it can take it: it touches only memory under cc_lock and hands its
 * push off. The console's call runs where every console call does (the pool), behind its token. */
#include "api.h"
#include "chats.h"

#include "util/clock.h"

#include <stdio.h>
#include <string.h>

#define answer cc_api_answer

/* A refusal's HTTP status by its code. */
static int status_of(int code) {
    switch (code) {
    case CW_E_CHAT_ROOM: return 404;
    case CW_E_CHAT_TEXT: case CW_E_CHAT_WORDS: case CW_E_BAD: return 400;
    case CW_E_CHAT_GUEST: case CW_E_CHAT_MUTED: return 403;
    case CW_E_CHAT_FAST: return 429;
    default: return 503;
    }
}

static void refuse(cb_http_res *res, int code, int why, uint32_t wait_s) {
    cw_chat_err e = { (uint8_t)code, (int8_t)why, { 0, 0 }, wait_s };
    answer(res, status_of(code), &e, sizeof e);
}

static void h_say(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_user u;
    if (!cc_api_caller(ctx, req, res, &u)) return;
    cw_chat_req q;
    if (req->body_len != sizeof q) { refuse(res, CW_E_BAD, 0, 0); return; }
    memcpy(&q, req->body, sizeof q);
    uint32_t wait_s = 0;
    int why = 0;
    pthread_mutex_lock(&cc_lock);
    cc_player *p = cc_player_of(u.id, u.username);
    int code = p ? cc_chat_post(p, &q, cc_mono(), cc_wall(), &wait_s, &why) : CW_E_BUSY;
    pthread_mutex_unlock(&cc_lock);
    if (code != CW_E_NONE) refuse(res, code, why, wait_s);
    else answer(res, 200, NULL, 0);
}

/* GET /c/chat/<room>: anyone may read a room's lines (anyone may watch the room). */
static void h_log(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    char room[CW_ROOM];
    const char *s = req->path + strlen("/c/chat/");
    size_t n = 0;
    while (s[n] && s[n] != '?' && n < CW_ROOM) n++;
    if (n == CW_ROOM) { refuse(res, CW_E_CHAT_ROOM, 0, 0); return; }
    memcpy(room, s, n);
    room[n] = 0;
    uint8_t buf[CW_CHAT_LOG_MAX];
    pthread_mutex_lock(&cc_lock);
    size_t len = cc_chat_log(room, buf);
    pthread_mutex_unlock(&cc_lock);
    if (!len) refuse(res, CW_E_CHAT_ROOM, 0, 0);
    else answer(res, 200, buf, len);
}

/* POST /admin/api/mute, cw_mute_req: plain text, as a person reads it. */
static void h_mute(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cw_mute_req q;
    if (req->body_len != sizeof q) { cb_res_text(res, 400, "the body is a cw_mute_req (wire.h)\n"); return; }
    memcpy(&q, req->body, sizeof q);
    if (q.minutes > CC_CHAT_MUTE_MAX_MIN) { cb_res_text(res, 400, "a mute is at most a year\n"); return; }
    char name[CW_NAME + 1];
    size_t nn = strnlen(q.name, CW_NAME);
    memcpy(name, q.name, nn);
    name[nn] = 0;
    int64_t wall = cc_wall(), until = 0;
    pthread_mutex_lock(&cc_lock);
    cc_player *p = nn ? cc_player_by_name(name) : NULL;
    if (p) { cc_chat_mute(p, q.minutes, wall); until = p->mute_until; }
    pthread_mutex_unlock(&cc_lock);
    if (!p) { cb_res_text(res, 404, "no such player\n"); return; }
    char out[160], at[40];
    if (!q.minutes) snprintf(out, sizeof out, "unmuted %s\n", name);
    else {
        cb_iso8601(until, at, sizeof at);
        snprintf(out, sizeof out, "muted %s for %u minutes, until %s\n", name, q.minutes, at);
    }
    cb_res_text(res, 200, out);
}

#define GAME_ROUTE (CB_ROUTE_POOL | CB_ROUTE_INLINE | CB_ROUTE_HOME)

static const cb_route ROUTES[] = {
    { "POST", "/c/chat", GAME_ROUTE, h_say, 500 },
    { "GET", "/c/chat/*", GAME_ROUTE, h_log, 500 },
};

const cb_route_list cc_chat_routes = { ROUTES, (int)(sizeof ROUTES / sizeof *ROUTES) };

const cb_admin_call cc_admin_calls[CC_N_ADMIN_CALLS] = {
    { "POST", "mute", h_mute },
};
