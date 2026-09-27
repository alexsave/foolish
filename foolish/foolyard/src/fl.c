#include <stdio.h>
#include <stdlib.h>

#include "fl.h"

void fl_init(FL *fl, u32 type_size, u32 capacity) {
    fl->data = calloc(capacity, type_size);
    fl->live = calloc(capacity, sizeof(u8));
    fl->free_ids = malloc(capacity * sizeof(u32));
    if (!fl->data || !fl->live || !fl->free_ids) {
        fprintf(stderr, "fl: out of memory for %u x %u bytes\n", capacity, type_size);
        exit(1);
    }
    for (u32 i = 0; i < capacity; i++) fl->free_ids[i] = capacity - 1 - i;
    fl->n_free = capacity;
    fl->capacity = capacity;
    fl->type_size = type_size;
    fl->high_water = 0;
}

void fl_release(FL *fl, u32 id) {
    if (id >= fl->capacity || !fl->live[id]) {
        fprintf(stderr, "fl: slot %u released twice (or never held)\n", id);
        exit(1);
    }
    fl->live[id] = 0;
    fl->free_ids[fl->n_free++] = id;
}

void *fl_get(FL *fl, u32 id) {
    return fl->data + (size_t)id * fl->type_size;
}

u32 fl_alloc(FL *fl) {
    if (fl->n_free == 0) return FL_NONE;
    u32 id = fl->free_ids[--fl->n_free];
    fl->live[id] = 1;
    u32 in_flight = fl_in_flight(fl);
    if (in_flight > fl->high_water) fl->high_water = in_flight;
    return id;
}

void fl_free(FL *fl) {
    free(fl->data);
    free(fl->live);
    free(fl->free_ids);
}
