// birdseye_app.c - birdseye as the platform's app (cbase/app.h): its routes (routes.c), its one
// topic family (the aggregate's pushes, through c_base realtime to the room "all" of s/{doc}),
// and serve_open, where the sim's player threads start against the game server.
//
// Settings, beside the platform's CBASE_* (config.h):
//   BE_SERVER        the game server, host:port (default 127.0.0.1:8811)
//   BE_SERVER_PID    its pid, for the RSS and CPU in the ops frame (0: unknown)
//   BE_PLAYERS       fake humans (default 60)
//   BE_TABLES        tables kept live at once (default BE_PLAYERS / 2)
//   BE_THREADS       player threads (default 2)
//   BE_THINK_MS      think time per move, "min-max" ms (default 800-2500)
//   BE_SEED          the sim's seed (default 1)
//   BE_BOT_SHARE     percent of tables with a server bot in a seat (default 40)
//   BE_PODKIDNOY     percent of tables under podkidnoy rules (default 20)
//   BE_HOSTILE_EVERY one move in N is hostile (default 50; 0 never)
//   BE_REMATCH       percent of finished games that rematch (default 30)
#include "cbase/app.h"

#include "metrics.h"
#include "net/loop.h"
#include "realtime/realtime.h"
#include "routes.h"
#include "sim.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "world.h"
#include "../../../shared/c/num_parse.h"
#include "../../../shared/c/stream_frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(BE_SF_V == SF_V, "wire.h BE_SF_V must be stream_frame.h's SF_V");

static cb_ctx *g_ctx;
static const cb_topic_family BE_FAMILY = { "birdseye", "s/{doc}" };
static const cb_topic_family *const BE_TOPICS[] = { &BE_FAMILY };
#define BE_ROOM "all"

// Every frame goes to the one room; the hub copies it and posts it to the loop (any thread).
void be_publish(const void *frame, size_t len) {
    if (!g_ctx || !g_ctx->rt) return;
    cb_rt_to to = { &BE_FAMILY, NULL, BE_ROOM, NULL };
    cb_rt_publish(g_ctx->rt, &to, 1, frame, len);
}

static long env_long(const char *name, long dflt, long lo, long hi) {
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    int64_t n;
    return num_parse_i64(v, strlen(v), lo, hi, &n) ? (long)n : dflt;
}

// The ops frame, once a second on the loop thread (loop-thread delivery is immediate).
static void ops_tick(cb_loop *l, void *ud) {
    if (cb_rt_room_heard(g_ctx->rt, BE_ROOM)) {
        uint8_t frame[2 + sizeof(be_w_ops)];
        frame[0] = BE_SF_V;
        frame[1] = BE_F_OPS;
        be_w_ops o;
        be_ops_fill(&o);
        memcpy(frame + 2, &o, sizeof o);
        be_publish(frame, sizeof frame);
    }
    be_world_lock(&be_world_g);
    be_world_prune(&be_world_g);
    be_world_unlock(&be_world_g);
    cb_loop_timer(l, 1000, ops_tick, ud);
}

static void ops_arm(cb_loop *l, void *ud) { cb_loop_timer(l, 1000, ops_tick, ud); }

static int be_serve_open(cb_ctx *ctx, char *err, size_t cap) {
    g_ctx = ctx;
    if (be_world_init(&be_world_g, BE_WORLD_MAX_TABLES) != 0) {
        snprintf(err, cap, "birdseye: no memory for the world");
        return CB_E_NOMEM;
    }
    be_world_g.publish = be_publish;
    be_sim_opts o;
    memset(&o, 0, sizeof o);
    const char *server = getenv("BE_SERVER");
    snprintf(o.server, sizeof o.server, "%s", server && *server ? server : "127.0.0.1:8811");
    o.server_pid = (int)env_long("BE_SERVER_PID", 0, 0, INT32_MAX);
    o.players = (int)env_long("BE_PLAYERS", 60, 1, BE_SIM_MAX_PLAYERS);
    o.tables = (int)env_long("BE_TABLES", o.players / 2 > 0 ? o.players / 2 : 1, 1, BE_WORLD_MAX_TABLES);
    o.threads = (int)env_long("BE_THREADS", 2, 1, BE_SIM_MAX_THREADS);
    o.think_min_ms = 800;
    o.think_max_ms = 2500;
    const char *think = getenv("BE_THINK_MS");
    if (think && *think) {
        int a = 0, b = 0;
        if (sscanf(think, "%d-%d", &a, &b) == 2 && a >= 0 && b >= a) { o.think_min_ms = a; o.think_max_ms = b; }
        else if (sscanf(think, "%d", &a) == 1 && a >= 0) o.think_min_ms = o.think_max_ms = a;
    }
    o.seed = (uint64_t)env_long("BE_SEED", 1, 0, INT32_MAX);
    o.bot_share = (int)env_long("BE_BOT_SHARE", 40, 0, 100);
    o.podkidnoy = (int)env_long("BE_PODKIDNOY", 20, 0, 100);
    o.hostile_every = (int)env_long("BE_HOSTILE_EVERY", 50, 0, 1000000);
    o.rematch = (int)env_long("BE_REMATCH", 30, 0, 100);
    int rc = be_sim_start(&o, err, cap);
    if (rc != CB_OK) return rc;
    CB_INFO("birdseye", "sim: %d players on %d threads, %d tables live, think %d-%d ms, bots %d%%, podkidnoy %d%%, hostile 1/%d, rematch %d%%, against %s",
            o.players, o.threads, o.tables, o.think_min_ms, o.think_max_ms, o.bot_share, o.podkidnoy, o.hostile_every, o.rematch, o.server);
    if (ctx->loop) cb_loop_post(ctx->loop, ops_arm, NULL);
    return CB_OK;
}

static void be_serve_close(cb_ctx *ctx) {
    be_sim_stop();
    be_world_g.publish = NULL;
    be_world_free(&be_world_g);
    g_ctx = NULL;
}

static const cb_route_list *const BE_ROUTES[] = { &be_route_list };

static const cb_app BE_APP = {
    .name = "birdseye",
    .routes = BE_ROUTES,
    .n_route_lists = 1,
    .topics = BE_TOPICS,
    .n_topics = 1,
    .serve_open = be_serve_open,
    .serve_close = be_serve_close,
};

const cb_app *const cb_main_app = &BE_APP;
