// feed.h - THE LIVE FEED, on the platform's realtime (c_base realtime.h): every processed upload
// (an adv_w_activity) and, every second, the operating numbers (an adv_w_ops) are pushes to ONE
// ROOM, "feed", of the app's one topic family, which is ANONYMOUS (cbase/app.h): the feed is the
// demo's public window, so a page that never signed in hears it. Under `adventurer serve` the
// page opens GET /ws/public, says an empty HELLO and SUBs the room (shared/c/stream_frame.h, the
// frames' heads written and read by code structgen generates); the hub delivers each push to the
// room's watchers exactly as it delivers foolish's spectators' casts (rt_local.c pub_deliver).
// Publish from any thread: the platform copies the frame and hands it to the loop.
#ifndef ADV_FEED_H
#define ADV_FEED_H

#include "cbase/app.h"
#include "ctx.h"
#include "wire.h"

#include <stddef.h>

#define ADV_FEED_ROOM "feed"

extern const cb_topic_family adv_fam_feed;   // "s/{doc}", anonymous; its one room is ADV_FEED_ROOM
void adv_feed_publish(cb_ctx *ctx, const void *frame, size_t len);
// Arms the once-a-second ops push on the loop: fill writes the current adv_w_ops.
typedef void (*adv_ops_fill_fn)(adv_w_ops *out);
void adv_feed_start_ops(cb_ctx *ctx, adv_ops_fill_fn fill);
// The feed room's watchers as last counted on the loop (the ops tick), for a display.
unsigned adv_feed_conns(void);

#endif
