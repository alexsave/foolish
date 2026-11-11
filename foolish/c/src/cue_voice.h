// cue_voice.h - which take a sound cue plays, and when its file starts.
//
// The kernel decides WHICH cue a beat makes and WHEN (anim_plan.h "sound
// cues"). This is the next layer down, the one a host's audio engine asks once
// a cue is due: given "cue K, its moment D from now, standing for N cards",
// which of the cue's takes to play, how far from now its file starts so that
// its ANCHOR (the cue's moment inside the file) falls on the moment, whether
// the strokes before a sweep or a pickup (a PRELUDE) go with it, and at what
// level and speed. docs/AUDIO.md is the guide; the rules are its "How the web
// plays one":
//
//   A TAKE IS NEVER CUT. A take whose anchor is longer than the time left
//   before its moment starts now, whole, and late. A cue with TIGHT takes (the
//   gesture pressed closer to its moment) plays one of those when no ordinary
//   take fits, so it is late by as little as it can be.
//   PRELUDES: one stroke per `cards_per_level` cards, 1 at least, as many as
//   the cue has levels; started their own anchor before the same moment, over
//   whatever is sounding, from now when there is no time.
//   A REVERT is the undone cue's ordinary take played backwards, its anchor
//   measured from the file's other end, with no prelude.
//   VARIANTS: a cue never plays the take it played last, when it has others.
//   THE MIX: a cue's gain, how far its level and pitch may wander either way,
//   and how much louder it is a card past the first (CueMix, from soundgen).
//
// The TAKE TABLE and the MIX are soundgen's (tools/soundgen writes them, as
// build/gen/cue_takes.c, from the render that writes the files), never written
// by hand. The rules hold no table of their own: they are handed one, by the
// resident voice below or by a test.
//
// EVERY HOST ASKS THIS. iOS through fio_cue_* (c/ios/include/ios_api.h), the
// web through wasm_cue_* (c/wasm/wasm_api.c, sdk/ts/wasm/bots.ts), both one line
// over the resident voice below; a host only loads files and starts them where
// it is told. The web's engine made these decisions in TypeScript first, and
// they were recorded before it was deleted: tests/cue_voice_parity.h holds this
// file to every one (anim_plan_test test_cue_voice_matches_the_web).

#ifndef CUE_VOICE_H
#define CUE_VOICE_H

#include <stdint.h>
#include "anim_plan.h"

#define CUE_ROLE_MAIN    0
#define CUE_ROLE_TIGHT   1
#define CUE_ROLE_PRELUDE 2

// A take's file stem, without ".m4a": "deal_0", "sweep_strokes_s3_1".
#define CUE_NAME_MAX 32

// One file soundgen writes.
typedef struct {
    int32_t rate;          // Hz, the file's own
    int32_t n_samples;     // its length, to the sample (the m4a's edit list says so too)
    int32_t anchor_us;     // from its first sample to its cue's moment; negative
                           //   for a file that starts after the moment (the pass's slide)
    uint8_t cue;           // ANIM_CUE_*
    uint8_t role;          // CUE_ROLE_*
    uint8_t level;         // a prelude's stroke count, 1..5; 0 otherwise
    uint8_t name_len;
    char    name[CUE_NAME_MAX];
} CueTakeRow;

// How a cue sits in the mix (soundgen's CUE_MIX). Thousandths, so the table is
// exact in every host.
typedef struct {
    int32_t gain_pm;           // 1000 = the level the files were written at
    int32_t pitch_pm;          // how far a play's speed may wander either way
    int32_t level_pm;          // how far its level may wander either way
    int32_t per_card_pm;       // louder by this a card past the first, to +600
    int32_t cards_per_level_pm;// a prelude's cards per stroke (2500); 0 for none
} CueMix;

// What a host remembers between strikes: the take each cue played last, and
// the prelude, so neither is played twice running. Zeroed is the start.
typedef struct {
    int16_t last[2 * ANIM_N_CUES];   // take index + 1; [cue] main, [ANIM_N_CUES + cue] prelude
    uint64_t rng;               // shared/c/rng.h's splitmix64 state; any value, 0 included, is a good one
} CueVoiceState;

