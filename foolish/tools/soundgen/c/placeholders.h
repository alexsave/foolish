// placeholders.h - the synthesised stand-ins for the cues with no model yet.
#ifndef SOUNDGEN_PLACEHOLDERS_H
#define SOUNDGEN_PLACEHOLDERS_H

#include "anim_plan.h"

typedef struct Buf Buf;
typedef struct {
    int    cue;                                 // ANIM_CUE_*
    void (*make)(Buf *, int v, float k);
    int    rate;                                // Hz: the lowest that holds its highest partial
} Placeholder;
extern const Placeholder PLACEHOLDERS[];
extern const int N_PLACEHOLDERS;

// Render variant `v` of `variants`; *x and *n are valid until the next call.
int placeholder_render(void (*make)(Buf *, int, float), int rate, int cue, int v, int variants,
                       const float **x, int *n);

#endif
