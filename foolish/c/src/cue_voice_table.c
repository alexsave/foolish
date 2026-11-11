// cue_voice_table.c - THE ONE VOICE a host plays from, over soundgen's table.
//
// cue_voice.c is the rules, handed a table; this is a host's one use of them:
// the table soundgen wrote for this build (build/gen/cue_takes.c, `soundgen
// ctable`, remade by c/Makefile whenever soundgen changes, from the same render
// as the files a host serves or bundles) and the one voice state that remembers
// what was played last. Both hosts that make sound link it beside that table and
// export it under their own prefix, one line each: iOS as fio_cue_*
// (c/ios/ios_api_sound.c), the web as wasm_cue_* (c/wasm/wasm_api.c). So the
// rules, the table and the state are the same C in both, and neither host keeps
// a copy of any of them.
//
// ONE VOICE STATE, like the one resident game: a page or an iMessage extension
// has one board, and a cue's "not the take it played last" is about what that
// board last sounded. It lives here, in the module's memory, not in the host:
// the host seeds it once (cue_table_seed) and otherwise only asks.

#include <string.h>
#include "cue_voice.h"

extern const CueTakeRow CUE_TAKE_ROWS[];
extern const int CUE_N_TAKE_ROWS;
extern const CueMix CUE_MIX_ROWS[ANIM_N_CUES];

static CueVoiceState g_voice;
static CueStrike g_strike;

// The table in use: the linked one, until the tuner hands one over (cue_table_load).
static CueTakeRow g_loaded[CUE_TABLE_MAX_ROWS];
static CueMix g_loaded_mix[ANIM_N_CUES];
static const CueTakeRow *g_rows = CUE_TAKE_ROWS;
static const CueMix *g_mix = CUE_MIX_ROWS;
static int g_n = -1;   // -1: CUE_N_TAKE_ROWS (an extern const, so not a constant initialiser)
static int n_rows(void) { return g_n < 0 ? CUE_N_TAKE_ROWS : g_n; }

int cue_table_n_takes(void) { return n_rows(); }

const CueTakeRow *cue_table_take(int i) {
    return i >= 0 && i < n_rows() ? &g_rows[i] : 0;
}

const CueMix *cue_table_mix(int kind) {
    return kind > ANIM_CUE_NONE && kind < ANIM_N_CUES ? &g_mix[kind] : 0;
}

int cue_table_lead_us(int kind) { return cue_lead_us(g_rows, n_rows(), kind); }
int cue_table_late_us(int kind) { return cue_late_us(g_rows, n_rows(), kind); }

int cue_table_load(const unsigned char *b, int len) {
    uint32_t magic;
    int32_t n;
    if (!b || len < 8) return -1;
    memcpy(&magic, b, 4);
    memcpy(&n, b + 4, 4);
    if (magic != CUE_TABLE_MAGIC || n < 0 || n > CUE_TABLE_MAX_ROWS
        || len != 8 + n * (int)sizeof(CueTakeRow) + ANIM_N_CUES * (int)sizeof(CueMix)) return -1;
    // Checked a row at a time (a whole table on the stack is 24 KB of a wasm stack).
    for (int i = 0; i < n; i++) {
        CueTakeRow row;
        memcpy(&row, b + 8 + (size_t)i * sizeof row, sizeof row);
        const CueTakeRow *r = &row;
        if (r->cue <= ANIM_CUE_NONE || r->cue >= ANIM_N_CUES || r->role > CUE_ROLE_PRELUDE || r->rate <= 0
            || r->n_samples <= 0) return -1;
        // The name ends inside its field, where its length says: found by looking in
        // the field, never by reading at an index the bytes gave.
        int len = 0;
        while (len < CUE_NAME_MAX && r->name[len]) len++;
        if (len == CUE_NAME_MAX || len == 0 || len != r->name_len) return -1;
    }
    memcpy(g_loaded, b + 8, (size_t)n * sizeof(CueTakeRow));
    memcpy(g_loaded_mix, b + 8 + (size_t)n * sizeof(CueTakeRow), sizeof g_loaded_mix);
    g_rows = g_loaded;
    g_mix = g_loaded_mix;
    g_n = n;
    // What was played last indexes the old rows.
    for (int i = 0; i < 2 * ANIM_N_CUES; i++) g_voice.last[i] = 0;
    return 0;
}

void cue_table_seed(uint32_t seed) { g_voice = (CueVoiceState){ .rng = seed }; }

const CueStrike *cue_table_strike(int kind, int lead_us, int n_cards, int undoes, int gain_pm) {
    cue_strike(g_rows, n_rows(), g_mix, &g_voice, kind, lead_us, n_cards, undoes,
               gain_pm, &g_strike);
    return &g_strike;
}

const CueStrike *cue_table_strike_opening(int kind, int lead_us, int n_cards, int undoes, int gain_pm) {
    cue_strike_opening(g_rows, n_rows(), g_mix, &g_voice, kind, lead_us, n_cards, undoes,
                       gain_pm, &g_strike);
    return &g_strike;
}
