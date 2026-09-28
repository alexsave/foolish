// metrics.h - what this process will tell you about itself.
//
// Two readings of the same gauges and counters, for two different audiences:
// GET /stats for a load tool (foolish_hammer's --mode=ws summary,
// bot_stress.sh) that samples a before/after delta around a timed run, and GET
// /metrics for a Prometheus scrape. The counters themselves belong to the
// domains that move them (bots.h, play.h, httpd.h, registry.h); this file only
// reads them.
#ifndef FOOLISH_METRICS_H
#define FOOLISH_METRICS_H

#include "httpd.h"

// GET /stats - a tiny counter dump: no game_id, no auth, process-wide, one
// packed CTL_STATS frame (ctl_wire.h). Answered inline off the acceptor (see
// acceptor_main), same as /health - cheap atomic loads, no lock, no reason to
// pay a work-queue round trip.
void h_stats(Req *r, Conn *conn);

// GET /metrics - Prometheus text exposition of the same gauges/counters
// /stats carries (plus rate-limit rejections), for Fly/Grafana scraping.
// Text, not a packed frame and not a binary buffer - Prometheus can only
// scrape the line-based text format (see respond_text). The game wire stays
// binary; this endpoint's audience is the monitoring stack, not the game
// client.
void h_metrics(Req *r, Conn *conn);

#endif
