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
// the file list and the mix, into the take table every host plays by
// (`soundgen ctable`: c/build/gen/cue_takes.c, linked into web.wasm and the iOS
// kernel beside c/src/cue_voice.c, which decides what a strike plays from it).
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
// THE TIMING AND THE LEVELS ARE tuning.tsv's: where each part of a gesture sits
// from its cue's moment and how loud it is on the deal's scale (g = -34..-32
// dBFS), from each *_game.py and the levels study, and the whole set is lifted
// by one common gain, so the loudest take peaks near full scale (PEAK_DB).
// Balancing the cues against each other is that file's mix lines (MIX_ROWS).
// The tuner on /sounds edits that file by ear (`npm run tune`).
//
// NOTHING IT WRITES IS COMMITTED: `npm run sounds` writes public/audio/gen/ and
// c/Makefile writes c/build/gen/cue_takes.c, both ignored, and the hooks that
// build the site run them.
//
// TIED TO THE KERNEL'S CUE LIST BY THE COMPILER: the table below is indexed by
// ANIM_CUE_* and sized by ANIM_N_CUES (the _Static_assert).
//
// DETERMINISTIC: every draw is shared/c/rng.h seeded per cue, level and take,
// and nothing reads a clock. Across machines the last bit can differ (libm).
//
//   soundgen write <dir> [<c>]  every file into <dir>, and its manifest (and the take table, from the same render)
//   soundgen ctable <c>         the take table every host plays by, alone
//   soundgen list               one line a file: name, cue id, rate, ms, bytes, anchor ms, role, level
//   soundgen stats              the same, with peak and RMS
//   soundgen parts <dir>        each model on its own at 48 kHz, 12 takes, named
//                               as its Python names them: what measure.py compares

// fork and mmap: the takes are rendered by several processes (render_all).
#if !defined(__APPLE__)
#define _DEFAULT_SOURCE
#endif
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "anim_plan.h"
#include "cue_voice.h"
#include "c/dsp.h"
#include "c/models.h"
#include "c/placeholders.h"
#include "c/m4a.h"

#define S_(sec) ((int)((sec) * SR))
static double db2(double db) { return pow(10, db / 20); }

// THE TIMING AND LEVEL OF EVERY PART are not here: tuning.tsv holds them, one
// line a part of a cue's gesture (where it sits from the cue's moment, how loud,
// and how far each may wander a take at a time), and the mix of every cue
// (MIX_ROWS). read_tuning() loads it before anything renders, and a line
// missing, unknown or out of range stops the build. It is the one owner: the
// tuner on /sounds edits that file and nothing else (docs/AUDIO.md "How to tune").
// The ids below are the parts the gestures place, each named in the file by its
// cue, role and part.
typedef enum {
    T_DEAL_THUMB, T_DEAL_GRAB, T_DEAL_TIGHT_THUMB, T_DEAL_TIGHT_GRAB, T_DEAL_LAND,
    T_TURN_FLIP, T_FLIP_LAND, T_FLIP_DECK,
    T_LIFT_GRAB, T_LIFT_THUMB, T_LIFT_TIGHT_GRAB, T_LIFT_TIGHT_THUMB,
    T_PLACE_LAND, T_COVER_LAND, T_PASS_SLIDE,
    T_SWEEP_STROKES, T_SWEEP_LIFT, T_PILE_PLACE, T_PICKUP_STROKES, T_PICKUP_LIFT, T_INTO_HAND_SLIDE,
    N_TUNE
} TuneId;
enum { ROLE_MAIN, ROLE_TIGHT, ROLE_PRELUDE };
static const char *const ROLE_NAME[] = { "main", "tight", "prelude" };
static const struct { int cue, role; const char *part; } TUNE_KEY[N_TUNE] = {
    [T_DEAL_THUMB]       = { ANIM_CUE_DEAL, ROLE_MAIN, "thumb" },
    [T_DEAL_GRAB]        = { ANIM_CUE_DEAL, ROLE_MAIN, "grab" },
    [T_DEAL_TIGHT_THUMB] = { ANIM_CUE_DEAL, ROLE_TIGHT, "thumb" },
    [T_DEAL_TIGHT_GRAB]  = { ANIM_CUE_DEAL, ROLE_TIGHT, "grab" },
    [T_DEAL_LAND]        = { ANIM_CUE_DEAL_LAND, ROLE_MAIN, "land" },
    [T_TURN_FLIP]        = { ANIM_CUE_TURN, ROLE_MAIN, "flip" },
    [T_FLIP_LAND]        = { ANIM_CUE_FLIP, ROLE_MAIN, "land" },
    [T_FLIP_DECK]        = { ANIM_CUE_FLIP, ROLE_MAIN, "deck" },
    [T_LIFT_GRAB]        = { ANIM_CUE_LIFT, ROLE_MAIN, "grab" },
    [T_LIFT_THUMB]       = { ANIM_CUE_LIFT, ROLE_MAIN, "thumb" },
    [T_LIFT_TIGHT_GRAB]  = { ANIM_CUE_LIFT, ROLE_TIGHT, "grab" },
    [T_LIFT_TIGHT_THUMB] = { ANIM_CUE_LIFT, ROLE_TIGHT, "thumb" },
    [T_PLACE_LAND]       = { ANIM_CUE_PLACE, ROLE_MAIN, "land" },
    [T_COVER_LAND]       = { ANIM_CUE_COVER, ROLE_MAIN, "land" },
    [T_PASS_SLIDE]       = { ANIM_CUE_PASS, ROLE_MAIN, "slide" },
    [T_SWEEP_STROKES]    = { ANIM_CUE_SWEEP, ROLE_PRELUDE, "strokes" },
    [T_SWEEP_LIFT]       = { ANIM_CUE_SWEEP, ROLE_MAIN, "lift" },
    [T_PILE_PLACE]       = { ANIM_CUE_PILE, ROLE_MAIN, "place" },
    [T_PICKUP_STROKES]   = { ANIM_CUE_PICKUP, ROLE_PRELUDE, "strokes" },
    [T_PICKUP_LIFT]      = { ANIM_CUE_PICKUP, ROLE_MAIN, "lift" },
    [T_INTO_HAND_SLIDE]  = { ANIM_CUE_INTO_HAND, ROLE_MAIN, "slide" },
};
typedef struct { double at_ms, at_spread_ms, db, db_spread; } Tune;
static Tune TUNE[N_TUNE];
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
// Where part `id` sits from the cue, in seconds, and its level in dB for this
// take: drawn up to its spread quieter (no draw where it has none).
static double at_of(TuneId id) { return TUNE[id].at_ms / 1000; }
// THE TUNER'S SOLO (`soundgen solo`): one part heard alone, every other part of its
// take silenced. They are still drawn, so the solo part is the very one the take has.
static int SOLO = -1;
static double level_of(Take *t, TuneId id) {
    const Tune *u = &TUNE[id];
    const double db = u->db_spread > 0 ? t->g - uni(&t->r, -u->db, -u->db + u->db_spread) : t->g + u->db;
    return SOLO >= 0 && (int)id != SOLO ? -400.0 : db;
}
static void take_init(Take *t, uint64_t seed) { t->mix = sig_new(SPAN); rng_init(&t->r, seed); t->g = uni(&t->r, -34, -32); }
// Add `part` with its sample `ref` at `at_s` from the cue, its peak at `db` (the games' put()).
static void put_peak(Take *t, Sig part, int ref, double at_s, double db) {
    const double k = db2(db) / (sig_peak(part.x, part.n) + 1e-12);
    const int o = CUE0 + S_(at_s) - ref;
    for (int i = 0; i < part.n; i++) if (o + i >= 0 && o + i < SPAN) t->mix.x[o + i] += part.x[i] * k;
    sig_free(&part);
}
// ...or with its loudest 50 ms RMS (150 Hz-15 kHz) at `db` (disc_game.py's put()).
static void put_rms50(Take *t, Sig part, int ref, double at_s, double db) {
    Sig b = bp_copy(&part, 150, 15000);
    const double m = max_moving_rms(&b, 2400, 0, b.n);
    sig_free(&b);
    const double k = db2(db) / (m + 1e-12);
    const int o = CUE0 + S_(at_s) - ref;
    for (int i = 0; i < part.n; i++) if (o + i >= 0 && o + i < SPAN) t->mix.x[o + i] += part.x[i] * k;
    sig_free(&part);
}

