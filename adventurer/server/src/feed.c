// feed.c - feed.h.
#include "feed.h"

#include "server.h"
#include "net/loop.h"
#include "net/router.h"
#include "net/ws.h"
#include "util/err.h"
#include "util/log.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

// Loop-thread state: the open sockets.
static cb_ws  *g_conns[ADV_FEED_MAX_CONNS];
static int     g_n;
static atomic_uint g_count;

static void on_open(cb_ws *ws, void *ud) {
    if (g_n >= ADV_FEED_MAX_CONNS) {
        cb_ws_close(ws, 1013);   // try again later
        return;
    }
    g_conns[g_n++] = ws;
    atomic_store(&g_count, (unsigned)g_n);
    cb_metrics_set(adv_srv.ctx ? adv_srv.ctx->metrics : NULL, ADV_M_FEED_CONNS, (uint64_t)g_n);
}

static void on_message(cb_ws *ws, int opcode, const uint8_t *data, size_t len, void *ud) {
    // Nothing is expected from a reader; a client that talks is closed (a bounded inbox is no inbox).
    cb_ws_close(ws, 1003);
}

static void on_close(cb_ws *ws, void *ud) {
    for (int i = 0; i < g_n; i++)
        if (g_conns[i] == ws) {
            g_conns[i] = g_conns[--g_n];
            break;
        }
    atomic_store(&g_count, (unsigned)g_n);
    cb_metrics_set(adv_srv.ctx ? adv_srv.ctx->metrics : NULL, ADV_M_FEED_CONNS, (uint64_t)g_n);
}

static const cb_ws_handlers FEED_WS = { on_open, on_message, on_close };

static void feed_upgrade(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_ws_upgrade(req, res, &FEED_WS, NULL);   // on refusal res already holds the 400/426
}

static const cb_route FEED_ROUTES[] = {
    { "GET", "/adv/feed", CB_ROUTE_LOOP, feed_upgrade, 0 },
};
const cb_route_list adv_feed_route_list = { FEED_ROUTES, 1 };

unsigned adv_feed_conns(void) { return atomic_load(&g_count); }

typedef struct {
    size_t  len;
    uint8_t bytes[];
} frame;

static void send_all(cb_loop *l, void *ud) {
    frame *f = ud;
    for (int i = 0; i < g_n; i++) {
        // A send may close a slow reader (CB_E_FULL), whose on_close shuffles the list: walk by
        // index and re-check, never hold a pointer across a send.
        cb_ws *ws = g_conns[i];
        int rc = cb_ws_send(ws, CB_WS_OP_BIN, f->bytes, f->len);
        if (rc != CB_OK && i < g_n && g_conns[i] != ws) i--;
    }
    free(f);
}

void adv_feed_publish(cb_ctx *ctx, const void *bytes, size_t len) {
    if (!ctx->loop || atomic_load(&g_count) == 0) return;
    frame *f = malloc(sizeof *f + len);
    if (!f) return;
    f->len = len;
    memcpy(f->bytes, bytes, len);
    if (cb_loop_post(ctx->loop, send_all, f) != CB_OK) free(f);
}

typedef struct {
    cb_ctx         *ctx;
    adv_ops_fill_fn fill;
} ops_timer;

static void ops_tick(cb_loop *l, void *ud) {
    ops_timer *t = ud;
    if (g_n > 0) {
        adv_w_ops o;
        t->fill(&o);
        frame *f = malloc(sizeof *f + sizeof o);
        if (f) {
            f->len = sizeof o;
            memcpy(f->bytes, &o, sizeof o);
            send_all(l, f);
        }
    }
    cb_loop_timer(l, 1000, ops_tick, t);
}

static void ops_arm(cb_loop *l, void *ud) { cb_loop_timer(l, 1000, ops_tick, ud); }

void adv_feed_start_ops(cb_ctx *ctx, adv_ops_fill_fn fill) {
    static ops_timer t;
    t.ctx = ctx;
    t.fill = fill;
    if (ctx->loop) cb_loop_post(ctx->loop, ops_arm, &t);
}
