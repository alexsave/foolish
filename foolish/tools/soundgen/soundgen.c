// soundgen - every sound the game plays, synthesised at build time.
//
// Every sound cue the kernel's animation plan can make (c/src/anim_plan.h
// "sound cues") is heard as a file this writes: the card sounds from the models
// Alex approved by ear (c/models.c, ported from the Python prototypes in the
// model folders beside this file), and the five cues nobody has modelled yet
// (OUT, GOOD, HAND_OFF, FINISH, FOOL) from the first synthesised stand-ins
// (c/placeholders.c). docs/AUDIO.md is the guide.
//
// A FILE IS A WHOLE GESTURE, LINED UP ON ITS CUE. A card thrown in is a grab, a
// thumb slide, a flight and a landing; the kernel strikes LIFT as the card
// leaves and PLACE as it lands. So LIFT's file is the grab and the slide that
// come BEFORE the card leaves, and every file carries an ANCHOR: how far into
// the file its cue's moment is. The host starts the file that much early (or,
// for a negative anchor, late). soundgen owns the anchors and writes them, with
// the file list, into a generated table the web reads (src/audio/gen/cueTakes.ts).
// The kernel owns WHEN; this owns what the moment sounds like and how much of
// it comes before.
//
// TAKES, AND NEVER A TAIL. Every card cue has several takes, as the models vary
// every draw within the recordings' ranges, and a take is always heard WHOLE
// (Alex: "you should definitely play them ... and play them in full"). Where
// the plan leaves less time before a moment than a take's anchor (a move that
// opens its run strikes its lift at 0 ms), the host plays it late rather than
// cut: so the deal and the lift also have TIGHT takes, the grab and the thumb
// slide moved closer together and closer to the moment, which the host plays
// when no ordinary take fits, so that the late part is as small as it can be.
//
// PRELUDES. A sweep and a pickup are gathered in strokes first, one per ~2.5
// cards (disc_game.py). The strokes are a file of their own (a PRELUDE, one set
// per stroke count, 1-5) played before the lift's own take, in full, over
// whatever is sounding then (Alex: "just play the pickup over the other
// sounds"); when there is no time before the moment at all they start at once
// and the lift still lands on its own cue.
//
// THE LEVELS ARE THE MODELS'. Every part is placed at the level its *_game.py
// sets on the deal's scale (g = -34..-32 dBFS), and the whole set is lifted by
// one common gain, so the loudest take peaks near full scale (PART_DB, PEAK_DB). Balancing
// the cues against each other is Alex's, by ear, on /sounds (the gain column of
// src/audio/cueSounds.ts), and is not decided here.
//
// NOTHING IT WRITES IS COMMITTED: `npm run sounds` writes public/audio/gen/ and
// src/audio/gen/, both ignored, and the hooks that build the site run it.
//
// TIED TO THE KERNEL'S CUE LIST BY THE COMPILER: the table below is indexed by
// ANIM_CUE_* and sized by ANIM_N_CUES (the _Static_assert).
//
// DETERMINISTIC: every draw is shared/c/rng.h seeded per cue, level and take,
// and nothing reads a clock. Across machines the last bit can differ (libm).
//
//   soundgen write <dir> <ts>   every file into <dir>, the take table into <ts>
//   soundgen ctable <c>         the same table for a C host (iOS), alone
//   soundgen list               one line a file: name, cue id, rate, ms, bytes, anchor ms, role, level
//   soundgen stats              the same, with peak and RMS
//   soundgen parts <dir>        each model on its own at 48 kHz, 12 takes, named
//                               as its Python names them: what measure.py compares

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anim_plan.h"
#include "c/dsp.h"
#include "c/models.h"
#include "c/placeholders.h"
#include "c/m4a.h"

#define S_(sec) ((int)((sec) * SR))
static double db2(double db) { return pow(10, db / 20); }

// THE BALANCE BETWEEN THE PARTS: dB on top of the level each *_game.py sets
// (positive is louder), each model's own balance kept. From the levels study
// of 2026-11-07 (within-video ratios in Alex's recordings, the rest corrected
// for each video's distance to the phone; its scripts and table were
// scratchpad/levels/py/table.py), as Alex asked ("do it yourself"):
//   - a card LANDING is ~25 dB over the grab and slide before it in the same
//     video, where the games had it ~4 dB over: the throw-in landing +18 (also
//     the trump's and the pass's landing, the same model), the cover landing
//     +19, the trump flip +17, the deck set down +14, the discard's lift +18;
//   - the discard and pickup videos were shot about twice as far from the
//     phone as the pass video, so their parts' "6.5 dB under a pass" was the
//     distance: the strokes +7, the placement +8, the pickup's lift +8, the
//     slide into the hand +8;
//   - unchanged: the deal's thumb slide, grab and landing (the landing Alex set
//     by ear), the two-finger grab, the throw-in's slide and the pass's shfff.
enum { P_THUMB, P_GRAB, P_LAND, P_TGRAB, P_THROW, P_COVER, P_FLIP, P_DECK, P_PASS,
       P_STROKE, P_LIFT, P_PLACE, P_PLIFT, P_INSERT, N_PARTS };
static const double PART_DB[N_PARTS] = {
    [P_THUMB] = 0, [P_GRAB] = 0, [P_LAND] = 0, [P_TGRAB] = 0, [P_PASS] = 0,
    [P_THROW] = 18, [P_COVER] = 19, [P_FLIP] = 17, [P_DECK] = 14, [P_LIFT] = 18,
    [P_STROKE] = 7, [P_PLACE] = 8, [P_PLIFT] = 8, [P_INSERT] = 8,
};
// THE COMMON GAIN is not a constant: after every take is rendered, the whole
// card set is scaled by one gain so its loudest take peaks at PEAK_DB at full
// master volume, or lower if a mix of takes a plan really strikes together
// (MIXES, below) would then pass MIX_DB. One gain for every card sound, so the
// balance is the parts' own.
#define PEAK_DB -3.0
#define MIX_DB  -1.0
// Silence is trimmed off both ends of a take below this, re its peak.
#define TRIM_DB -60.0
// The most strokes a gathering has: 2.5 cards a stroke, and a table of twelve
// or more is five (disc_game.py).
#define MAX_LEVEL 5