// ---- the gestures: each *_game.py at the kernel's moments -------------------------
// Each places its parts where tuning.tsv says, from the cue's moment; the
// comments say what the defaults were set from.
#define PEAK(id, sig, ref) put_peak(t, sig, ref, at_of(id), level_of(t, id))
#define RMS50(id, sig, ref) put_rms50(t, sig, ref, at_of(id), level_of(t, id))
// DEAL (deal/an/deal_game.py): the thumb slide's peak 130 ms before the grab,
// the grab at the moment the card leaves the stock.
static void g_deal(Take *t, int level) {
    (void)level;
    Sig s = m_thumb(&t->r); PEAK(T_DEAL_THUMB, s, sig_argmax_abs(&s));
    Sig g = m_grab(&t->r); PEAK(T_DEAL_GRAB, g, sig_argmax_abs(&g));
}
// ...and TIGHT, for a card dealt with no time before it: the slide's peak 50 ms
// before the grab (its swell still before that), so played late from the
// moment it is over a sixth of a second sooner.
static void g_deal_tight(Take *t, int level) {
    (void)level;
    Sig s = m_thumb(&t->r); PEAK(T_DEAL_TIGHT_THUMB, s, sig_argmax_abs(&s));
    Sig g = m_grab(&t->r); PEAK(T_DEAL_TIGHT_GRAB, g, sig_argmax_abs(&g));
}
// DEAL_LAND: the card on the cloth, land229, quietest of the three. At the
// viewer's own seat; the host scales it by the seat (anim_seat_gain_pm).
static void g_deal_land(Take *t, int level) {
    (void)level;
    Sig l = m_land(&t->r); PEAK(T_DEAL_LAND, l, sig_argmax_abs(&l));
}
// TURN (flip/an/flip_game.py): the trump flipped, its snap as it leaves the stock.
static void g_turn(Take *t, int level) {
    (void)level;
    Sig f = m_flip(&t->r); PEAK(T_TURN_FLIP, f, S_(FLIP_REF_S));
}
// FLIP: the trump landing (a throw-in, Alex: "similar to a throw in"), then the
// stock set down on it 350 ms later ("350 ms after trump lands is fine"). Baked
// into one file because nothing on screen moves at the set-down: there is no
// moment for the kernel to name, only the sound's own rhythm.
static void g_flip_land(Take *t, int level) {
    (void)level;
    Sig l = m_throw(&t->r); PEAK(T_FLIP_LAND, l, sig_argmax_abs(&l));
    Sig d = m_deck(&t->r); PEAK(T_FLIP_DECK, d, S_(DECK_REF_S));
}
// LIFT (throwin/an/throwin_game.py): two fingers take the card, the thumb slides
// it out, it leaves: the grab 110 ms before the slide's peak, the slide's peak
// 130 ms before the card leaves. The same for an attack, a cover and a pass.
static void g_lift(Take *t, int level) {
    (void)level;
    Sig s = m_thumb(&t->r); PEAK(T_LIFT_THUMB, s, sig_argmax_abs(&s));
    Sig g = m_tgrab(&t->r); PEAK(T_LIFT_GRAB, g, S_(TGRAB_REF_S));
}
// ...and TIGHT (Alex: "maybe put them closer to each other"), for a move that
// opens its run and so has no time before its lift: the grab 60 ms before the
// slide's peak and the peak 30 ms before the card leaves, where they were 110
// and 130 ms. The host plays it from the moment, whole.
static void g_lift_tight(Take *t, int level) {
    (void)level;
    Sig s = m_thumb(&t->r); PEAK(T_LIFT_TIGHT_THUMB, s, sig_argmax_abs(&s));
    Sig g = m_tgrab(&t->r); PEAK(T_LIFT_TIGHT_GRAB, g, S_(TGRAB_REF_S));
}
// PLACE: the throw-in's landing.
static void g_place(Take *t, int level) {
    (void)level;
    Sig l = m_throw(&t->r); PEAK(T_PLACE_LAND, l, sig_argmax_abs(&l));
}
// COVER: the cover's landing, on a card.
static void g_cover(Take *t, int level) {
    (void)level;
    Sig l = m_cover(&t->r); PEAK(T_COVER_LAND, l, sig_argmax_abs(&l));
}
// PASS (pass/an/pass_game.py): struck at the transfer's landing, beside its
// PLACE; the "shfff" starts 150 ms after it.
static void g_pass(Take *t, int level) {
    (void)level;
    Sig p = m_pass(&t->r); PEAK(T_PASS_SLIDE, p, S_(PASS_REF_S));
}
// The strokes gathering the table (discard/an/disc_game.py), the last ending
// where `id` says before the cue, at its level (their 50 ms RMS): each 0.8 times
// a recorded stroke's length (0.25-0.7 s), 60-200 ms apart, a little louder as
// the pile grows. Not compressed: they are heard in full, over whatever is sounding.
static void strokes(Take *t, int count, TuneId id) {
    const Tune *u = &TUNE[id];
    const double gap = uni(&t->r, -u->at_ms / 1000, (-u->at_ms + u->at_spread_ms) / 1000);
    const double sc = level_of(t, id);
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
        put_rms50(t, y, S_(STROKE_PRE_S), at, sc - 1.5 + 3.0 * s / (count > 1 ? count - 1 : 1) - uni(&t->r, 0, 1.5));
        at += d[s] + gp[s];
    }
}
// SWEEP: the pile lifted and turned, as the sweep opens (its strokes are the
// prelude below). disc_game.py's levels: the strokes at g-30.5, the lift 5 dB
// over them (and the levels study's +7 and +18 on top).
static void g_sweep(Take *t, int level) {
    (void)level;
    Sig l = m_lift(&t->r); RMS50(T_SWEEP_LIFT, l, S_(EVENT_PRE_S));
}
// ...its prelude: `level` strokes, the last 100-180 ms before the lift.
static void g_sweep_strokes(Take *t, int level) { strokes(t, level, T_SWEEP_STROKES); }
// PILE: the swept cards set softly on the discard pile, as they land.
static void g_pile(Take *t, int level) {
    (void)level;
    Sig p = m_place(&t->r); RMS50(T_PILE_PLACE, p, S_(EVENT_PRE_S));
}
// PICKUP: the one soft lift, as the table lifts, 10 dB under the strokes (before the study).
static void g_pickup(Take *t, int level) {
    (void)level;
    Sig l = m_plift(&t->r); RMS50(T_PICKUP_LIFT, l, S_(EVENT_PRE_S));
}
// ...its prelude: `level` strokes, the last 50-120 ms before the lift.
static void g_pickup_strokes(Take *t, int level) { strokes(t, level, T_PICKUP_STROKES); }
// INTO_HAND: the cards slid into the fanned hand as they land, its onset there.
static void g_into_hand(Take *t, int level) {
    (void)level;
    Sig s = m_insert(&t->r); RMS50(T_INTO_HAND_SLIDE, s, S_(STROKE_PRE_S));
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

// ---- the mix: how loud each cue sits against the others ----------------------------
// THE ONE PLACE A CUE'S LEVEL IS SET, for every host: tuning.tsv's `mix` lines,
// which soundgen writes into the take table (c/build/gen/cue_takes.c), which
// c/src/cue_voice.c reads for the web and for iOS alike, so the two can never be
// mixed differently. Thousandths: gain, pitch, level and per_card as the file's
// header says. NONE has no line and is silent.
typedef struct { int gain, pitch, level, per_card; } MixRow;
static MixRow MIX_ROWS[ANIM_N_CUES];

// ---- tuning.tsv: read before anything renders ----------------------------------------
// SOUNDGEN_TUNING names another file (the tests' and the tuner's own). Every part
// of TUNE_KEY and every cue but NONE must have exactly one line; anything else,
// or a number out of range, is an error: 0, or -1 with the reason on stderr.
static int cue_named(const char *name) {
    for (int k = 1; k < ANIM_N_CUES; k++) if (!strcmp(RECIPES[k].name, name)) return k;
    return -1;
}
// The tab-separated fields of `line` into f (at most `max`); how many.
static int fields(char *line, char **f, int max) {
    int n = 0;
    for (char *p = line; n < max; ) {
        f[n++] = p;
        char *tab = strchr(p, '\t');
        if (!tab) break;
        *tab = 0; p = tab + 1;
    }
    char *end = f[n - 1] + strcspn(f[n - 1], "\r\n");
    *end = 0;
    return n;
}
static int number(const char *s, double lo, double hi, double *out) {
    char *end;
    const double v = strtod(s, &end);
    if (end == s || *end || !(v >= lo && v <= hi)) return -1;
    *out = v;
    return 0;
}
static int read_tuning(void) {
    const char *path = getenv("SOUNDGEN_TUNING");
    if (!path || !*path) path = "tuning.tsv";
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "soundgen: cannot read %s\n", path); return -1; }
    int seen_part[N_TUNE] = { 0 }, seen_mix[ANIM_N_CUES] = { 0 }, bad = 0;
    char line[1024];
    for (int ln = 1; fgets(line, sizeof line, f); ln++) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r' || line[0] == 0) continue;
        char *fl[10];
        const int nf = fields(line, fl, 10);
        int ok = 0;
        if (nf == 9 && !strcmp(fl[0], "part")) {
            const int cue = cue_named(fl[1]);
            int role = -1, id = -1;
            for (int r = 0; r < 3; r++) if (!strcmp(ROLE_NAME[r], fl[2])) role = r;
            for (int i = 0; i < N_TUNE; i++)
                if (TUNE_KEY[i].cue == cue && TUNE_KEY[i].role == role && !strcmp(TUNE_KEY[i].part, fl[3])) id = i;
            Tune u;
            if (id >= 0 && !seen_part[id] && *fl[8]
                && !number(fl[4], -3000, 3000, &u.at_ms) && !number(fl[5], 0, 1000, &u.at_spread_ms)
                && !number(fl[6], -80, 40, &u.db) && !number(fl[7], 0, 30, &u.db_spread)) {
                TUNE[id] = u; seen_part[id] = 1; ok = 1;
            }
        } else if (nf == 6 && !strcmp(fl[0], "mix")) {
            const int cue = cue_named(fl[1]);
            double g, p, l, c;
            if (cue > 0 && !seen_mix[cue] && !number(fl[2], 0, 8000, &g) && !number(fl[3], 0, 500, &p)
                && !number(fl[4], 0, 1000, &l) && !number(fl[5], 0, 600, &c)
                && g == (int)g && p == (int)p && l == (int)l && c == (int)c) {
                MIX_ROWS[cue] = (MixRow){ (int)g, (int)p, (int)l, (int)c }; seen_mix[cue] = 1; ok = 1;
            }
        }
        if (!ok) { fprintf(stderr, "soundgen: %s line %d: not a part or mix line soundgen knows, a second line for one, or out of range\n", path, ln); bad++; }
    }
    fclose(f);
    for (int i = 0; i < N_TUNE; i++)
        if (!seen_part[i]) { fprintf(stderr, "soundgen: %s has no line for part %s %s %s\n", path, RECIPES[TUNE_KEY[i].cue].name, ROLE_NAME[TUNE_KEY[i].role], TUNE_KEY[i].part); bad++; }
    for (int c = 1; c < ANIM_N_CUES; c++)
        if (!seen_mix[c]) { fprintf(stderr, "soundgen: %s has no mix line for %s\n", path, RECIPES[c].name); bad++; }
    return bad ? -1 : 0;
}

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

