// world.h - THE AGGREGATE: every table the sim holds, as the site sees it (wire.h be_w_table),
// with the hands the sim's own humans hold and the last events of each table. The player threads
// write it after each push they apply (one lock per change, under a single mutex: a few hundred
// takes a second at most), the routes read it, and every change goes out at once through c_base
// realtime (birdseye_app.c: the family s/{doc}, room "all").
#ifndef BE_WORLD_H
#define BE_WORLD_H

#include "wire.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BE_WORLD_MAX_TABLES 4096
#define BE_WORLD_KEEP_GONE_MS 90000   // a deleted table stays on the site this long

typedef struct {
    bool       used;
    be_w_table w;
    be_w_hand  hands[BE_SEATS];
    be_w_event events[BE_EVENTS];   // a ring, oldest at ev_head
    int        ev_head, ev_n;
    int64_t    gone_ms;
} be_table;

typedef struct {
    pthread_mutex_t mu;
    be_table *tables;
    int       cap, n_used;
    uint32_t  live, lobby, playing;   // maintained on every phase change
    // Where a change goes: the app's publish into c_base realtime (birdseye_app.c), NULL for none.
    void    (*publish)(const void *frame, size_t len);
} be_world;

extern be_world be_world_g;

int  be_world_init(be_world *w, int cap);
void be_world_free(be_world *w);

// Every write happens between these. A caller that opened a table, changed a row or added an
// event calls be_world_changed / be_world_event before unlocking so the feed sees it in order.
void be_world_lock(be_world *w);
void be_world_unlock(be_world *w);

// The table of `gid` (locked), or NULL.
be_table *be_world_find(be_world *w, const char *gid, int gid_len);
// A fresh table for `gid` (locked): phase LOBBY, created now (every stamp is the wall clock, cb_now_ms).
// NULL when full or the id is taken.
be_table *be_world_open(be_world *w, const char *gid, int gid_len);
// A phase change (locked): keeps the live/lobby/playing counts right and stamps the times.
void be_world_set_phase(be_world *w, be_table *t, uint8_t phase);
// The row changed (locked): stamps changed_ms and publishes it on the feed.
void be_world_changed(be_world *w, be_table *t);
// An event at `t` (locked; t may be NULL for one with no table): appended to the table's ring
// and published on the feed.
void be_world_event(be_world *w, be_table *t, const be_w_event *e);
// Forgets GONE tables older than BE_WORLD_KEEP_GONE_MS (locked).
void be_world_prune(be_world *w);

// The answers (they lock): the list body (be_w_list then rows) into a growing buffer the caller
// owns (returns the bytes, or 0 when out of memory), and one table's detail (false: no such gid).
size_t be_world_list(be_world *w, void **out);
bool   be_world_detail(be_world *w, const char *gid, int gid_len, be_w_detail *out);

#endif