// ---- a take being assembled -----------------------------------------------------
// A timeline at 48 kHz with the cue's moment at sample CUE0; parts are added at
// their offsets from it, and the take is trimmed and resampled at the end.
#define CUE0 S_(5.0)
#define SPAN S_(7.0)
typedef struct { Sig mix; Rng r; double g; } Take;
static void take_init(Take *t, uint64_t seed) { t->mix = sig_new(SPAN); rng_init(&t->r, seed); t->g = uni(&t->r, -34, -32); }
// Add `part` with its sample `ref` at `at_s` from the cue, its peak at `db` (the games' put()).
static void put_peak(Take *t, int part_id, Sig part, int ref, double at_s, double db) {
    const double k = db2(db + PART_DB[part_id]) / (sig_peak(part.x, part.n) + 1e-12);
    const int o = CUE0 + S_(at_s) - ref;
    for (int i = 0; i < part.n; i++) if (o + i >= 0 && o + i < SPAN) t->mix.x[o + i] += part.x[i] * k;
    sig_free(&part);
}
// ...or with its loudest 50 ms RMS (150 Hz-15 kHz) at `db` (disc_game.py's put()).
static void put_rms50(Take *t, int part_id, Sig part, int ref, double at_s, double db) {
    Sig b = bp_copy(&part, 150, 15000);
    const double m = max_moving_rms(&b, 2400, 0, b.n);
    sig_free(&b);
    const double k = db2(db + PART_DB[part_id]) / (m + 1e-12);
    const int o = CUE0 + S_(at_s) - ref;
    for (int i = 0; i < part.n; i++) if (o + i >= 0 && o + i < SPAN) t->mix.x[o + i] += part.x[i] * k;
    sig_free(&part);
}

// ---- the gestures: each *_game.py at the kernel's moments -------------------------
// DEAL (deal/an/deal_game.py): the thumb slide's peak 130 ms before the grab,
// the grab at the moment the card leaves the stock.
static void g_deal(Take *t, int level) {
    (void)level;
    Sig s = m_thumb(&t->r); put_peak(t, P_THUMB, s, sig_argmax_abs(&s), -0.13, t->g - uni(&t->r, 6, 9));
    Sig g = m_grab(&t->r); put_peak(t, P_GRAB, g, sig_argmax_abs(&g), 0, t->g - 5.0);
}
// ...and TIGHT, for a card dealt with no time before it: the slide's peak 50 ms
// before the grab (its swell still before that), so played late from the
// moment it is over a sixth of a second sooner.
static void g_deal_tight(Take *t, int level) {
    (void)level;
    Sig s = m_thumb(&t->r); put_peak(t, P_THUMB, s, sig_argmax_abs(&s), -0.05, t->g - uni(&t->r, 6, 9));
    Sig g = m_grab(&t->r); put_peak(t, P_GRAB, g, sig_argmax_abs(&g), 0, t->g - 5.0);
}
// DEAL_LAND: the card on the cloth, land229, quietest of the three. At the
// viewer's own seat; the host scales it by the seat (anim_seat_gain_pm).
static void g_deal_land(Take *t, int level) {
    (void)level;
    Sig l = m_land(&t->r); put_peak(t, P_LAND, l, sig_argmax_abs(&l), 0, t->g - uni(&t->r, 13, 16));
}
// TURN (flip/an/flip_game.py): the trump flipped, its snap as it leaves the stock.
static void g_turn(Take *t, int level) {
    (void)level;
    Sig f = m_flip(&t->r); put_peak(t, P_FLIP, f, S_(FLIP_REF_S), 0, t->g - 5.0);
}
// FLIP: the trump landing (a throw-in, Alex: "similar to a throw in"), then the
// stock set down on it 350 ms later ("350 ms after trump lands is fine"). Baked
// into one file because nothing on screen moves at the set-down: there is no
// moment for the kernel to name, only the sound's own rhythm.
static void g_flip_land(Take *t, int level) {
    (void)level;
    Sig l = m_throw(&t->r); put_peak(t, P_THROW, l, sig_argmax_abs(&l), 0, t->g - uni(&t->r, 9, 12));
    Sig d = m_deck(&t->r); put_peak(t, P_DECK, d, S_(DECK_REF_S), 0.35, t->g - uni(&t->r, 3, 6));
}
// LIFT (throwin/an/throwin_game.py): two fingers take the card, the thumb slides
// it out, it leaves: the grab 110 ms before the slide's peak, the slide's peak
// 130 ms before the card leaves. The same for an attack, a cover and a pass.
static void g_lift(Take *t, int level) {
    (void)level;
    Sig s = m_thumb(&t->r); put_peak(t, P_THUMB, s, sig_argmax_abs(&s), -0.13, t->g - uni(&t->r, 6, 9));
    Sig g = m_tgrab(&t->r); put_peak(t, P_TGRAB, g, S_(TGRAB_REF_S), -0.24, t->g - 14.0);
}
// ...and TIGHT (Alex: "maybe put them closer to each other"), for a move that
// opens its run and so has no time before its lift: the grab 60 ms before the
// slide's peak and the peak 30 ms before the card leaves, where they were 110
// and 130 ms. The host plays it from the moment, whole.
static void g_lift_tight(Take *t, int level) {
    (void)level;
    Sig s = m_thumb(&t->r); put_peak(t, P_THUMB, s, sig_argmax_abs(&s), -0.03, t->g - uni(&t->r, 6, 9));
    Sig g = m_tgrab(&t->r); put_peak(t, P_TGRAB, g, S_(TGRAB_REF_S), -0.09, t->g - 14.0);
}
// PLACE: the throw-in's landing.
static void g_place(Take *t, int level) {
    (void)level;
    Sig l = m_throw(&t->r); put_peak(t, P_THROW, l, sig_argmax_abs(&l), 0, t->g - uni(&t->r, 3, 5));
}
// COVER: the cover's landing, on a card.
static void g_cover(Take *t, int level) {
    (void)level;
    Sig l = m_cover(&t->r); put_peak(t, P_COVER, l, sig_argmax_abs(&l), 0, t->g - uni(&t->r, 4, 6));
}
// PASS (pass/an/pass_game.py): struck at the transfer's landing, beside its
// PLACE; the "shfff" starts 150 ms after it.
static void g_pass(Take *t, int level) {
    (void)level;
    Sig p = m_pass(&t->r); put_peak(t, P_PASS, p, S_(PASS_REF_S), 0.15, t->g - uni(&t->r, 11, 13));
}
// The strokes gathering the table (discard/an/disc_game.py), the last ending
// `gap` before the cue, at `sc` (their 50 ms RMS): each 0.8 times a recorded
// stroke's length (0.25-0.7 s), 60-200 ms apart, a little louder as the pile
// grows. Not compressed: they are heard in full, over whatever is sounding.
static void strokes(Take *t, int count, double gap, double sc) {
    double d[MAX_LEVEL], gp[MAX_LEVEL], total = 0;
    for (int s = 0; s < count; s++) {
        const double v = m_stroke_dur(&t->r) * 0.8 * uni(&t->r, 0.9, 1.1);
        d[s] = v < 0.25 ? 0.25 : v > 0.7 ? 0.7 : v;
        gp[s] = s < count - 1 ? uni(&t->r, 0.06, 0.2) : 0;
        total += d[s] + gp[s];
    }
    double at = -gap - total;                            // the first stroke's onset, from the cue
    for (int s = 0; s < count; s++) {
        Sig y = m_stroke(&t->r, d[s]);
        put_rms50(t, P_STROKE, y, S_(STROKE_PRE_S), at, sc - 1.5 + 3.0 * s / (count > 1 ? count - 1 : 1) - uni(&t->r, 0, 1.5));
        at += d[s] + gp[s];
    }
}
// SWEEP: the pile lifted and turned, as the sweep opens (its strokes are the
// prelude below). The levels are disc_game.py's: the strokes at g-30.5, the
// lift 5 dB over them.
static void g_sweep(Take *t, int level) {
    (void)level;
    Sig l = m_lift(&t->r); put_rms50(t, P_LIFT, l, S_(EVENT_PRE_S), 0, t->g - 30.5 + 5);
}
// ...its prelude: `level` strokes, the last 100-180 ms before the lift.
static void g_sweep_strokes(Take *t, int level) { strokes(t, level, uni(&t->r, 0.10, 0.18), t->g - 30.5); }
// PILE: the swept cards set softly on the discard pile, as they land.
static void g_pile(Take *t, int level) {
    (void)level;
    Sig p = m_place(&t->r); put_rms50(t, P_PLACE, p, S_(EVENT_PRE_S), 0, t->g - 30.5 + 3);
}
// PICKUP: the one soft lift, as the table lifts, 10 dB under the strokes.
static void g_pickup(Take *t, int level) {
    (void)level;
    Sig l = m_plift(&t->r); put_rms50(t, P_PLIFT, l, S_(EVENT_PRE_S), 0, t->g - 30.5 - 10);
}
// ...its prelude: `level` strokes, the last 50-120 ms before the lift.
static void g_pickup_strokes(Take *t, int level) { strokes(t, level, uni(&t->r, 0.05, 0.12), t->g - 30.5); }
// INTO_HAND: the cards slid into the fanned hand as they land, its onset there.
static void g_into_hand(Take *t, int level) {
    (void)level;
    Sig s = m_insert(&t->r); put_rms50(t, P_INSERT, s, S_(STROKE_PRE_S), 0, t->g - 30.5 - 11);
}

