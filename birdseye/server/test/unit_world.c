// unit_world.c - the aggregate: tables opened and found, the phase counts, every change and event
// published as a frame of the wire, the event ring, the list and detail answers, pruning.
#include "t.h"
#include "wire.h"
#include "world.h"
#include "../../../shared/c/stream_frame.h"

#include <stdlib.h>

static int g_frames, g_tables, g_events;
static uint8_t g_last_kind;
static be_w_table g_last_table;
static be_w_event g_last_event;

static void capture(const void *frame, size_t len) {
    const uint8_t *p = frame;
    g_frames++;
    T_CHECK(len >= 2 && p[0] == SF_V);
    g_last_kind = p[1];
    if (p[1] == BE_F_TABLE) {
        g_tables++;
        T_EQ_INT(len, 2 + sizeof(be_w_table));
        memcpy(&g_last_table, p + 2, sizeof g_last_table);
    } else if (p[1] == BE_F_EVENT) {
        g_events++;
        T_EQ_INT(len, 2 + sizeof(be_w_event));
        memcpy(&g_last_event, p + 2, sizeof g_last_event);
    }
}

static be_w_event ev(const char *gid, int kind, int seat) {
    be_w_event e;
    memset(&e, 0, sizeof e);
    e.gid_len = (uint8_t)strlen(gid);
    memcpy(e.gid, gid, e.gid_len);
    e.kind = (uint8_t)kind;
    e.seat = (int8_t)seat;
    return e;
}

static void tables_and_phases(void) {
    be_world w;
    T_EQ_INT(be_world_init(&w, 4), 0);
    w.publish = capture;
    be_world_lock(&w);
    be_table *a = be_world_open(&w, "aaaa", 4);
    T_CHECK(a != NULL);
    T_CHECK(be_world_open(&w, "aaaa", 4) == NULL);   // taken
    T_CHECK(be_world_open(&w, "", 0) == NULL);        // not an id
    T_CHECK(be_world_find(&w, "aaaa", 4) == a);
    T_CHECK(be_world_find(&w, "aaab", 4) == NULL);
    T_EQ_INT(a->w.phase, BE_PHASE_LOBBY);
    T_EQ_INT(a->w.fool, -1);
    T_CHECK(a->w.created_ms > 1700000000000LL);   // the wall clock, not the monotonic one
    T_EQ_INT(w.live, 1);
    T_EQ_INT(w.lobby, 1);
    T_EQ_INT(w.playing, 0);
    be_world_set_phase(&w, a, BE_PHASE_PLAYING);
    T_EQ_INT(w.lobby, 0);
    T_EQ_INT(w.playing, 1);
    be_world_set_phase(&w, a, BE_PHASE_OVER);
    T_EQ_INT(w.playing, 0);
    T_EQ_INT(w.live, 1);
    T_CHECK(a->w.finished_ms > 0);
    be_world_set_phase(&w, a, BE_PHASE_GONE);
    T_EQ_INT(w.live, 0);
    // Back to the lobby (a rematch after GONE never happens, but the counts must stay sane).
    be_table *b = be_world_open(&w, "bbbb", 4);
    be_world_set_phase(&w, b, BE_PHASE_PLAYING);
    be_world_set_phase(&w, b, BE_PHASE_LOBBY);
    T_EQ_INT(b->w.finished_ms, 0);
    T_EQ_INT(w.lobby, 1);
    T_EQ_INT(w.live, 1);
    // Full: four slots.
    T_CHECK(be_world_open(&w, "cccc", 4) != NULL);
    T_CHECK(be_world_open(&w, "dddd", 4) != NULL);
    T_CHECK(be_world_open(&w, "eeee", 4) == NULL);
    be_world_unlock(&w);
    be_world_free(&w);
}

