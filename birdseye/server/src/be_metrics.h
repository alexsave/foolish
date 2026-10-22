// be_metrics.h - WHAT BIRDSEYE COUNTS: the names, as the platform's registry wants them
// (c_base/core/src/metrics.h): one row per metric, declared once in birdseye_app.c
// (be_metric_defs, in this order), updated by index from the player threads and the ops timer,
// reported by the platform (GET /metrics, and the BE_F_OPS push once a second) in its own fixed
// layout, which the site decodes with the reader structgen writes from the platform's header.
// Nothing of the machinery lives here: the histogram, the rate ring, the counters and the
// process readers are the platform's (util/metrics.h).
#ifndef BE_METRICS_H
#define BE_METRICS_H

#include "metrics.h"

enum {
    // histograms, microseconds
    BE_M_MOVE_ACK,       // a move: request out to reply in
    BE_M_MOVE_PUSH,      // a move: request out to the mover's own cast of it in
    BE_M_CAST_DECODE,    // a cast's arrival to applied on the client table
    BE_M_REQ,            // every request that is not a move (asks, creates, reads)
    BE_M_GAME_S,         // a finished game's length, deal to fool, in SECONDS
    // rates, per second
    BE_M_MOVES,
    BE_M_GAMES,
    BE_M_PUSHES,         // frames heard on every player socket
    BE_M_PUSH_BYTES,
    BE_M_REQS,           // requests sent, moves included
    // counters
    BE_M_REFUSED,        // legitimate moves the server refused (a sim bug or a server bug)
    BE_M_HOSTILE,        // hostile moves sent
    BE_M_HOSTILE_TAKEN,  // hostile moves the server applied (each one is a finding)
    BE_M_RESYNCS,        // games refetched after a cast could not be applied
    BE_M_STALLS,         // tables left because nobody moved for a long time
    // gauges
    BE_M_TABLES_LIVE,
    BE_M_TABLES_LOBBY,
    BE_M_TABLES_PLAYING,
    BE_M_PLAYERS,
    BE_M_PLAYERS_SEATED,
    BE_M_SOCKETS,
    BE_M_BOTS_SEATED,
    BE_M_THREADS,
    BE_M_SERVER_RSS_MB,
    BE_M_SERVER_CPU_CPCT,   // the game server's CPU over the last second, hundredths of a percent of one core
    BE_M_N
};

extern const cb_metric_def be_metric_defs[BE_M_N];

// The registry the sim bumps: ctx->metrics once the app is open, NULL before (every update on a
// NULL registry is ignored by the platform, so no writer checks).
extern cb_metrics *be_mx;

#endif