// ---- the table: what every cue is written as ------------------------------------
// `name` is the file stem and the kernel's own name for the cue in lower case.
// `rate`: 48 kHz where the deal's thumb slide is in the take (its recordings
// reach 20 kHz and a third of its energy is over 16 kHz), 32 kHz everywhere else
// (every other model cuts at 15 kHz, so 32 kHz holds all of it).
// `kbps` in each set is the AAC bitrate it ships at: the lowest that measured
// transparent against the WAV (aac/bitrate.py, docs/AUDIO.md "Shipped as AAC"),
// the highest over a prelude's levels.
// A cue's takes come in up to three SETS: its own takes (MAIN), the TIGHT ones
// played when none of those fits the time before the moment, and a PRELUDE in
// levels (the strokes before a sweep or a pickup, one set per stroke count).
// A recipe with no sets is a cue with no sound of its own (REVERT), or one the
// placeholders make.
enum { ROLE_MAIN, ROLE_TIGHT, ROLE_PRELUDE };
static const char *const ROLE_NAME[] = { "main", "tight", "prelude" };
typedef struct {
    void (*make)(Take *, int level);
    int    takes;      // per level
    int    lo, hi;     // levels, inclusive (0, 0 for a set with none)
    int    kbps;       // the AAC bitrate it ships at: the lowest that measured transparent (aac/bitrate.py)
} Set;
typedef struct {
    const char *name;
    int         rate;
    Set         main, tight, prelude;
    double      cards_per_level;
} Recipe;
#define ONE(f, n, k) { f, n, 0, 0, k }
#define NONE         { NULL, 0, 0, 0, 0 }
// The placeholders' bitrate: at 8 and 12 kHz ffmpeg's encoder gives no more
// than this whatever it is asked, and they measure within 0.8 dB at it.
#define PLACEHOLDER_KBPS 64

static const Recipe RECIPES[] = {
    [ANIM_CUE_NONE]      = { "none",      0,     NONE, NONE, NONE, 0 },
    [ANIM_CUE_DEAL]      = { "deal",      48000, ONE(g_deal, 8, 128), ONE(g_deal_tight, 4, 192), NONE, 0 },
    [ANIM_CUE_FLIP]      = { "flip",      32000, ONE(g_flip_land, 8, 96), NONE, NONE, 0 },
    [ANIM_CUE_LIFT]      = { "lift",      48000, ONE(g_lift, 8, 128), ONE(g_lift_tight, 4, 160), NONE, 0 },
    [ANIM_CUE_PLACE]     = { "place",     32000, ONE(g_place, 8, 96), NONE, NONE, 0 },
    [ANIM_CUE_COVER]     = { "cover",     32000, ONE(g_cover, 8, 96), NONE, NONE, 0 },
    [ANIM_CUE_PICKUP]    = { "pickup",    32000, ONE(g_pickup, 8, 128), NONE, { g_pickup_strokes, 3, 1, MAX_LEVEL, 160 }, 2.5 },
    [ANIM_CUE_SWEEP]     = { "sweep",     32000, ONE(g_sweep, 8, 128), NONE, { g_sweep_strokes, 3, 1, MAX_LEVEL, 128 }, 2.5 },
    [ANIM_CUE_PILE]      = { "pile",      32000, ONE(g_pile, 8, 96), NONE, NONE, 0 },
    [ANIM_CUE_OUT]       = { "out",       0,     NONE, NONE, NONE, 0 },
    [ANIM_CUE_REVERT]    = { "revert",    0,     NONE, NONE, NONE, 0 },
    [ANIM_CUE_GOOD]      = { "good",      0,     NONE, NONE, NONE, 0 },
    [ANIM_CUE_HAND_OFF]  = { "hand_off",  0,     NONE, NONE, NONE, 0 },
    [ANIM_CUE_FINISH]    = { "finish",    0,     NONE, NONE, NONE, 0 },
    [ANIM_CUE_FOOL]      = { "fool",      0,     NONE, NONE, NONE, 0 },
    [ANIM_CUE_DEAL_LAND] = { "deal_land", 32000, ONE(g_deal_land, 8, 96), NONE, NONE, 0 },
    [ANIM_CUE_TURN]      = { "turn",      32000, ONE(g_turn, 8, 128), NONE, NONE, 0 },
    [ANIM_CUE_PASS]      = { "pass",      32000, ONE(g_pass, 8, 128), NONE, NONE, 0 },
    [ANIM_CUE_INTO_HAND] = { "into_hand", 32000, ONE(g_into_hand, 8, 128), NONE, NONE, 0 },
};
_Static_assert(sizeof RECIPES / sizeof RECIPES[0] == ANIM_N_CUES,
               "every sound cue of anim_plan.h has a line in RECIPES");
