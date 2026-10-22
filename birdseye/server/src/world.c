// world.c - world.h.
#include "world.h"

#include "util/clock.h"

#include <stdlib.h>
#include <string.h>

be_world be_world_g;

int be_world_init(be_world *w, int cap) {
    memset(w, 0, sizeof *w);
    w->tables = calloc((size_t)cap, sizeof *w->tables);
    if (!w->tables) return -1;
    w->cap = cap;
    pthread_mutex_init(&w->mu, NULL);
    return 0;
}

void be_world_free(be_world *w) {
    free(w->tables);
    w->tables = NULL;
    w->cap = w->n_used = 0;
    pthread_mutex_destroy(&w->mu);
}

void be_world_lock(be_world *w) { pthread_mutex_lock(&w->mu); }
void be_world_unlock(be_world *w) { pthread_mutex_unlock(&w->mu); }

be_table *be_world_find(be_world *w, const char *gid, int gid_len) {
    if (gid_len <= 0 || gid_len > BE_GID) return NULL;
    for (int i = 0; i < w->cap; i++) {
        be_table *t = &w->tables[i];
        if (t->used && t->w.gid_len == gid_len && memcmp(t->w.gid, gid, (size_t)gid_len) == 0) return t;
    }
    return NULL;
}

static void count_phase(be_world *w, uint8_t phase, int d) {
    if (phase == BE_PHASE_LOBBY) w->lobby += (uint32_t)d;
    else if (phase == BE_PHASE_PLAYING) w->playing += (uint32_t)d;
    if (phase != BE_PHASE_GONE) w->live += (uint32_t)d;
}

be_table *be_world_open(be_world *w, const char *gid, int gid_len) {
    int64_t now_ms = cb_now_ms();
    if (gid_len <= 0 || gid_len > BE_GID || be_world_find(w, gid, gid_len)) return NULL;
    for (int i = 0; i < w->cap; i++) {
        be_table *t = &w->tables[i];
        if (t->used) continue;
        memset(t, 0, sizeof *t);
        t->used = true;
        t->w.gid_len = (uint8_t)gid_len;
        memcpy(t->w.gid, gid, (size_t)gid_len);
        t->w.phase = BE_PHASE_LOBBY;
        t->w.power_suit = -1;
        t->w.first_attacker = t->w.defender = t->w.fool = t->w.last_seat = -1;
        t->w.created_ms = t->w.changed_ms = now_ms;
        w->n_used++;
        count_phase(w, BE_PHASE_LOBBY, 1);
        return t;
    }
    return NULL;
}

void be_world_set_phase(be_world *w, be_table *t, uint8_t phase) {
    if (t->w.phase == phase) return;
    int64_t now_ms = cb_now_ms();
    count_phase(w, t->w.phase, -1);
    count_phase(w, phase, 1);
    t->w.phase = phase;
    if (phase == BE_PHASE_OVER) t->w.finished_ms = now_ms;
    if (phase == BE_PHASE_GONE) t->gone_ms = now_ms;
    if (phase == BE_PHASE_LOBBY) t->w.finished_ms = 0;
}

void be_world_changed(be_world *w, be_table *t) {
    t->w.changed_ms = cb_now_ms();
    if (!w->publish) return;
    uint8_t frame[2 + sizeof t->w];
    frame[0] = BE_SF_V;
    frame[1] = BE_F_TABLE;
    memcpy(frame + 2, &t->w, sizeof t->w);
    w->publish(frame, sizeof frame);
}

void be_world_event(be_world *w, be_table *t, const be_w_event *e) {
    if (t) {
        int at = (t->ev_head + t->ev_n) % BE_EVENTS;
        t->events[at] = *e;
        if (t->ev_n < BE_EVENTS) t->ev_n++;
        else t->ev_head = (t->ev_head + 1) % BE_EVENTS;
        t->w.last_kind = e->kind;
        t->w.last_seat = e->seat;
    }
    if (!w->publish) return;
    uint8_t frame[2 + sizeof *e];
    frame[0] = BE_SF_V;
    frame[1] = BE_F_EVENT;
    memcpy(frame + 2, e, sizeof *e);
    w->publish(frame, sizeof frame);
}

void be_world_prune(be_world *w) {
    int64_t now_ms = cb_now_ms();
    for (int i = 0; i < w->cap; i++) {
        be_table *t = &w->tables[i];
        if (t->used && t->w.phase == BE_PHASE_GONE && now_ms - t->gone_ms > BE_WORLD_KEEP_GONE_MS) {
            t->used = false;
            w->n_used--;
        }
    }
}

size_t be_world_list(be_world *w, void **out) {
    be_world_lock(w);
    size_t cap = sizeof(be_w_list) + (size_t)w->n_used * sizeof(be_w_table);
    uint8_t *buf = malloc(cap ? cap : 1);
    if (!buf) {
        be_world_unlock(w);
        return 0;
    }
    be_w_list head = { be_wire_hash(), 0, (uint32_t)sizeof(be_w_list), (uint32_t)sizeof(be_w_table) };
    size_t at = sizeof head;
    for (int i = 0; i < w->cap && at + sizeof(be_w_table) <= cap; i++) {
        be_table *t = &w->tables[i];
        if (!t->used) continue;
        memcpy(buf + at, &t->w, sizeof t->w);
        at += sizeof t->w;
        head.n++;
    }
    be_world_unlock(w);
    memcpy(buf, &head, sizeof head);
    *out = buf;
    return at;
}

bool be_world_detail(be_world *w, const char *gid, int gid_len, be_w_detail *out) {
    be_world_lock(w);
    be_table *t = be_world_find(w, gid, gid_len);
    if (!t) {
        be_world_unlock(w);
        return false;
    }
    memset(out, 0, sizeof *out);
    out->table = t->w;
    memcpy(out->hands, t->hands, sizeof out->hands);
    out->n_events = (uint8_t)t->ev_n;
    for (int i = 0; i < t->ev_n; i++) out->events[i] = t->events[(t->ev_head + i) % BE_EVENTS];
    be_world_unlock(w);
    return true;
}