// ---- every card take, rendered by several processes ------------------------------
// A take is its own seed and its own timeline, so the takes are rendered in any
// order and by any number of processes with the same samples: forked workers
// (not threads: some models keep scratch arrays) take the next job off a counter
// in shared memory and leave the take there, and the parent copies them out.
// SOUNDGEN_JOBS sets how many (1: in this process); the files are the same
// whatever it is.
typedef struct { int cue, role, level, v; const Set *set; Out *o; } Job;
typedef struct { char name[48]; int cue, level, role, rate, n, kbps, ok; double anchor_ms; } Done;
static int workers(int njobs) {
    const char *e = getenv("SOUNDGEN_JOBS");
    long w = e && *e ? strtol(e, NULL, 10) : sysconf(_SC_NPROCESSORS_ONLN);
    if (w < 1) w = 1;
    if (w > 16) w = 16;
    return (int)(w > njobs ? (njobs > 0 ? njobs : 1) : w);
}
static int render_jobs(Job *jobs, int nj) {
    const int w = workers(nj);
    if (w == 1) {
        for (int j = 0; j < nj; j++) render_card(jobs[j].cue, jobs[j].role, jobs[j].set, jobs[j].level, jobs[j].v, jobs[j].o);
        return 0;
    }
    const size_t meta_bytes = sizeof(int) + sizeof(Done) * (size_t)nj, x_bytes = sizeof(double) * (size_t)SPAN * (size_t)nj;
    void *meta = mmap(NULL, meta_bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    double *xs = mmap(NULL, x_bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (meta == MAP_FAILED || xs == MAP_FAILED) { fprintf(stderr, "soundgen: cannot map %zu bytes\n", x_bytes); return -1; }
    int *next = meta;
    Done *done = (Done *)((char *)meta + sizeof(int));
    pid_t pids[16];
    for (int k = 0; k < w; k++) {
        pids[k] = fork();
        if (pids[k] < 0) { fprintf(stderr, "soundgen: cannot fork\n"); return -1; }
        if (pids[k] == 0) {
            for (int j; (j = __atomic_fetch_add(next, 1, __ATOMIC_RELAXED)) < nj; ) {
                Out o; memset(&o, 0, sizeof o);
                render_card(jobs[j].cue, jobs[j].role, jobs[j].set, jobs[j].level, jobs[j].v, &o);
                if (o.n > SPAN) _exit(3);
                memcpy(xs + (size_t)j * SPAN, o.x, sizeof(double) * (size_t)o.n);
                Done *d = &done[j];
                memcpy(d->name, o.name, sizeof d->name);
                d->cue = o.cue; d->level = o.level; d->role = o.role; d->rate = o.rate; d->n = o.n; d->kbps = o.kbps;
                d->anchor_ms = o.anchor_ms; d->ok = 1;
                free(o.x);
            }
            _exit(0);
        }
    }
    int bad = 0;
    for (int k = 0; k < w; k++) { int st; if (waitpid(pids[k], &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st)) bad++; }
    for (int j = 0; j < nj && !bad; j++) {
        const Done *d = &done[j];
        Out *o = jobs[j].o;
        if (!d->ok) { bad++; break; }
        memcpy(o->name, d->name, sizeof o->name);
        o->cue = d->cue; o->level = d->level; o->role = d->role; o->rate = d->rate; o->n = d->n; o->kbps = d->kbps;
        o->anchor_ms = d->anchor_ms;
        o->x = malloc(sizeof(double) * (size_t)d->n);
        memcpy(o->x, xs + (size_t)j * SPAN, sizeof(double) * (size_t)d->n);
    }
    munmap(meta, meta_bytes); munmap(xs, x_bytes);
    if (bad) fprintf(stderr, "soundgen: a render worker failed\n");
    return bad ? -1 : 0;
}

// THE TUNER'S CACHE (SOUNDGEN_CACHE=<dir>; tools/soundgen/tune.sh sets it and
// nothing else does): each take as rendered, before the common gain, under a key
// of its cue, role, level and take and every tuning.tsv line of its cue and
// role. A take whose lines did not change is read back instead of rendered, so
// moving one part re-renders that one set. The samples read back are the ones
// written, so the files are the same either way; tune.sh empties the directory
// whenever soundgen itself is rebuilt, which is the one change the key cannot see.
typedef struct { uint64_t key; Done d; } CacheHead;
static uint64_t job_key(const Job *j) {
    uint64_t h = 1469598103934665603ull;
    const int head[4] = { j->cue, j->role, j->level, j->v };
    const unsigned char *p = (const unsigned char *)head;
    for (size_t i = 0; i < sizeof head; i++) { h ^= p[i]; h *= 1099511628211ull; }
    for (int t = 0; t < N_TUNE; t++) {
        if (TUNE_KEY[t].cue != j->cue || TUNE_KEY[t].role != j->role) continue;
        p = (const unsigned char *)&TUNE[t];
        for (size_t i = 0; i < sizeof TUNE[t]; i++) { h ^= p[i]; h *= 1099511628211ull; }
    }
    return h;
}
static void cache_path(const char *dir, const Job *j, char *out, size_t cap) {
    snprintf(out, cap, "%s/c%d_r%d_l%d_v%d.take", dir, j->cue, j->role, j->level, j->v);
}
static int cache_load(const char *dir, const Job *j) {
    char path[1024];
    cache_path(dir, j, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    CacheHead h;
    int ok = fread(&h, sizeof h, 1, f) == 1 && h.key == job_key(j) && h.d.ok && h.d.n > 0 && h.d.n <= SPAN;
    Out *o = j->o;
    if (ok) {
        o->x = malloc(sizeof(double) * (size_t)h.d.n);
        ok = fread(o->x, sizeof(double), (size_t)h.d.n, f) == (size_t)h.d.n;
        if (!ok) { free(o->x); o->x = NULL; }
    }
    fclose(f);
    if (!ok) return 0;
    memcpy(o->name, h.d.name, sizeof o->name);
    o->cue = h.d.cue; o->level = h.d.level; o->role = h.d.role; o->rate = h.d.rate; o->n = h.d.n; o->kbps = h.d.kbps;
    o->anchor_ms = h.d.anchor_ms;
    return 1;
}
static void cache_save(const char *dir, const Job *j) {
    char path[1024];
    cache_path(dir, j, path, sizeof path);
    const Out *o = j->o;
    CacheHead h;
    memset(&h, 0, sizeof h);
    h.key = job_key(j);
    memcpy(h.d.name, o->name, sizeof h.d.name);
    h.d.cue = o->cue; h.d.level = o->level; h.d.role = o->role; h.d.rate = o->rate; h.d.n = o->n; h.d.kbps = o->kbps;
    h.d.anchor_ms = o->anchor_ms; h.d.ok = 1;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const int ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(o->x, sizeof(double), (size_t)o->n, f) == (size_t)o->n;
    if (fclose(f) != 0 || !ok) remove(path);
}
static int render_all(Job *jobs, int nj) {
    const char *dir = getenv("SOUNDGEN_CACHE");
    if (!dir || !*dir) return render_jobs(jobs, nj);
    Job *todo = malloc(sizeof(Job) * (size_t)(nj > 0 ? nj : 1));
    int nt = 0;
    for (int j = 0; j < nj; j++) if (!cache_load(dir, &jobs[j])) todo[nt++] = jobs[j];
    const int rc = render_jobs(todo, nt);
    if (rc == 0) for (int t = 0; t < nt; t++) cache_save(dir, &todo[t]);
    fprintf(stderr, "soundgen: %d of %d takes rendered, the rest read back from %s\n", nt, nj, dir);
    free(todo);
    return rc;
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
// A file whose bytes are already these is left alone, so that `encode` (which
// skips a take whose .m4a is newer than its WAV) re-encodes only what changed:
// the tuner's rebuild (tune.sh). `make sounds` empties both directories first.
static void put_le(unsigned char *p, uint32_t v, int bytes) { for (int i = 0; i < bytes; i++) p[i] = (unsigned char)((v >> (8 * i)) & 0xff); }
static int write_pcm(const char *path, const int16_t *x, int n, int rate) {
    const uint32_t data = (uint32_t)n * 2;
    const size_t size = 44 + (size_t)data;
    unsigned char *b = malloc(size);
    memcpy(b, "RIFF", 4); put_le(b + 4, 36 + data, 4); memcpy(b + 8, "WAVEfmt ", 8);
    put_le(b + 16, 16, 4); put_le(b + 20, 1, 2); put_le(b + 22, 1, 2); put_le(b + 24, (uint32_t)rate, 4);
    put_le(b + 28, (uint32_t)rate * 2, 4); put_le(b + 32, 2, 2); put_le(b + 34, 16, 2);
    memcpy(b + 36, "data", 4); put_le(b + 40, data, 4);
    for (int i = 0; i < n; i++) put_le(b + 44 + 2 * (size_t)i, (uint32_t)(uint16_t)x[i], 2);
    FILE *f = fopen(path, "rb");
    if (f) {
        unsigned char *old = malloc(size + 1);
        const size_t got = fread(old, 1, size + 1, f);
        fclose(f);
        const int same = got == size && !memcmp(old, b, size);
        free(old);
        if (same) { free(b); return 0; }
    }
    f = fopen(path, "wb");
    if (!f) { free(b); return -1; }
    const size_t put = fwrite(b, 1, size, f);
    free(b);
    return fclose(f) != 0 || put != size ? -1 : 0;
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

// ---- the take table every host plays by (c/src/cue_voice.h) ----------------------------
// Each file's name, rate, length to the sample and anchor (in microseconds,
// rounded), its cue, role and level, and each cue's mix. c/src/cue_voice_table.c
// is its one reader, for the web (web.wasm) and iOS alike.
static int write_ctable(const char *path, Out *outs, int n) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "// GENERATED by tools/soundgen (soundgen ctable) - do not edit, and not committed.\n"
               "// Every take soundgen writes, and how each cue sits in the mix, read through\n"
               "// c/src/cue_voice.h by every host.\n\n"
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

// The same table as bytes (cue_voice.h "THE TUNER'S TABLE"), for the tuner to hand a
// running page's kernel in place of the linked one.
static int write_ctable_bin(const char *path, Out *outs, int n) {
    if (n > CUE_TABLE_MAX_ROWS) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    const uint32_t magic = CUE_TABLE_MAGIC;
    const int32_t rows = n;
    int ok = fwrite(&magic, 4, 1, f) == 1 && fwrite(&rows, 4, 1, f) == 1;
    for (int i = 0; i < n && ok; i++) {
        const Out *o = &outs[i];
        CueTakeRow r;
        memset(&r, 0, sizeof r);
        r.rate = o->rate; r.n_samples = o->n; r.anchor_us = (int32_t)lrint(o->anchor_ms * 1000);
        r.cue = (uint8_t)o->cue; r.role = (uint8_t)o->role; r.level = (uint8_t)o->level;
        r.name_len = (uint8_t)strlen(o->name);
        if (r.name_len >= CUE_NAME_MAX) { ok = 0; break; }
        memcpy(r.name, o->name, r.name_len);
        ok = fwrite(&r, sizeof r, 1, f) == 1;
    }
    for (int c = 0; c < ANIM_N_CUES && ok; c++) {
        const MixRow *m = &MIX_ROWS[c];
        const CueMix x = { m->gain, m->pitch, m->level, m->per_card, (int32_t)lrint(RECIPES[c].cards_per_level * 1000) };
        ok = fwrite(&x, sizeof x, 1, f) == 1;
    }
    return fclose(f) != 0 || !ok ? -1 : 0;
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
// Is `a` there and modified no earlier than `b`? To the nanosecond.
static int newer(const char *a, const char *b) {
    struct stat sa, sb;
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0) return 0;
#ifdef __APPLE__
    const struct timespec ta = sa.st_mtimespec, tb = sb.st_mtimespec;
#else
    const struct timespec ta = sa.st_mtim, tb = sb.st_mtim;
#endif
    return ta.tv_sec > tb.tv_sec || (ta.tv_sec == tb.tv_sec && ta.tv_nsec >= tb.tv_nsec);
}
// Every take into <outdir>/<name>.m4a, eight ffmpegs at a time; a take whose
// .m4a is already newer than its WAV is skipped (only the tuner's rebuild ever
// has one: `make sounds` starts from empty directories). A failed encode leaves
// no file behind to be skipped next time.
static int encode(const char *wavs, const char *outdir) {
    Entry *e; const int n = read_manifest(wavs, &e);
    if (n < 0) return 1;
    int pids[8], at[8], live = 0, bad = 0, done = 0;
    for (int i = 0; i < n || live > 0; ) {
        while (i < n && live < 8) {
            char w[1024], m[1024];
            snprintf(w, sizeof w, "%s/%s.wav", wavs, e[i].name);
            snprintf(m, sizeof m, "%s/%s.m4a", outdir, e[i].name);
            if (newer(m, w)) { i++; continue; }
            done++;
            // Named, for the tuner: an open page fetches again only the files that changed.
            if (getenv("SOUNDGEN_SAY_ENCODED")) fprintf(stderr, "soundgen: encoded %s.m4a\n", e[i].name);
            const int pid = m4a_encode(w, m, e[i].rate, e[i].kbps);
            if (pid < 0) { fprintf(stderr, "soundgen: cannot run ffmpeg (it encodes the shipped AAC: install it)\n"); return 1; }
            pids[live] = pid; at[live] = i; live++; i++;
        }
        if (live == 0) break;
        if (m4a_wait(pids[0]) != 0) {
            char m[1024];
            snprintf(m, sizeof m, "%s/%s.m4a", outdir, e[at[0]].name);
            remove(m);
            fprintf(stderr, "soundgen: ffmpeg failed on %s\n", e[at[0]].name); bad++;
        }
        for (int k = 1; k < live; k++) { pids[k - 1] = pids[k]; at[k - 1] = at[k]; }
        live--;
    }
    if (getenv("SOUNDGEN_SAY_ENCODED")) fprintf(stderr, "soundgen: %d of %d files encoded\n", done, n);
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

// ---- recordings: a sound replaced by a recording -----------------------------------
// A RECORDING IS A TAKE SOUNDGEN DOES NOT SYNTHESISE. recordings/recordings.tsv
// lists them, a line each (# starts a comment):
//
//   <cue> <role> <level> <file> <anchor ms> <kbps>
//   place main 0 place_rec_0.wav 12.5 96
//
// <cue> is the cue's name as RECIPES spells it, <role> main, tight or prelude,
// <level> a prelude's stroke count (0 otherwise), <file> a 16-bit mono PCM WAV
// beside the list at its own rate, named <cue>_<anything>.wav, and <anchor ms>
// how far into it the cue's moment is. Each is a take of its cue like any other:
// written, encoded, checked and listed with the rest, in the table every host
// plays by (ctable) and the files they serve and bundle. A cue with a recording
// plays its recordings only: its synthesised takes are not written. The mix
// (MIX_ROWS) is still its row. SOUNDGEN_RECORDINGS names another directory.
#define REC_MAX 64
static int le16(const unsigned char *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
// A 16-bit mono PCM WAV into o (its samples, rate and length); 0 or -1.
static int read_wav(const char *path, Out *o) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)size);
    const int got = b && fread(b, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    int rate = 0, fmt_ok = 0, rc = -1;
    if (got && size >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) {
        for (long at = 12; at + 8 <= size; ) {
            const uint32_t len = le32(b + at + 4);
            const unsigned char *c = b + at + 8;
            if (at + 8 + (long)len > size) break;
            if (!memcmp(b + at, "fmt ", 4) && len >= 16) {
                fmt_ok = le16(c) == 1 && le16(c + 2) == 1 && le16(c + 14) == 16;
                rate = (int)le32(c + 4);
            } else if (!memcmp(b + at, "data", 4) && fmt_ok && rate >= 8000 && rate <= 48000) {
                o->n = (int)(len / 2);
                o->rate = rate;
                o->pcm = malloc(sizeof(int16_t) * (size_t)(o->n ? o->n : 1));
                for (int k = 0; k < o->n; k++) o->pcm[k] = (int16_t)le16(c + 2 * k);
                rc = o->n > 0 ? 0 : -1;
                break;
            }
            at += 8 + len + (len & 1);
        }
    }
    free(b);
    return rc;
}
// Every recording the list names, in its order; how many, or -1 for a bad line or file.
static int read_recordings(Out *recs) {
    const char *dir = getenv("SOUNDGEN_RECORDINGS");
    if (!dir || !*dir) dir = "recordings";
    char path[1024], line[512];
    snprintf(path, sizeof path, "%s/recordings.tsv", dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int n = 0, bad = 0;
    for (int ln = 1; fgets(line, sizeof line, f); ln++) {
        char cue[32], role[16], file[256];
        int level, kbps;
        double anchor;
        const char *t = line;
        while (*t == ' ' || *t == '\t') t++;
        if (*t == '#' || *t == '\n' || *t == 0) continue;
        int c = 0, r = -1;
        if (sscanf(t, "%31s %15s %d %255s %lf %d", cue, role, &level, file, &anchor, &kbps) != 6 || n >= REC_MAX) c = -1;
        for (int k = 1; c == 0 && k < ANIM_N_CUES; k++) if (!strcmp(RECIPES[k].name, cue)) c = k;
        for (int k = 0; k < 3; k++) if (!strcmp(ROLE_NAME[k], role)) r = k;
        const size_t stem = strlen(file) > 4 && !strcmp(file + strlen(file) - 4, ".wav") ? strlen(file) - 4 : 0;
        if (c <= 0 || c == ANIM_CUE_REVERT || r < 0 || level < 0 || level > MAX_LEVEL || (r == ROLE_PRELUDE) != (level > 0)
            || kbps < 32 || kbps > 320 || !stem || stem >= sizeof recs[n].name
            || strncmp(file, cue, strlen(cue)) || file[strlen(cue)] != '_') {
            fprintf(stderr, "soundgen: %s line %d: want <cue> <main|tight|prelude> <level> <cue>_<name>.wav <anchor ms> <kbps>\n", path, ln);
            bad++; continue;
        }
        Out *o = &recs[n];
        char wav[1400];
        snprintf(wav, sizeof wav, "%s/%s", dir, file);
        if (read_wav(wav, o) != 0) { fprintf(stderr, "soundgen: %s is not a 16-bit mono PCM WAV at 8-48 kHz\n", wav); bad++; continue; }
        memcpy(o->name, file, stem); o->name[stem] = 0;
        o->cue = c; o->role = r; o->level = level; o->anchor_ms = anchor; o->kbps = kbps;
        n++;
    }
    fclose(f);
    return bad ? -1 : n;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "parts") && argc > 2) return parts(argv[2]);
    if (!strcmp(mode, "encode") && argc > 3) return encode(argv[2], argv[3]);
    if (!strcmp(mode, "check") && argc > 3) return check(argv[2], argv[3]);
    const int writing = !strcmp(mode, "write") && argc > 2;
    const int ctable = !strcmp(mode, "ctable") && argc > 2;
    const int stats = !strcmp(mode, "stats");
    const int solo = !strcmp(mode, "solo") && argc > 6;   // [7]: a prelude's strokes
    if (!writing && !stats && !ctable && !solo && strcmp(mode, "list")) {
        fprintf(stderr, "usage: soundgen write <wavdir> [<cue_takes.c>] | encode <wavdir> <outdir>\n"
                        "                | check <wavdir> <outdir> | ctable <cue_takes.c> | list | stats | parts <dir>\n"
                        "                | solo <cue> <role> <part> <take> <out.wav> [<strokes>]\n");
        return 2;
    }
    if (read_tuning() != 0) return 1;
    Out *outs = calloc(512, sizeof(Out));
    Out recs[REC_MAX];
    memset(recs, 0, sizeof recs);
    const int n_recs = read_recordings(recs);
    if (n_recs < 0) return 1;
    int n = 0, nj = 0;
    long total = 0;
    double loudest = 0;
    Job *jobs = calloc(512, sizeof(Job));
    for (int cue = 1; cue < ANIM_N_CUES; cue++) {
        const Recipe *r = &RECIPES[cue];
        int recorded = 0;
        for (int i = 0; i < n_recs; i++) {
            if (recs[i].cue != cue) continue;
            outs[n] = recs[i];
            total += 44 + 2L * outs[n].n;
            n++; recorded++;
        }
        if (recorded) continue;
        if (!r->main.make) {
            if (render_placeholder(cue, 0, &outs[n])) { total += 44 + 2L * outs[n].n; n++; }
            continue;
        }
        const Set *sets[3] = { &r->main, &r->tight, &r->prelude };
        for (int role = ROLE_MAIN; role <= ROLE_PRELUDE; role++) {
            const Set *set = sets[role];
            if (!set->make) continue;
            for (int level = set->lo; level <= set->hi; level++) {
                for (int v = 0; v < set->takes; v++) jobs[nj++] = (Job){ cue, role, level, v, set, &outs[n++] };
            }
        }
    }
    if (render_all(jobs, nj) != 0) return 1;
    for (int j = 0; j < nj; j++) total += 44 + 2L * jobs[j].o->n;
    free(jobs);
    const double gain = common_gain(outs, n, writing || stats);
    // `solo <cue> <role> <part> <take> <out.wav> [<strokes>]` (the tuner's per-part Play):
    // that take of the part's set rendered again with only the part sounding, at the common
    // gain of the whole set, so it is heard at the level it has in the game. A prelude's is
    // its <strokes>-stroke take (3 when none is given). Nothing else is written.
    if (solo) {
        for (int i = 0; i < N_TUNE; i++)
            if (!strcmp(RECIPES[TUNE_KEY[i].cue].name, argv[2]) && !strcmp(ROLE_NAME[TUNE_KEY[i].role], argv[3])
                && !strcmp(TUNE_KEY[i].part, argv[4])) SOLO = i;
        if (SOLO < 0) { fprintf(stderr, "soundgen: no part %s %s %s\n", argv[2], argv[3], argv[4]); return 1; }
        const int cue = TUNE_KEY[SOLO].cue, role = TUNE_KEY[SOLO].role;
        const Set *sets[3] = { &RECIPES[cue].main, &RECIPES[cue].tight, &RECIPES[cue].prelude };
        const Set *set = sets[role];
        const int want = argc > 7 ? atoi(argv[7]) : 3;
        const int v = atoi(argv[5]), level = role == ROLE_PRELUDE ? (want < set->lo ? set->lo : want > set->hi ? set->hi : want) : set->lo;
        if (!set->make || v < 0 || v >= set->takes) { fprintf(stderr, "soundgen: no take %d of %s %s\n", v, argv[2], argv[3]); return 1; }
        Out o;
        memset(&o, 0, sizeof o);
        Job j = { cue, role, level, v, set, &o };
        if (render_jobs(&j, 1) != 0) return 1;
        int16_t *pcm = malloc(sizeof(int16_t) * (size_t)(o.n ? o.n : 1));
        for (int k = 0; k < o.n; k++) pcm[k] = to16(o.x[k] * gain);
        if (write_pcm(argv[6], pcm, o.n, o.rate) != 0) { fprintf(stderr, "soundgen: cannot write %s\n", argv[6]); return 1; }
        return 0;
    }
    for (int i = 0; i < n; i++) if (outs[i].x) for (int k = 0; k < outs[i].n; k++) if (abs(outs[i].pcm[k]) / 32767.0 > loudest) loudest = abs(outs[i].pcm[k]) / 32767.0;
    FILE *manifest = NULL;
    if (writing) {
        char path[1024];
        snprintf(path, sizeof path, "%s/" MANIFEST, argv[2]);
        if (!(manifest = fopen(path, "w"))) { fprintf(stderr, "soundgen: cannot write %s\n", path); return 1; }
    }
    for (int i = 0; i < n && !ctable; i++) {
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
    // `write <wavdir> <ctable>` writes the take table from the same render (the tuner's rebuild).
    const char *ctable_path = ctable ? argv[2] : writing && argc > 3 ? argv[3] : NULL;
    if (ctable_path && write_ctable(ctable_path, outs, n) != 0) { fprintf(stderr, "soundgen: cannot write %s\n", ctable_path); return 1; }
    // `write <wavdir> <ctable> <table.bin>`: the same table as bytes, for the tuner.
    if (writing && argc > 4 && write_ctable_bin(argv[4], outs, n) != 0) { fprintf(stderr, "soundgen: cannot write %s\n", argv[4]); return 1; }
    if (writing || stats)
        fprintf(writing ? stderr : stdout, "soundgen: %d files, %ld bytes, loudest card take %.1f dBFS\n",
                n, total, 20 * log10(loudest + 1e-9));
    if (loudest > db2(PEAK_DB) * 1.001) { fprintf(stderr, "soundgen: a card take is over PEAK_DB\n"); return 1; }
    return 0;
}