static const char *const CUE_CONST[ANIM_N_CUES] = {
    [ANIM_CUE_DEAL] = "ANIM_CUE_DEAL", [ANIM_CUE_FLIP] = "ANIM_CUE_FLIP", [ANIM_CUE_LIFT] = "ANIM_CUE_LIFT",
    [ANIM_CUE_PLACE] = "ANIM_CUE_PLACE", [ANIM_CUE_COVER] = "ANIM_CUE_COVER", [ANIM_CUE_PICKUP] = "ANIM_CUE_PICKUP",
    [ANIM_CUE_SWEEP] = "ANIM_CUE_SWEEP", [ANIM_CUE_PILE] = "ANIM_CUE_PILE", [ANIM_CUE_OUT] = "ANIM_CUE_OUT",
    [ANIM_CUE_GOOD] = "ANIM_CUE_GOOD", [ANIM_CUE_HAND_OFF] = "ANIM_CUE_HAND_OFF", [ANIM_CUE_FINISH] = "ANIM_CUE_FINISH",
    [ANIM_CUE_FOOL] = "ANIM_CUE_FOOL", [ANIM_CUE_DEAL_LAND] = "ANIM_CUE_DEAL_LAND", [ANIM_CUE_TURN] = "ANIM_CUE_TURN",
    [ANIM_CUE_PASS] = "ANIM_CUE_PASS", [ANIM_CUE_INTO_HAND] = "ANIM_CUE_INTO_HAND",
};

// ---- the mix: how loud each cue sits against the others ----------------------------
// THE ONE PLACE A CUE'S LEVEL IS SET, for every host: soundgen writes it into the
// web's take table (CUE_MIX in src/audio/gen/cueTakes.ts, which cueSounds.ts reads)
// and into iOS's (c/build/gen/cue_takes.c, which c/src/cue_voice.c reads), so the
// two can never be mixed differently. Thousandths, so both read the same numbers.
//   gain      against the level the files were written at (1000 for every model:
//             PART_DB and the common gain already set the card sounds' balance)
//   pitch     how far a play's speed may wander either way (60 is about a semitone);
//             0 for every model, whose takes already vary as the recordings did
//   level     how far its level may wander either way
//   per_card  louder by this for each card past the first, up to +600; 0 when the
//             takes already say how many cards it is (a sweep's strokes)
// THE PLACEHOLDERS, set against the modelled card set (Alex 2026-11-07: "turn
// them down"; they sat 10-20 dB over most card sounds). Targets by loudest 50 ms
// RMS (`make stats` times the gain), with a card landing at about -22 dBFS and a
// deal at about -40: the game over -26 (once a game, just under a landing), a
// seat going out -30, a good -34 and the hand-off -38 (heard every bout, so
// under the good). The hand-off is the one stand-in that wanders (a row of one
// sample is a row of cards and not a machine); the notes keep their pitch.
typedef struct { int gain, pitch, level, per_card; } MixRow;
#define MODEL { 1000, 0, 0, 0 }
static const MixRow MIX_ROWS[] = {
    [ANIM_CUE_NONE]      = { 0, 0, 0, 0 },
    [ANIM_CUE_DEAL]      = MODEL, [ANIM_CUE_FLIP]   = MODEL, [ANIM_CUE_LIFT]      = MODEL,
    [ANIM_CUE_PLACE]     = MODEL, [ANIM_CUE_COVER]  = MODEL, [ANIM_CUE_PICKUP]    = MODEL,
    [ANIM_CUE_SWEEP]     = MODEL, [ANIM_CUE_PILE]   = MODEL,
    [ANIM_CUE_OUT]       = { 68, 0, 0, 0 },        // -6.6 dBFS raw -> -30
    [ANIM_CUE_REVERT]    = { 0, 0, 0, 0 },         // the undone cue's row, backwards
    [ANIM_CUE_GOOD]      = { 44, 0, 0, 0 },        // -6.8 -> -34
    [ANIM_CUE_HAND_OFF]  = { 47, 60, 180, 0 },     // -11.4 -> -38
    [ANIM_CUE_FINISH]    = { 100, 0, 0, 0 },       // -5.9 -> -26
    [ANIM_CUE_FOOL]      = { 95, 0, 0, 0 },        // -5.6 -> -26
    [ANIM_CUE_DEAL_LAND] = MODEL, [ANIM_CUE_TURN]   = MODEL, [ANIM_CUE_PASS]      = MODEL,
    [ANIM_CUE_INTO_HAND] = MODEL,
};
_Static_assert(sizeof MIX_ROWS / sizeof MIX_ROWS[0] == ANIM_N_CUES, "every sound cue has a line in MIX_ROWS");

// ---- one rendered file -------------------------------------------------------------
typedef struct {
    char   name[48];
    int    cue, level, role, rate, n, kbps;
    double anchor_ms;      // from the file's first sample to the cue's moment
    double *x;             // the take before the common gain (a card take; NULL for a placeholder)
    int16_t *pcm;
} Out;

static int16_t to16(double v) { v *= 32767; if (v > 32767) v = 32767; if (v < -32768) v = -32768; return (int16_t)lrint(v); }

// Trim, fade the ends, resample to the shipping rate.
static void finish_take(Take *t, int rate, Out *o) {
    const double pk = sig_peak(t->mix.x, t->mix.n), thr = pk * db2(TRIM_DB);
    int a = 0, b = t->mix.n - 1;
    while (a < b && fabs(t->mix.x[a]) < thr) a++;
    while (b > a && fabs(t->mix.x[b]) < thr) b--;
    a -= S_(0.002); if (a < 0) a = 0;
    b += S_(0.004); if (b >= t->mix.n) b = t->mix.n - 1;
    Sig cut = sig_new(b - a + 1);
    memcpy(cut.x, t->mix.x + a, sizeof(double) * (size_t)cut.n);
    const int fi = S_(0.001), fo = S_(0.004);
    for (int i = 0; i < fi && i < cut.n; i++) cut.x[i] *= (double)i / fi;
    for (int i = 0; i < fo && i < cut.n; i++) cut.x[cut.n - 1 - i] *= (double)i / fo;
    Sig sh = rate == SR ? cut : resample_3_2(&cut);
    o->rate = rate; o->n = sh.n;
    o->anchor_ms = (double)(CUE0 - a) * 1000.0 / SR;
    o->x = malloc(sizeof(double) * (size_t)sh.n);
    memcpy(o->x, sh.x, sizeof(double) * (size_t)sh.n);
    if (sh.x != cut.x) sig_free(&sh);
    sig_free(&cut);
}

