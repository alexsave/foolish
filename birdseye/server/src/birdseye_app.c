// birdseye_app.c - birdseye as the platform's app (cbase/app.h): its routes (routes.c), its one
// topic family (the aggregate's pushes, through c_base realtime to the room "all" of s/{doc}),
// and serve_open, where the sim's player threads start against the game server.
//
// Settings, beside the platform's CBASE_* (config.h):
//   BE_MODE          "league" (the default): the server's bots play each other at tables of their own
//                    (league.h); "tournament": they play the stored tournament, or a new one when
//                    none is running (tournament.h); "mixed": fake humans and some bots (sim.h)
//   BE_TOURNAMENT_FILE the tournament's file (default ../data/tournament.bin), shown in every mode
//   BE_SERVER        the game server, host:port (default 127.0.0.1:8811)
//   BE_SERVER_PID    its pid, for the RSS and CPU in the ops frame (0: unknown)
// The league's:
//   BE_ADMIN_TOKEN   the game server's CBASE_ADMIN_TOKEN (its dev-only POST /dev/bot_table)
//   BE_LEAGUE_TABLES live league tables at once (default 10)
//   BE_LEAGUE_PER_BOT tables one bot may sit at at once (default 1)
//   BE_LEAGUE_FILE   the rated games' history (default ../data/league.bin; "" keeps none)
//   BE_PODKIDNOY     percent of tables under podkidnoy rules (default 20 mixed, 30 league)
//   BE_SEED          the seed (default 1)
// The mixed sim's:
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

#include "be_metrics.h"
#include "gauges.h"
#include "league.h"
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
cb_metrics *be_mx;
uint8_t be_mode;
static int g_server_pid;

// THE NAMES, in be_metrics.h's order (the enum indexes this table).
const cb_metric_def be_metric_defs[BE_M_N] = {
    { "move_ack_us", CB_METRIC_HIST },      { "move_push_us", CB_METRIC_HIST },  { "cast_decode_us", CB_METRIC_HIST },
    { "req_create_us", CB_METRIC_HIST },    { "req_ask_us", CB_METRIC_HIST },    { "req_read_us", CB_METRIC_HIST },
    { "game_s", CB_METRIC_HIST },
    { "moves", CB_METRIC_RATE },            { "games", CB_METRIC_RATE },         { "pushes", CB_METRIC_RATE },
    { "push_bytes", CB_METRIC_RATE },       { "reqs", CB_METRIC_RATE },
    { "refused", CB_METRIC_COUNTER },       { "hostile", CB_METRIC_COUNTER },    { "hostile_taken", CB_METRIC_COUNTER },
    { "resyncs", CB_METRIC_COUNTER },       { "stalls", CB_METRIC_COUNTER },
    { "tables_live", CB_METRIC_GAUGE },     { "tables_lobby", CB_METRIC_GAUGE }, { "tables_playing", CB_METRIC_GAUGE },
    { "players", CB_METRIC_GAUGE },         { "players_seated", CB_METRIC_GAUGE }, { "sockets", CB_METRIC_GAUGE },
    { "bots_seated", CB_METRIC_GAUGE },     { "threads", CB_METRIC_GAUGE },      { "server_rss_mb", CB_METRIC_GAUGE },
    { "server_cpu_cpct", CB_METRIC_GAUGE },
};

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

// The numbers, once a second on the loop thread: the gauges written, then the platform's
// report pushed to the room as it answers GET /metrics (loop-thread delivery is immediate).
static void ops_tick(cb_loop *l, void *ud) {
    be_gauges_common(g_server_pid);
    if (be_mode != BE_MODE_MIXED) be_league_gauges();
    else be_sim_gauges();
    if (cb_rt_room_heard(g_ctx->rt, BE_ROOM) && g_ctx->metrics) {
        size_t size = cb_metrics_report_size(g_ctx->metrics);
        uint8_t *frame = malloc(2 + size);
        if (frame) {
            frame[0] = BE_SF_V;
            frame[1] = BE_F_OPS;
            size_t n = cb_metrics_report(g_ctx->metrics, g_ctx, cb_now_ms(), frame + 2, size);
            if (n) be_publish(frame, 2 + n);
            free(frame);
        }
    }
    be_world_lock(&be_world_g);
    be_world_prune(&be_world_g);
    be_world_unlock(&be_world_g);
    cb_loop_timer(l, 1000, ops_tick, ud);
}

