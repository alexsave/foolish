// feed.c - feed.h.
#include "feed.h"

#include "server.h"
#include "net/loop.h"
#include "realtime/realtime.h"

#include <stdatomic.h>

const cb_topic_family adv_fam_feed = { .name = "feed", .pattern = "s/{doc}", .anonymous = true };

static const cb_rt_to FEED_TO = { .family = &adv_fam_feed, .doc_id = ADV_FEED_ROOM };
static atomic_uint g_watchers;

unsigned adv_feed_conns(void) { return atomic_load_explicit(&g_watchers, memory_order_relaxed); }

void adv_feed_publish(cb_ctx *ctx, const void *bytes, size_t len) {
    cb_rt_publish(ctx->rt, &FEED_TO, 1, bytes, len);
}

typedef struct {
    cb_ctx         *ctx;
    adv_ops_fill_fn fill;
} ops_timer;

// On the loop, once a second: the watchers counted, and the ops pushed when anyone watches.
static void ops_tick(cb_loop *l, void *ud) {
    ops_timer *t = ud;
    int n = cb_rt_room_watchers(t->ctx->rt, ADV_FEED_ROOM);
    atomic_store_explicit(&g_watchers, n > 0 ? (unsigned)n : 0, memory_order_relaxed);
    cb_metrics_set(t->ctx->metrics, ADV_M_FEED_CONNS, n > 0 ? (uint64_t)n : 0);
    if (cb_rt_room_heard(t->ctx->rt, ADV_FEED_ROOM)) {
        adv_w_ops o;
        t->fill(&o);
        adv_feed_publish(t->ctx, &o, sizeof o);   // on the loop: delivered at once, not copied
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