static void render_card(int cue, int role, const Set *set, int level, int v, Out *o) {
    const Recipe *r = &RECIPES[cue];
    Take t;
    take_init(&t, (uint64_t)cue * 1000003u + (uint64_t)role * 50021u + (uint64_t)level * 7919u + (uint64_t)v * 104729u + 17u);
    set->make(&t, level);
    finish_take(&t, r->rate, o);
    sig_free(&t.mix);
    if (role == ROLE_PRELUDE) snprintf(o->name, sizeof o->name, "%s_strokes_s%d_%d", r->name, level, v);
    else if (role == ROLE_TIGHT) snprintf(o->name, sizeof o->name, "%s_tight_%d", r->name, v);
    else snprintf(o->name, sizeof o->name, "%s_%d", r->name, v);
    o->cue = cue; o->level = level; o->role = role; o->kbps = set->kbps;
}

static int render_placeholder(int cue, int v, Out *o) {
    for (int i = 0; i < N_PLACEHOLDERS; i++) {
        if (PLACEHOLDERS[i].cue != cue) continue;
        const float *x; int n;
        placeholder_render(PLACEHOLDERS[i].make, PLACEHOLDERS[i].rate, cue, v, 1, &x, &n);
        o->pcm = malloc(sizeof(int16_t) * (size_t)n);
        for (int k = 0; k < n; k++) o->pcm[k] = to16(x[k]);
        o->n = n; o->rate = PLACEHOLDERS[i].rate; o->anchor_ms = 0; o->cue = cue; o->level = 0; o->role = ROLE_MAIN;
        o->kbps = PLACEHOLDER_KBPS;
        snprintf(o->name, sizeof o->name, "%s_%d", RECIPES[cue].name, v);
        return 1;
    }
    return 0;
}

// ---- WAV: 16-bit PCM, mono, the 44-byte header and nothing else ------------
static void le(FILE *f, uint32_t v, int bytes) { for (int i = 0; i < bytes; i++) fputc((int)((v >> (8 * i)) & 0xff), f); }
static int write_pcm(const char *path, const int16_t *x, int n, int rate) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    const uint32_t data = (uint32_t)n * 2;
    fwrite("RIFF", 1, 4, f); le(f, 36 + data, 4); fwrite("WAVEfmt ", 1, 8, f);
    le(f, 16, 4); le(f, 1, 2); le(f, 1, 2); le(f, (uint32_t)rate, 4);
    le(f, (uint32_t)rate * 2, 4); le(f, 2, 2); le(f, 16, 2);
    fwrite("data", 1, 4, f); le(f, data, 4);
    for (int i = 0; i < n; i++) le(f, (uint32_t)(uint16_t)x[i], 2);
    return fclose(f);
}

// ---- parts: each model alone, for measure.py ---------------------------------------
static int write_sig(const char *dir, const char *name, int v, Sig s) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s_%02d.wav", dir, name, v);
    int16_t *p = malloc(sizeof(int16_t) * (size_t)s.n);
    for (int i = 0; i < s.n; i++) p[i] = to16(s.x[i]);
    const int rc = write_pcm(path, p, s.n, SR);
    free(p); sig_free(&s);
    return rc;
}
// disc_syn.py writes its parts at these peaks (150 Hz-15 kHz); the others come out of their models at their own.
static Sig at_peak(Sig s, double db) {
    Sig b = bp_copy(&s, 150, 15000);
    sig_scale(&s, db2(db) / (sig_peak(b.x, b.n) + 1e-12));
    sig_free(&b);
    return s;
}
static int parts(const char *dir) {
    for (int v = 1; v <= 12; v++) {
        Rng r; rng_init(&r, 4242u + (uint64_t)v);
        if (write_sig(dir, "thumb9", v, m_thumb(&r)) || write_sig(dir, "grab2", v, m_grab(&r))
            || write_sig(dir, "land229", v, m_land(&r)) || write_sig(dir, "throw9", v, m_throw(&r))
            || write_sig(dir, "cover3", v, m_cover(&r)) || write_sig(dir, "tgrab2", v, m_tgrab(&r))
            || write_sig(dir, "flipE", v, m_flip(&r)) || write_sig(dir, "deck229c", v, m_deck(&r))
            || write_sig(dir, "pass7", v, m_pass(&r)) || write_sig(dir, "scrape", v, at_peak(m_stroke(&r, 0), -36))
            || write_sig(dir, "lift", v, at_peak(m_lift(&r), -22)) || write_sig(dir, "place", v, at_peak(m_place(&r), -31))
            || write_sig(dir, "insert", v, at_peak(m_insert(&r), -40)) || write_sig(dir, "plift", v, at_peak(m_plift(&r), -40))) {
            fprintf(stderr, "soundgen: cannot write into %s\n", dir); return 1;
        }
    }
    return 0;
}

