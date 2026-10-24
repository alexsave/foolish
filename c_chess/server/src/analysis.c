/* analysis.c - see analysis.h. A request carries the moves that reach the position (replayed here:
 * an illegal list is refused); the search is the engine's at full strength for a node budget; its
 * answer, turned to white's view, is cached by the position's key and pushed to the asker. One
 * search in flight per player: a newer request replaces nothing, it is answered when its turn comes. */
#include "analysis.h"
#include "engine.h"
#include "pushes.h"

#include "auth/auth.h"
#include "util/err.h"

#include <stdlib.h>
#include <string.h>

#define EVAL_NODES 400000
#define EVAL_CAP_MS 3000
#define CACHE_BITS 18

static cw_eval *CACHE;   /* 2^18 answers, 12 MB, under cc_lock */
static uint64_t asked, cached, refused;

static cw_eval *slot(uint64_t key) {
    if (!CACHE && !(CACHE = calloc((size_t)1 << CACHE_BITS, sizeof *CACHE))) return NULL;
    return &CACHE[key >> (64 - CACHE_BITS)];
}

typedef struct { char user[CB_UUID_LEN + 1]; uint64_t key; uint16_t ply; uint8_t stm; } ask_ud;

static void answered(void *ud, const cc_engine_answer *a) {
    ask_ud *q = ud;
    cw_eval e;
    memset(&e, 0, sizeof e);
    e.v = CW_V;
    e.kind = CW_P_EVAL;
    e.ply = q->ply;
    e.key = q->key;
    e.depth = (uint8_t)(a->depth > 255 ? 255 : a->depth);
    int sign = q->stm == CH_WHITE ? 1 : -1;   /* the engine speaks for the side to move */
    int cp = a->score_cp > 3000 ? 3000 : a->score_cp < -3000 ? -3000 : a->score_cp;
    e.score = (int16_t)(sign * cp);
    e.mate = (int8_t)(sign * (a->mate > 120 ? 120 : a->mate < -120 ? -120 : a->mate));
    e.npv = (uint8_t)(a->npv > CW_PV_MAX ? CW_PV_MAX : a->npv);
    for (int i = 0; i < e.npv; i++) e.pv[i] = a->pv[i];
    if (!e.npv && a->best != CH_MOVE_NONE) { e.pv[0] = a->best; e.npv = 1; }
    pthread_mutex_lock(&cc_lock);
    cw_eval *c = slot(q->key);
    if (c && a->best != CH_MOVE_NONE) *c = e;
    pthread_mutex_unlock(&cc_lock);
    cc_push_user(q->user, (const uint8_t *)&e, sizeof e);
    free(q);
}

static void answer_status(cb_http_res *res, int status) { cb_res_send(res, status, "application/octet-stream", NULL, 0); }

static void h_eval(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_user u;
    if (!ctx->auth || cb_auth_verify_bearer(ctx->auth, req, &u) != CB_OK) { answer_status(res, 401); return; }
    cw_eval_req q;
    if (req->body_len < sizeof q) { answer_status(res, 400); return; }
    memcpy(&q, req->body, sizeof q);
    if (q.n > CC_MAX_PLY || req->body_len != sizeof q + (size_t)q.n * 2) { answer_status(res, 400); return; }
    ch_move moves[CC_MAX_PLY];
    memcpy(moves, req->body + sizeof q, (size_t)q.n * 2);
    cc_game g;
    cc_game_new(&g, 0, 0, 0);
    g.left_ms[0] = g.left_ms[1] = INT64_MAX / 4;   /* the rules alone */
    if (cc_game_replay(&g, moves, q.n) != CC_OK) { answer_status(res, 400); return; }
    pthread_mutex_lock(&cc_lock);
    asked++;
    cw_eval *c = slot(g.pos.key);
    bool hit = c && c->key == g.pos.key && c->v == CW_V;
    cw_eval e;
    if (hit) { e = *c; e.ply = q.n; cached++; }
    pthread_mutex_unlock(&cc_lock);
    if (hit) { cc_push_user(u.id, (const uint8_t *)&e, sizeof e); answer_status(res, 200); return; }
    if (!cc_engine_available()) { answer_status(res, 503); return; }
    ask_ud *ud = calloc(1, sizeof *ud);
    if (!ud) { answer_status(res, 503); return; }
    memcpy(ud->user, u.id, CB_UUID_LEN);
    ud->key = g.pos.key;
    ud->ply = q.n;
    ud->stm = g.pos.stm;
    cc_engine_ask ask = { moves, q.n, 20, EVAL_CAP_MS, 0, EVAL_NODES, 1, NULL };
    if (cc_engine_search(&ask, answered, ud) != CB_OK) {
        free(ud);
        pthread_mutex_lock(&cc_lock);
        refused++;
        pthread_mutex_unlock(&cc_lock);
        answer_status(res, 503);
        return;
    }
    answer_status(res, 200);
}

void cc_analysis_counts(uint64_t *a, uint64_t *c, uint64_t *r) {
    pthread_mutex_lock(&cc_lock);
    *a = asked; *c = cached; *r = refused;
    pthread_mutex_unlock(&cc_lock);
}

static const cb_route ROUTES[] = {
    { "POST", "/c/eval", CB_ROUTE_POOL | CB_ROUTE_INLINE | CB_ROUTE_HOME, h_eval, 500 },
};
const cb_route_list cc_analysis_routes = { ROUTES, 1 };
