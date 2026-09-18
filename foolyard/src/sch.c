#include <stdio.h>
#include <stdlib.h>

#include "sch.h"

void sch_init(SCH *sch) {
    sch->now_us = 0;
    sch->cursor = 0;
    sch->cursor_start = 0;
    for (u32 i = 0; i < BUCKETS; i++) pq_init(&sch->buckets[i]);

    sch->n_free_timers = SCH_LONG_TIMERS;
    for (u32 i = 0; i < SCH_LONG_TIMERS; i++) sch->free_timers[i] = SCH_LONG_TIMERS - 1 - i;
}

static void schedule_near(SCH *sch, u32 event, u64 delta_us) {
    u64 fire = sch->now_us + delta_us;
    u32 bucket = (u32)((fire >> P_BITS) & BUCKET_MASK);
    pq_push(&sch->buckets[bucket], ((u32)(fire & P_MASK) << E_BITS) | (event & E_MASK));
}

// Past the wheel's reach: park the real event in a timer slot and walk it in
// with EV_HOP events one horizon at a time. sch_pop consumes those itself, so
// no caller ever sees a hop.
static void schedule_far(SCH *sch, u32 event, u64 delta_us) {
    if (sch->n_free_timers == 0) {
        fprintf(stderr, "sch: out of long timers (%d in flight), raise SCH_LONG_TIMERS\n", SCH_LONG_TIMERS);
        exit(1);
    }
    u32 slot = sch->free_timers[--sch->n_free_timers];
    sch->timers[slot].fire_at_us = sch->now_us + delta_us;
    sch->timers[slot].event = event;
    schedule_near(sch, event_of(EV_HOP, slot), MAX_DELTA_US);
}

void sch_schedule(SCH *sch, u32 event, u64 delta_us) {
    if (delta_us > MAX_DELTA_US) schedule_far(sch, event, delta_us);
    else                         schedule_near(sch, event, delta_us);
}

u32 sch_pop(SCH *sch) {
    for (;;) {
        u32 spun = 0;
        while (pq_empty(&sch->buckets[sch->cursor])) {
            if (++spun > BUCKETS) return SCH_EMPTY;
            sch->cursor = (sch->cursor + 1) & BUCKET_MASK;
            sch->cursor_start += P_SPAN;
        }

        u32 packed = pq_pop(&sch->buckets[sch->cursor]);
        sch->now_us = sch->cursor_start + (packed >> E_BITS);

        u32 event = packed & E_MASK;
        if (event_type(event) != EV_HOP) return event;

        u32 slot = event_param(event);
        u64 fire_at = sch->timers[slot].fire_at_us;
        u64 remaining = fire_at > sch->now_us ? fire_at - sch->now_us : 0;
        if (remaining > MAX_DELTA_US) {
            schedule_near(sch, event, MAX_DELTA_US);
        } else {
            schedule_near(sch, sch->timers[slot].event, remaining);
            sch->free_timers[sch->n_free_timers++] = slot;
        }
    }
}

void sch_free(SCH *sch) {
    for (u32 i = 0; i < BUCKETS; i++) pq_free(&sch->buckets[i]);
}
