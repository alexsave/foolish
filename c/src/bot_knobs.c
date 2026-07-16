// See bot_knobs.h. Self-contained on purpose: the wasm32 freestanding shim
// (c/wasm/include/) provides only getenv/atoi/strcmp — no strlen, no
// strncmp, no strcpy — so the scanning below is hand-rolled pointer work that
// compiles identically for native and wasm.

#include "bot_knobs.h"

#include <stdlib.h>

// The roster spec in force for the current bot_roster_choose call.
// _Thread_local to match the strategies' own flag caches (cordite_strategy.c
// et al.): the arena runs seats on several threads, and a knob installed for
// one seat's decision must never leak into another's.
static _Thread_local const char *g_spec = 0;

void bot_knobs_set(const char *spec) { g_spec = (spec && spec[0]) ? spec : 0; }
void bot_knobs_clear(void) { g_spec = 0; }

#define KNOB_RING 4
#define KNOB_VAL  32
static _Thread_local char g_ring[KNOB_RING][KNOB_VAL];
static _Thread_local int  g_ring_at = 0;

// Scan `spec` ("K=V,K2=V2") for `name`. On a hit, copy the value into the ring
// and return it; NULL otherwise. Key match is exact and terminated by '=' —
// so CD_RACE does not match CD_RACE_C, in either order.
static const char *spec_find(const char *spec, const char *name) {
    for (const char *p = spec; *p;) {
        // Compare the key at p against name.
        const char *k = name;
        while (*p && *p == *k && *p != '=' && *p != ',') { p++; k++; }
        int hit = (*k == 0 && *p == '=');
        // Walk to the value (skip the rest of a non-matching key).
        while (*p && *p != '=' && *p != ',') p++;
        if (*p == '=') p++;
        if (hit) {
            char *out = g_ring[g_ring_at];
            g_ring_at = (g_ring_at + 1) % KNOB_RING;
            int n = 0;
            while (*p && *p != ',' && n < KNOB_VAL - 1) out[n++] = *p++;
            out[n] = 0;
            return out;
        }
        while (*p && *p != ',') p++;   // rest of this value
        if (*p == ',') p++;
    }
    return 0;
}

const char *bot_knob(const char *name) {
    const char *v = getenv(name);          // research override wins
    if (v && v[0]) return v;
    return g_spec ? spec_find(g_spec, name) : 0;
}

int bot_knob_int(const char *name, int def) {
    const char *v = bot_knob(name);
    return (v && v[0]) ? atoi(v) : def;
}

int bot_knob_flag(const char *name) {
    const char *v = bot_knob(name);
    return v && v[0] && v[0] != '0';
}
