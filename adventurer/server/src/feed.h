// feed.h - THE LIVE FEED: GET /adv/feed upgrades to a plain binary WebSocket (net/ws.h) on the
// loop thread; every frame is one wire.h record (an adv_w_activity as each upload is processed,
// an adv_w_ops every second). No subscription, no auth: the feed is the demo's public window.
// Publish from any thread: the frame is copied and posted to the loop, which writes it to every
// open connection in one pass (one send per connection per pass, the server's rule).
#ifndef ADV_FEED_H
#define ADV_FEED_H

#include "cbase/app.h"
#include "ctx.h"
#include "wire.h"

#include <stddef.h>

#define ADV_FEED_MAX_CONNS 256

extern const cb_route_list adv_feed_route_list;   // GET /adv/feed, a loop route
void adv_feed_publish(cb_ctx *ctx, const void *frame, size_t len);
// Arms the once-a-second ops frame: fill writes the current adv_w_ops.
typedef void (*adv_ops_fill_fn)(adv_w_ops *out);
void adv_feed_start_ops(cb_ctx *ctx, adv_ops_fill_fn fill);
unsigned adv_feed_conns(void);

#endif
