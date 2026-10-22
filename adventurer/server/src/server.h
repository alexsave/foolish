// server.h - the one instance of the product inside the process: the graph, the world, the
// workers, and what the routes need to know about them. Set up by adventurer_app.c's serve_open.
#ifndef ADV_SERVER_H
#define ADV_SERVER_H

#include "cbase/app.h"
#include "ctx.h"
#include "graph.h"
#include "metrics.h"
#include "net/router.h"
#include "work.h"
#include "world.h"

#include <stdint.h>

// WHAT THE SERVER COUNTS, as the platform's metrics registry holds it (c_base metrics.h): the
// table is adventurer_app.c's ADV_METRICS in this order, and every update names its row here.
// Histograms are microseconds. The platform adds its own rows (requests, connections) and
// serves the lot at GET /metrics; the ops frame (wire.h adv_w_ops) reads the same registry.
enum {
    ADV_M_ACT_CPU,     // hist: match + merge per activity
    ADV_M_TILE,        // hist: one tile render
    ADV_M_E2E,         // hist: a live upload's arrival to its feed frame
    ADV_M_ACTS,        // rate: activities processed
    ADV_M_TILES,       // rate: tiles answered (rendered or cached)
    ADV_M_TILES_304,   // counter: tiles answered not-modified
    ADV_M_FEED_CONNS,  // gauge: open feed sockets
    ADV_M_N
};

typedef struct {
    cb_ctx    *ctx;
    adv_graph  graph;
    adv_world  world;
    adv_work  *work;
    int64_t    started_ms;
    uint32_t   recovered;
    char       db_path[1024];
    bool       open;
} adv_server;

extern adv_server adv_srv;

// The product's HTTP routes (routes.c) and the ops frame (wire.h adv_w_ops) as of now.
extern const cb_route_list adv_route_list;
void adv_ops_fill(adv_w_ops *out);

#endif
