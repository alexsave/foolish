// server.h - the one instance of the product inside the process: the graph, the world, the
// workers, and what the routes need to know about them. Set up by adventurer_app.c's serve_open.
#ifndef ADV_SERVER_H
#define ADV_SERVER_H

#include "cbase/app.h"
#include "ctx.h"
#include "graph.h"
#include "net/router.h"
#include "work.h"
#include "world.h"

#include <stdint.h>

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
