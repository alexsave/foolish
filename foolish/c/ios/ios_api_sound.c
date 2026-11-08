// ios_api_sound.c - the sound half of the bridge: what a cue plays (cue_voice.h
// over soundgen's generated table) and how loud a dealt card lands at a seat.
//
// THE TABLE IS GENERATED. build/gen/cue_takes.c is soundgen's (`soundgen
// ctable`, run by this Makefile whenever soundgen changes), written from the
// same render as the web's src/audio/gen/cueTakes.ts and the files iOS bundles
// (ios/scripts/bundle_sounds.sh), so the anchors and levels here are the ones
// the files were written with. Nothing in Swift restates any of it.
//
// ONE VOICE STATE, like the one resident game: an iMessage extension has one
// board, and a cue's "not the take it played last" is about what that board
// last sounded.

#include "ios_api.h"
#include "cue_voice.h"

extern const CueTakeRow CUE_TAKE_ROWS[];
extern const int CUE_N_TAKE_ROWS;
extern const CueMix CUE_MIX_ROWS[ANIM_N_CUES];

static CueVoiceState g_voice;
static CueStrike g_strike;

int fio_seat_gain_pm(int viewer, int seat, int n_seats) {
    return anim_seat_gain_pm(viewer, seat, n_seats);
}

int fio_cue_n_takes(void) { return CUE_N_TAKE_ROWS; }

const void *fio_cue_take(int i) {
    return i >= 0 && i < CUE_N_TAKE_ROWS ? &CUE_TAKE_ROWS[i] : 0;
}

int fio_cue_lead_us(int kind) { return cue_lead_us(CUE_TAKE_ROWS, CUE_N_TAKE_ROWS, kind); }

void fio_cue_seed(uint32_t seed) {
    g_voice = (CueVoiceState){ .rng = seed };
}

const void *fio_cue_strike(int kind, int lead_us, int n_cards, int undoes, int gain_pm) {
    cue_strike(CUE_TAKE_ROWS, CUE_N_TAKE_ROWS, CUE_MIX_ROWS, &g_voice, kind, lead_us, n_cards, undoes,
               gain_pm, &g_strike);
    return &g_strike;
}