// ---- the take table the web reads ----------------------------------------------------
static int write_table(const char *path, Out *outs, int n) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "// GENERATED by tools/soundgen (npm run sounds) - do not edit, and not committed.\n"
               "// Every file soundgen writes into public/audio/gen/, by cue: its path under /audio/,\n"
               "// its ANCHOR (ms from the file's first sample to the cue's moment; the host starts\n"
               "// it that early, or late for a negative one), its ROLE (main; tight, played when no\n"
               "// main take fits the time before the moment; prelude, the strokes before a sweep or\n"
               "// a pickup) and its LEVEL (a prelude's stroke count). tools/soundgen/soundgen.c says why.\n\n"
               "import {\n");
    for (int c = 1; c < ANIM_N_CUES; c++) if (CUE_CONST[c]) fprintf(f, "    %s,\n", CUE_CONST[c]);
    fprintf(f, "} from '@sdk/ts/gen/kernel_consts.bots.ts';\n\n"
               "export type CueTakeRole = 'main' | 'tight' | 'prelude';\n"
               "export interface CueTake { readonly file: string; readonly anchorMs: number; readonly role: CueTakeRole; readonly level: number }\n\n"
               "export const CUE_TAKES: Readonly<Record<number, readonly CueTake[]>> = {\n");
    double lead = 0;
    for (int c = 1; c < ANIM_N_CUES; c++) {
        if (!CUE_CONST[c]) continue;
        int any = 0;
        for (int i = 0; i < n; i++) any |= outs[i].cue == c;
        if (!any) continue;
        fprintf(f, "    [%s]: [\n", CUE_CONST[c]);
        for (int i = 0; i < n; i++) {
            if (outs[i].cue != c) continue;
            fprintf(f, "        { file: 'gen/%s.m4a', anchorMs: %.1f, role: '%s', level: %d },\n", outs[i].name, outs[i].anchor_ms,
                    ROLE_NAME[outs[i].role], outs[i].level);
            if (outs[i].anchor_ms > lead) lead = outs[i].anchor_ms;
        }
        fprintf(f, "    ],\n");
    }
    fprintf(f, "};\n\n/** How many cards one level (one stroke) of a levelled cue gathers. */\n"
               "export const CUE_CARDS_PER_LEVEL: Readonly<Record<number, number>> = {\n");
    for (int c = 1; c < ANIM_N_CUES; c++)
        if (RECIPES[c].prelude.make) fprintf(f, "    [%s]: %.1f,\n", CUE_CONST[c], RECIPES[c].cards_per_level);
    fprintf(f, "};\n\n/** How each cue sits in the mix (soundgen.c MIX_ROWS): its gain against the level its\n"
               " *  files were written at, how far a play's pitch and level may wander either way, and how\n"
               " *  much louder it is a card past the first. */\n"
               "export interface CueMix { readonly gain: number; readonly pitch: number; readonly level: number; readonly perCard: number }\n"
               "export const CUE_MIX: Readonly<Record<number, CueMix>> = {\n");
    for (int c = 1; c < ANIM_N_CUES; c++) {
        if (!CUE_CONST[c]) continue;
        const MixRow *m = &MIX_ROWS[c];
        fprintf(f, "    [%s]: { gain: %g, pitch: %g, level: %g, perCard: %g },\n", CUE_CONST[c],
                m->gain / 1000.0, m->pitch / 1000.0, m->level / 1000.0, m->per_card / 1000.0);
    }
    fprintf(f, "};\n\n/** The longest anchor of all: how early the earliest file must start. */\n"
               "export const CUE_MAX_ANCHOR_MS = %.1f;\n", lead);
    return fclose(f);
}

// ---- the same table for a C host (iOS: c/src/cue_voice.h) ---------------------------
// One render writes both, so a take's anchor and a cue's level are the same
// numbers in every host. The anchor goes over in microseconds, rounded, from the
// same double the web's table prints to a tenth of a millisecond.
static int write_ctable(const char *path, Out *outs, int n) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "// GENERATED by tools/soundgen (soundgen ctable) - do not edit, and not committed.\n"
               "// Every take soundgen writes, and how each cue sits in the mix: the C twin of\n"
               "// src/audio/gen/cueTakes.ts, read through c/src/cue_voice.h.\n\n"
               "#include \"cue_voice.h\"\n\n"
               "const CueTakeRow CUE_TAKE_ROWS[] = {\n");
    for (int i = 0; i < n; i++) {
        const Out *o = &outs[i];
        fprintf(f, "    { %d, %d, %ld, %d, %d, %d, %d, \"%s\" },\n", o->rate, o->n, lrint(o->anchor_ms * 1000),
                o->cue, o->role, o->level, (int)strlen(o->name), o->name);
    }
    fprintf(f, "};\nconst int CUE_N_TAKE_ROWS = %d;\n\nconst CueMix CUE_MIX_ROWS[ANIM_N_CUES] = {\n", n);
    for (int c = 0; c < ANIM_N_CUES; c++) {
        const MixRow *m = &MIX_ROWS[c];
        fprintf(f, "    [%d] = { %d, %d, %d, %d, %ld },\n", c, m->gain, m->pitch, m->level, m->per_card,
                lrint(RECIPES[c].cards_per_level * 1000));
    }
    fprintf(f, "};\n");
    return fclose(f);
}

// ---- the shipped files: AAC, encoded by ffmpeg ---------------------------------------
// `write` leaves a manifest beside the WAVs (one line a take: name, rate, kbps,
// samples) for `encode` and `check` to read, so they need not render again.
#define MANIFEST "takes.tsv"
typedef struct { char name[48]; int rate, kbps, n; } Entry;
static int read_manifest(const char *dir, Entry **out) {
    char path[1024];
    snprintf(path, sizeof path, "%s/" MANIFEST, dir);
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "soundgen: no %s (run `soundgen write` first)\n", path); return -1; }
    Entry *e = calloc(1024, sizeof(Entry));
    int n = 0;
    while (n < 1024 && fscanf(f, "%47s %d %d %d", e[n].name, &e[n].rate, &e[n].kbps, &e[n].n) == 4) n++;
    fclose(f);
    *out = e;
    return n;
}
// Every take into <outdir>/<name>.m4a, eight ffmpegs at a time.
static int encode(const char *wavs, const char *outdir) {
    Entry *e; const int n = read_manifest(wavs, &e);
    if (n < 0) return 1;
    int pids[8], at[8], live = 0, bad = 0;
    for (int i = 0; i < n || live > 0; ) {
        while (i < n && live < 8) {
            char w[1024], m[1024];
            snprintf(w, sizeof w, "%s/%s.wav", wavs, e[i].name);
            snprintf(m, sizeof m, "%s/%s.m4a", outdir, e[i].name);
            const int pid = m4a_encode(w, m, e[i].rate, e[i].kbps);
            if (pid < 0) { fprintf(stderr, "soundgen: cannot run ffmpeg (it encodes the shipped AAC: install it)\n"); return 1; }
            pids[live] = pid; at[live] = i; live++; i++;
        }
        if (m4a_wait(pids[0]) != 0) { fprintf(stderr, "soundgen: ffmpeg failed on %s\n", e[at[0]].name); bad++; }
        for (int k = 1; k < live; k++) { pids[k - 1] = pids[k]; at[k - 1] = at[k]; }
        live--;
    }
    free(e);
    return bad ? 1 : 0;
}
// Every written file holds its take whole and from its first sample: one edit,
// starting after the encoder's priming, as long as the take to the sample, in a
// track at the take's own rate. A decoder that honours the edit list (all of
// them, docs/AUDIO.md) then hands back the WAV's samples where the anchor
// counts them.
static int check(const char *wavs, const char *outdir) {
    Entry *e; const int n = read_manifest(wavs, &e);
    if (n < 0) return 1;
    int bad = 0; long wav_bytes = 0, m4a_bytes = 0;
    for (int i = 0; i < n; i++) {
        char m[1024];
        snprintf(m, sizeof m, "%s/%s.m4a", outdir, e[i].name);
        M4aInfo info;
        const int rc = m4a_info(m, &info);
        const int64_t edit = rc ? -1 : info.edit_duration * e[i].rate / (info.movie_timescale ? info.movie_timescale : 1);
        const int ok = rc == 0 && info.n_edits == 1 && info.edit_media_time == AAC_PRIMING
                       && (int)info.media_timescale == e[i].rate && edit == e[i].n
                       && info.media_duration >= e[i].n + AAC_PRIMING;
        if (!ok) {
            bad++;
            fprintf(stderr, "soundgen: %s.m4a: %d edits, starts at %lld (want %d), lasts %lld samples (want %d), %u Hz (want %d)\n",
                    e[i].name, info.n_edits, (long long)info.edit_media_time, AAC_PRIMING, (long long)edit, e[i].n,
                    info.media_timescale, e[i].rate);
        }
        wav_bytes += 44 + 2L * e[i].n;
        m4a_bytes += info.bytes;
    }
    printf("soundgen: %d files checked, %d bad; %ld bytes as WAV, %ld as AAC\n", n, bad, wav_bytes, m4a_bytes);
    free(e);
    return bad ? 1 : 0;
}