static void ops_arm(cb_loop *l, void *ud) { cb_loop_timer(l, 1000, ops_tick, ud); }

static int be_serve_open(cb_ctx *ctx, char *err, size_t cap) {
    g_ctx = ctx;
    be_mx = ctx->metrics;
    if (be_world_init(&be_world_g, BE_WORLD_MAX_TABLES) != 0) {
        snprintf(err, cap, "birdseye: no memory for the world");
        return CB_E_NOMEM;
    }
    be_world_g.publish = be_publish;
    const char *server = getenv("BE_SERVER");
    if (!server || !*server) server = "127.0.0.1:8811";
    g_server_pid = (int)env_long("BE_SERVER_PID", 0, 0, INT32_MAX);
    const char *mode = getenv("BE_MODE");
    if (!mode || !*mode || strcmp(mode, "league") == 0) be_mode = BE_MODE_LEAGUE;
    else if (strcmp(mode, "tournament") == 0) be_mode = BE_MODE_TOURNAMENT;
    else if (strcmp(mode, "mixed") == 0) be_mode = BE_MODE_MIXED;
    else {
        snprintf(err, cap, "birdseye: BE_MODE='%s' is not league, tournament or mixed", mode);
        return CB_E_INVALID;
    }
    int rc;
    if (be_mode == BE_MODE_LEAGUE || be_mode == BE_MODE_TOURNAMENT) {
        be_league_opts o;
        memset(&o, 0, sizeof o);
        snprintf(o.server, sizeof o.server, "%s", server);
        const char *tok = getenv("BE_ADMIN_TOKEN");
        snprintf(o.admin_token, sizeof o.admin_token, "%s", tok ? tok : "");
        const char *file = getenv("BE_LEAGUE_FILE");
        snprintf(o.file, sizeof o.file, "%s", file ? file : "../data/league.bin");
        o.tournament = be_mode == BE_MODE_TOURNAMENT;
        const char *tf = getenv("BE_TOURNAMENT_FILE");
        snprintf(o.tour_file, sizeof o.tour_file, "%s", tf ? tf : "../data/tournament.bin");
        o.tables = (int)env_long("BE_LEAGUE_TABLES", o.tournament ? 16 : 10, 1, 64);
        o.tables_per_bot = (int)env_long("BE_LEAGUE_PER_BOT", 1, 1, 8);
        o.podkidnoy = (int)env_long("BE_PODKIDNOY", 30, 0, 100);
        o.seed = (uint64_t)env_long("BE_SEED", 1, 0, INT32_MAX);
        if ((rc = be_league_start(&o, err, cap)) != CB_OK) return rc;
    } else {
        be_sim_opts o;
        memset(&o, 0, sizeof o);
        snprintf(o.server, sizeof o.server, "%s", server);
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
        if ((rc = be_sim_start(&o, err, cap)) != CB_OK) return rc;
        CB_INFO("birdseye", "sim: %d players on %d threads, %d tables live, think %d-%d ms, bots %d%%, podkidnoy %d%%, hostile 1/%d, rematch %d%%, against %s",
                o.players, o.threads, o.tables, o.think_min_ms, o.think_max_ms, o.bot_share, o.podkidnoy, o.hostile_every, o.rematch, o.server);
    }
    if (ctx->loop) cb_loop_post(ctx->loop, ops_arm, NULL);
    return CB_OK;
}

static void be_serve_close(cb_ctx *ctx) {
    if (be_mode != BE_MODE_MIXED) be_league_stop();
    else be_sim_stop();
    be_world_g.publish = NULL;
    be_world_free(&be_world_g);
    be_mx = NULL;
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
    .metrics = be_metric_defs,
    .n_metrics = BE_M_N,
};

const cb_app *const cb_main_app = &BE_APP;
