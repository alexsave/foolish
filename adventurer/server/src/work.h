// work.h - THE UPLOAD QUEUE AND THE MATCHER WORKERS: an accepted upload's bytes wait in one of two
// lanes (live first, backfill when live is empty, so a user connecting with years of history never
// delays today's ride), and a worker thread takes the next, matches it (match.h), applies the
// result to the world under the user's lock (world.h), journals it (journal.h) and tells the feed.
//
// The lanes and the workers are the platform's (c_base util/lanes.h): bounded by bytes, not items
// (a lane refuses with CB_E_FULL, the route answers 503, when the bytes it holds would pass its
// cap), the workers a pool started from serve_open, before the loop takes its CPU, so they run on
// the background CPU when the knob pins. What is queued at a stop is dropped: it was never
// acknowledged as processed.
#ifndef ADV_WORK_H
#define ADV_WORK_H

#include "ctx.h"
#include "store/store.h"
#include "world.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct adv_work adv_work;

#define ADV_WORK_MAX_WORKERS 64

typedef struct {
    int    workers;            // threads (>= 1)
    size_t live_cap_bytes;     // 0: 64 MiB
    size_t backfill_cap_bytes; // 0: 256 MiB
    int    cpu_budget_us;      // the matcher's per-activity budget (0: match.h's default)
} adv_work_opts;

// Starts the workers. CB_OK, CB_E_NOMEM, CB_E_STATE (a thread could not start).
int  adv_work_start(adv_work **out, cb_ctx *ctx, adv_world *w, const adv_work_opts *o);
// Stops taking uploads, drops what is queued, waits for the uploads being matched, joins the workers.
void adv_work_stop(adv_work *wk);

// Queues a copy of body (already parsed and found valid by the route). On CB_OK *activity is the
// id the result will carry and *q_live/*q_backfill the lanes' depths after the add; CB_E_FULL
// when its lane is full (503). Its end-to-end time starts here (the lanes' put time).
int  adv_work_enqueue(adv_work *wk, const uint8_t *body, size_t len, bool backfill, uint32_t *activity, uint32_t *q_live,
                      uint32_t *q_backfill);
void adv_work_depths(const adv_work *wk, uint32_t *q_live, uint32_t *q_backfill);
int  adv_work_threads(const adv_work *wk);

#endif