// The answer for one strike. Times are microseconds from NOW, never negative.
typedef struct {
    int32_t take;             // index into the table; -1 when the strike is silent
    int32_t start_us;         // when its file starts
    int32_t gain_pm;          // its level, the strike's own gain included (1000 = as written)
    int32_t rate_pm;          // its speed (1000 = as written)
    int32_t prelude;          // the strokes before it: an index, or -1
    int32_t prelude_start_us;
    int32_t prelude_gain_pm;
    int32_t prelude_rate_pm;
    int32_t backwards;        // 1: play `take` reversed (a REVERT)
} CueStrike;

// Strike `kind` so that its moment is `lead_us` from now (negative is taken as
// now), standing for `n_cards`; `undoes` is a REVERT's undone cue, `gain_pm`
// the strike's own level (a dealt card's seat, anim_seat_gain_pm). Fills `out`
// and answers 0, or -1 with out->take = -1 for a strike with nothing to play
// (no takes, a revert of nothing, a bad argument).
int cue_strike(const CueTakeRow *takes, int n_takes, const CueMix *mix,
               CueVoiceState *st, int kind, int lead_us, int n_cards, int undoes,
               int gain_pm, CueStrike *out);

// The same for a strike that OPENS ITS RUN (the first card of a deal): the cue's tight takes
// play even where a longer one would fit, started so their anchor is on the moment all the same.
// A cue with no tight take is struck as cue_strike does.
int cue_strike_opening(const CueTakeRow *takes, int n_takes, const CueMix *mix,
                       CueVoiceState *st, int kind, int lead_us, int n_cards, int undoes,
                       int gain_pm, CueStrike *out);

// How long before its moment a cue's earliest take starts (its longest anchor,
// preludes included), in microseconds: when a host must hand the cue over.
int cue_lead_us(const CueTakeRow *takes, int n_takes, int kind);

// How long AFTER its moment a cue is first heard (a take whose first sound
// comes later than the moment has a negative anchor), in microseconds, 0 for a
// cue heard at its moment or before. What is drawn to go with the sound (the
// shield a transfer hands on, with its "shfff") waits this long.
int cue_late_us(const CueTakeRow *takes, int n_takes, int kind);

// A take played at `rate_pm` (a stand-in's wander, CueStrike.rate_pm): the
// samples a host's player starts in place of `in`, by linear interpolation, so
// a speed is a buffer and not a second resampler in every host. Answers the
// length written (cue_resampled_len), or -1 when `cap` cannot hold it or an
// argument is bad. At 1000 it is a copy.
int cue_resampled_len(int n, int rate_pm);
int cue_resample(const float *in, int n, int rate_pm, float *out, int cap);

// ---- THE RESIDENT VOICE (cue_voice_table.c) -----------------------------------
// The above over soundgen's table for this build (build/gen/cue_takes.c) and
// one voice state in static storage, which is what a host exports: a take row,
// a cue's mix, its lead, the seed, and a strike, whose answer is good until the
// next one. A host that links this links the generated table with it; the
// tests hand cue_strike tables of their own instead.
int cue_table_n_takes(void);
const CueTakeRow *cue_table_take(int i);          // NULL out of range
const CueMix *cue_table_mix(int kind);             // NULL for NONE or out of range
int cue_table_lead_us(int kind);
int cue_table_late_us(int kind);
void cue_table_seed(uint32_t seed);
const CueStrike *cue_table_strike(int kind, int lead_us, int n_cards, int undoes, int gain_pm);
const CueStrike *cue_table_strike_opening(int kind, int lead_us, int n_cards, int undoes, int gain_pm);

// THE TUNER'S TABLE (development: tools/soundgen/tune.sh, src/audio/tunerPlay.ts).
// A table in place of the linked one, so a page hears a tuning change without the
// kernel being relinked and the page reloaded: soundgen writes it beside the C table
// as bytes, CUE_TABLE_MAGIC, an int32 row count, the rows and then ANIM_N_CUES mix
// rows, each exactly as the structs above lie in memory (the same on every host:
// no pointers, nothing wider than 32 bits, and the sizes asserted below). Answers 0,
// or -1 for bytes that are not such a table, which leaves the table as it was.
#define CUE_TABLE_MAGIC   0x54455543u   /* "CUET", little-endian */
#define CUE_TABLE_MAX_ROWS 512
int cue_table_load(const unsigned char *bytes, int len);
_Static_assert(sizeof(CueTakeRow) == 48, "CueTakeRow's bytes are the tuner table's layout");
_Static_assert(sizeof(CueMix) == 20, "CueMix's bytes are the tuner table's layout");

#endif