static void changes_and_events_publish(void) {
    be_world w;
    T_EQ_INT(be_world_init(&w, 4), 0);
    w.publish = capture;
    g_frames = g_tables = g_events = 0;
    be_world_lock(&w);
    be_table *a = be_world_open(&w, "aaaa", 4);
    a->w.version = 7;
    be_world_changed(&w, a);
    T_EQ_INT(g_tables, 1);
    T_EQ_INT(g_last_table.version, 7);
    T_EQ_INT(g_last_table.gid_len, 4);
    T_CHECK(g_last_table.changed_ms >= g_last_table.created_ms);
    be_w_event e = ev("aaaa", BE_EV_PICKUP, 1);
    be_world_event(&w, a, &e);
    T_EQ_INT(g_events, 1);
    T_EQ_INT(g_last_event.kind, BE_EV_PICKUP);
    T_EQ_INT(a->w.last_kind, BE_EV_PICKUP);
    T_EQ_INT(a->w.last_seat, 1);
    T_EQ_INT(a->ev_n, 1);
    // An event with no table still goes out, and changes no ring.
    be_world_event(&w, NULL, &e);
    T_EQ_INT(g_events, 2);
    // The ring keeps the newest BE_EVENTS, oldest first in the detail.
    for (int i = 0; i < BE_EVENTS + 5; i++) {
        be_w_event k = ev("aaaa", BE_EV_ATTACK, 0);
        k.version = (uint32_t)(100 + i);
        be_world_event(&w, a, &k);
    }
    T_EQ_INT(a->ev_n, BE_EVENTS);
    be_world_unlock(&w);
    be_w_detail *d = malloc(sizeof *d);
    T_CHECK(be_world_detail(&w, "aaaa", 4, d));
    T_EQ_INT(d->n_events, BE_EVENTS);
    T_EQ_INT(d->events[0].version, 100 + 5);                  // the five oldest fell off, and the pickup before them
    T_EQ_INT(d->events[BE_EVENTS - 1].version, 100 + BE_EVENTS + 4);
    T_EQ_INT(d->table.version, 7);
    T_CHECK(!be_world_detail(&w, "zzzz", 4, d));
    free(d);
    // No publisher: nothing crashes.
    w.publish = NULL;
    be_world_lock(&w);
    be_world_changed(&w, a);
    be_world_event(&w, a, &e);
    be_world_unlock(&w);
    be_world_free(&w);
}

static void list_and_prune(void) {
    be_world w;
    T_EQ_INT(be_world_init(&w, 8), 0);
    be_world_lock(&w);
    be_table *a = be_world_open(&w, "aaaa", 4);
    be_table *b = be_world_open(&w, "bbbb", 4);
    a->w.moves = 3;
    b->w.moves = 9;
    be_world_unlock(&w);
    void *buf = NULL;
    size_t n = be_world_list(&w, &buf);
    T_EQ_INT(n, sizeof(be_w_list) + 2 * sizeof(be_w_table));
    be_w_list head;
    memcpy(&head, buf, sizeof head);
    T_EQ_INT(head.hash, be_wire_hash());
    T_EQ_INT(head.n, 2);
    T_EQ_INT(head.first, sizeof(be_w_list));
    T_EQ_INT(head.size, sizeof(be_w_table));
    be_w_table rows[2];
    memcpy(rows, (uint8_t *)buf + head.first, sizeof rows);
    T_EQ_INT(rows[0].moves + rows[1].moves, 12);
    free(buf);
    // A GONE table stays BE_WORLD_KEEP_GONE_MS, then goes.
    be_world_lock(&w);
    be_world_set_phase(&w, a, BE_PHASE_GONE);
    be_world_prune(&w);
    T_EQ_INT(w.n_used, 2);
    a->gone_ms -= BE_WORLD_KEEP_GONE_MS + 1;
    be_world_prune(&w);
    T_EQ_INT(w.n_used, 1);
    T_CHECK(be_world_find(&w, "aaaa", 4) == NULL);
    T_CHECK(be_world_find(&w, "bbbb", 4) == b);
    be_world_unlock(&w);
    n = be_world_list(&w, &buf);
    memcpy(&head, buf, sizeof head);
    T_EQ_INT(head.n, 1);
    free(buf);
    be_world_free(&w);
}

int main(void) {
    T_RUN(tables_and_phases);
    T_RUN(changes_and_events_publish);
    T_RUN(list_and_prune);
    return T_DONE();
}
