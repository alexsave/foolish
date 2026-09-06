#ifndef FOOLYARD_FL_H
#define FOOLYARD_FL_H

#include "types.h"

#define FL_NONE 0xFFFFFFFFu

// Fixed-size slab + a stack of free ids, tiltyard's fl.c. Two departures:
// capacity never doubles (an id has to fit the event word's param field, so
// the ceiling is a design constant, not a growth problem), and `live` tracks
// occupancy so a double release aborts instead of handing one slot to two
// owners - the TODO at the top of tiltyard's fl.h.
typedef struct FL {
    u8  *data;
    u8  *live;
    u32 *free_ids;
    u32  n_free;
    u32  capacity;
    u32  type_size;
    u32  high_water;
} FL;

void  fl_init(FL *fl, u32 type_size, u32 capacity);
void  fl_free(FL *fl);
u32   fl_alloc(FL *fl);        // FL_NONE when the slab is full
void *fl_get(FL *fl, u32 id);
void  fl_release(FL *fl, u32 id);

static inline u32 fl_in_flight(const FL *fl) { return fl->capacity - fl->n_free; }

#endif
