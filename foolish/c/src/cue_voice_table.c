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

#include "cue_voice.h"

extern const CueTakeRow CUE_TAKE_ROWS[];
extern const int CUE_N_TAKE_ROWS;
extern const CueMix CUE_MIX_ROWS[ANIM_N_CUES];

static CueVoiceState g_voice;
static CueStrike g_strike;

int cue_table_n_takes(void) { return CUE_N_TAKE_ROWS; }

const CueTakeRow *cue_table_take(int i) {
    return i >= 0 && i < CUE_N_TAKE_ROWS ? &CUE_TAKE_ROWS[i] : 0;
}

const CueMix *cue_table_mix(int kind) {
    return kind > ANIM_CUE_NONE && kind < ANIM_N_CUES ? &CUE_MIX_ROWS[kind] : 0;
}

int cue_table_lead_us(int kind) { return cue_lead_us(CUE_TAKE_ROWS, CUE_N_TAKE_ROWS, kind); }

void cue_table_seed(uint32_t seed) { g_voice = (CueVoiceState){ .rng = seed }; }

const CueStrike *cue_table_strike(int kind, int lead_us, int n_cards, int undoes, int gain_pm) {
    cue_strike(CUE_TAKE_ROWS, CUE_N_TAKE_ROWS, CUE_MIX_ROWS, &g_voice, kind, lead_us, n_cards, undoes,
               gain_pm, &g_strike);
    return &g_strike;
}