// ---- the common gain, and what really sounds together ----------------------------
// MIXES a plan really strikes together, each as (cue, role, level, moment ms,
// start ms or NAN to start the take its anchor early). The common gain keeps
// the loudest of them under MIX_DB: six cards of one attack landing a stagger
// apart, a refill landing, the trump turned and landed, a transfer's landing
// and slide, a run-opening pickup's strokes started at once under its lift and
// over the last attack's landing, and a sweep's five strokes over the last
// cover and its hold.
typedef struct { int cue, role, level; double moment, start; } Hit;
typedef struct { const char *what; Hit hit[12]; int n; } Mix;
#define STAG ANIM_SOUND_STAGGER_MS
static const Mix MIXES[] = {
    { "six cards of an attack", { { ANIM_CUE_PLACE, 0, 0, 0, NAN }, { ANIM_CUE_PLACE, 0, 0, STAG, NAN }, { ANIM_CUE_PLACE, 0, 0, 2 * STAG, NAN },
      { ANIM_CUE_PLACE, 0, 0, 3 * STAG, NAN }, { ANIM_CUE_PLACE, 0, 0, 4 * STAG, NAN }, { ANIM_CUE_PLACE, 0, 0, 5 * STAG, NAN } }, 6 },
    { "six covers", { { ANIM_CUE_COVER, 0, 0, 0, NAN }, { ANIM_CUE_COVER, 0, 0, STAG, NAN }, { ANIM_CUE_COVER, 0, 0, 2 * STAG, NAN },
      { ANIM_CUE_COVER, 0, 0, 3 * STAG, NAN }, { ANIM_CUE_COVER, 0, 0, 4 * STAG, NAN }, { ANIM_CUE_COVER, 0, 0, 5 * STAG, NAN } }, 6 },
    { "six refilled cards landing", { { ANIM_CUE_DEAL_LAND, 0, 0, 0, NAN }, { ANIM_CUE_DEAL_LAND, 0, 0, STAG, NAN }, { ANIM_CUE_DEAL_LAND, 0, 0, 2 * STAG, NAN },
      { ANIM_CUE_DEAL_LAND, 0, 0, 3 * STAG, NAN }, { ANIM_CUE_DEAL_LAND, 0, 0, 4 * STAG, NAN }, { ANIM_CUE_DEAL_LAND, 0, 0, 5 * STAG, NAN } }, 6 },
    { "six cards off the stock", { { ANIM_CUE_DEAL, 0, 0, 0, NAN }, { ANIM_CUE_DEAL, 0, 0, STAG, NAN }, { ANIM_CUE_DEAL, 0, 0, 2 * STAG, NAN },
      { ANIM_CUE_DEAL, 0, 0, 3 * STAG, NAN }, { ANIM_CUE_DEAL, 0, 0, 4 * STAG, NAN }, { ANIM_CUE_DEAL, 0, 0, 5 * STAG, NAN } }, 6 },
    { "the trump turned and landed", { { ANIM_CUE_TURN, 0, 0, 0, NAN }, { ANIM_CUE_FLIP, 0, 0, ANIM_TIME_MS, NAN } }, 2 },
    { "a transfer's landing and slide", { { ANIM_CUE_PLACE, 0, 0, 0, NAN }, { ANIM_CUE_PASS, 0, 0, 0, NAN } }, 2 },
    { "a pickup opening its run", { { ANIM_CUE_PLACE, 0, 0, -ANIM_GAP_MS, NAN }, { ANIM_CUE_PICKUP, ROLE_PRELUDE, 5, 0, 0 },
      { ANIM_CUE_PICKUP, 0, 0, 0, 0 }, { ANIM_CUE_INTO_HAND, 0, 0, ANIM_TIME_MS, NAN } }, 4 },
    { "a sweep's strokes over the last cover", { { ANIM_CUE_COVER, 0, 0, -ANIM_GAP_MS - ANIM_BOUT_END_HOLD_MS, NAN },
      { ANIM_CUE_COVER, 0, 0, -ANIM_GAP_MS - ANIM_BOUT_END_HOLD_MS + STAG, NAN }, { ANIM_CUE_SWEEP, ROLE_PRELUDE, 5, 0, NAN },
      { ANIM_CUE_SWEEP, 0, 0, 0, NAN }, { ANIM_CUE_PILE, 0, 0, ANIM_TIME_MS, NAN } }, 5 },
};
// The takes of (cue, role, level), in order.
static int takes_of(Out *outs, int n, int cue, int role, int level, Out **out) {
    int k = 0;
    for (int i = 0; i < n; i++) if (outs[i].x && outs[i].cue == cue && outs[i].role == role && outs[i].level == level) out[k++] = &outs[i];
    return k;
}
// The loudest sample of a mix, every take of each hit tried in turn (hit h plays
// take (r + h) of its set): before the common gain.
static double mix_peak(Out *outs, int n, const Mix *m) {
    Out *sets[12][16]; int ns[12]; int rate = 0;
    for (int h = 0; h < m->n; h++) {
        ns[h] = takes_of(outs, n, m->hit[h].cue, m->hit[h].role, m->hit[h].level, sets[h]);
        if (ns[h] == 0) return 0;
        rate = sets[h][0]->rate;
    }
    const int len = rate * 8, zero = rate * 4;          // 8 s round the moment
    double *buf = malloc(sizeof(double) * (size_t)len), worst = 0;
    for (int r = 0; r < 8; r++) {
        memset(buf, 0, sizeof(double) * (size_t)len);
        for (int h = 0; h < m->n; h++) {
            const Out *o = sets[h][(r + h) % ns[h]];
            const double st = isnan(m->hit[h].start) ? m->hit[h].moment - o->anchor_ms : m->hit[h].start;
            const int at = zero + (int)lrint(st * rate / 1000.0);
            for (int i = 0; i < o->n; i++) if (at + i >= 0 && at + i < len) buf[at + i] += o->x[i];
        }
        for (int i = 0; i < len; i++) if (fabs(buf[i]) > worst) worst = fabs(buf[i]);
    }
    free(buf);
    return worst;
}
// The one gain for every card take: its loudest at PEAK_DB, unless a mix would
// then pass MIX_DB. The card takes are then quantised; the placeholders keep
// their own level, as the cue table sets theirs.
static double common_gain(Out *outs, int n, int verbose) {
    double single = 0;
    for (int i = 0; i < n; i++) if (outs[i].x) for (int k = 0; k < outs[i].n; k++) if (fabs(outs[i].x[k]) > single) single = fabs(outs[i].x[k]);
    double g = db2(PEAK_DB) / single, mix = 0;
    for (unsigned m = 0; m < sizeof MIXES / sizeof MIXES[0]; m++) {
        const double p = mix_peak(outs, n, &MIXES[m]);
        if (verbose) fprintf(stderr, "soundgen:   %-40s peaks %6.1f dBFS at the take gain\n", MIXES[m].what, 20 * log10(p * g + 1e-12));
        if (p > mix) mix = p;
    }
    if (mix * g > db2(MIX_DB)) g = db2(MIX_DB) / mix;
    if (verbose) fprintf(stderr, "soundgen: common gain %+.1f dB: the loudest take %.1f dBFS, the loudest mix %.1f dBFS\n",
                         20 * log10(g), 20 * log10(single * g), 20 * log10(mix * g));
    for (int i = 0; i < n; i++) {
        if (!outs[i].x) continue;
        outs[i].pcm = malloc(sizeof(int16_t) * (size_t)outs[i].n);
        for (int k = 0; k < outs[i].n; k++) outs[i].pcm[k] = to16(outs[i].x[k] * g);
    }
    return g;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "parts") && argc > 2) return parts(argv[2]);
    if (!strcmp(mode, "encode") && argc > 3) return encode(argv[2], argv[3]);
    if (!strcmp(mode, "check") && argc > 3) return check(argv[2], argv[3]);
    const int writing = !strcmp(mode, "write") && argc > 3;
    const int table_only = !strcmp(mode, "table") && argc > 2;
    const int ctable = !strcmp(mode, "ctable") && argc > 2;
    const int stats = !strcmp(mode, "stats");
    if (!writing && !stats && !table_only && !ctable && strcmp(mode, "list")) {
        fprintf(stderr, "usage: soundgen write <wavdir> <cueTakes.ts> | table <cueTakes.ts> | encode <wavdir> <outdir>\n"
                        "                | check <wavdir> <outdir> | ctable <cue_takes.c> | list | stats | parts <dir>\n");
        return 2;
    }
    Out *outs = calloc(512, sizeof(Out));
    int n = 0;
    long total = 0;
    double loudest = 0;
    for (int cue = 1; cue < ANIM_N_CUES; cue++) {
        const Recipe *r = &RECIPES[cue];
        if (!r->main.make) {
            if (render_placeholder(cue, 0, &outs[n])) { total += 44 + 2L * outs[n].n; n++; }
            continue;
        }
        const Set *sets[3] = { &r->main, &r->tight, &r->prelude };
        for (int role = ROLE_MAIN; role <= ROLE_PRELUDE; role++) {
            const Set *set = sets[role];
            if (!set->make) continue;
            for (int level = set->lo; level <= set->hi; level++) {
                for (int v = 0; v < set->takes; v++) {
                    Out *o = &outs[n++];
                    render_card(cue, role, set, level, v, o);
                    total += 44 + 2L * o->n;
                }
            }
        }
    }
    common_gain(outs, n, writing || stats);
    for (int i = 0; i < n; i++) if (outs[i].x) for (int k = 0; k < outs[i].n; k++) if (abs(outs[i].pcm[k]) / 32767.0 > loudest) loudest = abs(outs[i].pcm[k]) / 32767.0;
    FILE *manifest = NULL;
    if (writing) {
        char path[1024];
        snprintf(path, sizeof path, "%s/" MANIFEST, argv[2]);
        if (!(manifest = fopen(path, "w"))) { fprintf(stderr, "soundgen: cannot write %s\n", path); return 1; }
    }
    for (int i = 0; i < n && !table_only && !ctable; i++) {
        const Out *o = &outs[i];
        if (writing) {
            fprintf(manifest, "%s %d %d %d\n", o->name, o->rate, o->kbps, o->n);
            char path[1024];
            snprintf(path, sizeof path, "%s/%s.wav", argv[2], o->name);
            if (write_pcm(path, o->pcm, o->n, o->rate) != 0) { fprintf(stderr, "soundgen: cannot write %s\n", path); return 1; }
        } else if (stats) {
            int pk = 0;
            Sig y = sig_new(o->n);
            for (int k = 0; k < o->n; k++) { y.x[k] = o->pcm[k] / 32767.0; if (abs(o->pcm[k]) > pk) pk = abs(o->pcm[k]); }
            const double r50 = max_moving_rms(&y, o->rate / 20, 0, y.n);   // the loudest 50 ms
            sig_free(&y);
            printf("%-24s cue %2d  %5d Hz  %5d ms  %7ld B  anchor %7.1f ms  peak %6.1f dBFS  50 ms RMS %6.1f dBFS\n",
                   o->name, o->cue, o->rate, o->n * 1000 / o->rate, 44 + 2L * o->n, o->anchor_ms,
                   20 * log10(pk / 32767.0 + 1e-9), 20 * log10(r50 + 1e-9));
        } else {
            printf("%s.m4a\t%d\t%d\t%d\t%ld\t%.1f\t%s\t%d\t%d\n", o->name, o->cue, o->rate, o->n * 1000 / o->rate, 44 + 2L * o->n,
                   o->anchor_ms, ROLE_NAME[o->role], o->level, o->kbps);
        }
    }
    if (manifest) fclose(manifest);
    if ((writing || table_only) && write_table(argv[writing ? 3 : 2], outs, n) != 0) {
        fprintf(stderr, "soundgen: cannot write %s\n", argv[writing ? 3 : 2]); return 1;
    }
    if (ctable && write_ctable(argv[2], outs, n) != 0) { fprintf(stderr, "soundgen: cannot write %s\n", argv[2]); return 1; }
    if (writing || stats)
        fprintf(writing ? stderr : stdout, "soundgen: %d files, %ld bytes, loudest card take %.1f dBFS\n",
                n, total, 20 * log10(loudest + 1e-9));
    if (loudest > db2(PEAK_DB) * 1.001) { fprintf(stderr, "soundgen: a card take is over PEAK_DB\n"); return 1; }
    return 0;
}
