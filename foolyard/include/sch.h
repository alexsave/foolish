#ifndef FOOLYARD_SCH_H
#define FOOLYARD_SCH_H

#include "types.h"
#include "constants.h"
#include "pq.h"

#define SCH_LONG_TIMERS 1024
#define SCH_EMPTY 0xFFFFFFFFu

typedef struct LongTimer {
    u64 fire_at_us;
    u32 event;
} LongTimer;

typedef struct SCH {
    u64 now_us;
    u32 cursor;         // bucket being drained
    u64 cursor_start;   // absolute time at that bucket's first microsecond
    PQ  buckets[BUCKETS];

    LongTimer timers[SCH_LONG_TIMERS];
    u32       free_timers[SCH_LONG_TIMERS];
    u32       n_free_timers;
} SCH;

void sch_init(SCH *sch);
void sch_schedule(SCH *sch, u32 event, u64 delta_us);
u32  sch_pop(SCH *sch);    // SCH_EMPTY when nothing is left
void sch_free(SCH *sch);

static inline u64 sch_now_us(const SCH *sch) { return sch->now_us; }

#endif
