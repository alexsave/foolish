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
// one constant, LIFT_DB, so the loudest take peaks near full scale. Balancing
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

#define S_(sec) ((int)((sec) * SR))
static double db2(double db) { return pow(10, db / 20); }

// The whole set up by this much from the models' deal scale (g = -34..-32
// dBFS), which was a listening level, not a file level: the loudest take then
// peaks at about -3 dBFS. One number for every cue, so the balance between
// them is the models' own.
#define LIFT_DB 32.0
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
static void put_peak(Take *t, Sig part, int ref, double at_s, double db) {
    const double k = db2(db + LIFT_DB) / (sig_peak(part.x, part.n) + 1e-12);
    const int o = CUE0 + S_(at_s) - ref;
    for (int i = 0; i < part.n; i++) if (o + i >= 0 && o + i < SPAN) t->mix.x[o + i] += part.x[i] * k;
    sig_free(&part);
}
// ...or with its loudest 50 ms RMS (150 Hz-15 kHz) at `db` (disc_game.py's put()).
static void put_rms50(Take *t, Sig part, int ref, double at_s, double db) {
    Sig b = bp_copy(&part, 150, 15000);
    const double m = max_moving_rms(&b, 2400, 0, b.n);
    sig_free(&b);
    const double k = db2(db + LIFT_DB) / (m + 1e-12);
    const int o = CUE0 + S_(at_s) - ref;
    for (int i = 0; i < part.n; i++) if (o + i >= 0 && o + i < SPAN) t->mix.x[o + i] += part.x[i] * k;
    sig_free(&part);
}

// ---- the gestures: each *_game.py at the kernel's moments -------------------------
// DEAL (deal/an/deal_game.py): the thumb slide's peak 130 ms before the grab,
// the grab at the moment the card leaves the stock.
static void g_deal(Take *t, int level) {
    (void)level;
    Sig s = m_thumb(&t->r); put_peak(t, s, sig_argmax_abs(&s), -0.13, t->g - uni(&t->r, 6, 9));
    Sig g = m_grab(&t->r); put_peak(t, g, sig_argmax_abs(&g), 0, t->g - 5.0);
}
// ...and TIGHT, for a card dealt with no time before it: the slide's peak 50 ms
// before the grab (its swell still before that), so played late from the
// moment it is over a sixth of a second sooner.
static void g_deal_tight(Take *t, int level) {
    (void)level;
    Sig s = m_thumb(&t->r); put_peak(t, s, sig_argmax_abs(&s), -0.05, t->g - uni(&t->r, 6, 9));
    Sig g = m_grab(&t->r); put_peak(t, g, sig_argmax_abs(&g), 0, t->g - 5.0);
}
// DEAL_LAND: the card on the cloth, land229, quietest of the three. At the
// viewer's own seat; the host scales it by the seat (anim_seat_gain_pm).
static void g_deal_land(Take *t, int level) {
    (void)level;
    Sig l = m_land(&t->r); put_peak(t, l, sig_argmax_abs(&l), 0, t->g - uni(&t->r, 13, 16));
}
// TURN (flip/an/flip_game.py): the trump flipped, its snap as it leaves the stock.
static void g_turn(Take *t, int level) {
    (void)level;
    Sig f = m_flip(&t->r); put_peak(t, f, S_(FLIP_REF_S), 0, t->g - 5.0);
}
// FLIP: the trump landing (a throw-in, Alex: "similar to a throw in"), then the
// stock set down on it 350 ms later ("350 ms after trump lands is fine"). Baked
// into one file because nothing on screen moves at the set-down: there is no
// moment for the kernel to name, only the sound's own rhythm.
static void g_flip_land(Take *t, int level) {
    (void)level;
    Sig l = m_throw(&t->r); put_peak(t, l, sig_argmax_abs(&l), 0, t->g - uni(&t->r, 9, 12));
    Sig d = m_deck(&t->r); put_peak(t, d, S_(DECK_REF_S), 0.35, t->g - uni(&t->r, 3, 6));
}
// LIFT (throwin/an/throwin_game.py): two fingers take the card, the thumb slides
// it out, it leaves: the grab 110 ms before the slide's peak, the slide's peak
// 130 ms before the card leaves. The same for an attack, a cover and a pass.
static void g_lift(Take *t, int level) {
    (void)level;
    Sig s = m_thumb(&t->r); put_peak(t, s, sig_argmax_abs(&s), -0.13, t->g - uni(&t->r, 6, 9));
    Sig g = m_tgrab(&t->r); put_peak(t, g, S_(TGRAB_REF_S), -0.24, t->g - 14.0);
}
// ...and TIGHT (Alex: "maybe put them closer to each other"), for a move that
// opens its run and so has no time before its lift: the grab 60 ms before the
// slide's peak and the peak 30 ms before the card leaves, where they were 110
// and 130 ms. The host plays it from the moment, whole.
static void g_lift_tight(Take *t, int level) {
    (void)level;
    Sig s = m_thumb(&t->r); put_peak(t, s, sig_argmax_abs(&s), -0.03, t->g - uni(&t->r, 6, 9));
    Sig g = m_tgrab(&t->r); put_peak(t, g, S_(TGRAB_REF_S), -0.09, t->g - 14.0);
}
// PLACE: the throw-in's landing.
static void g_place(Take *t, int level) {
    (void)level;
    Sig l = m_throw(&t->r); put_peak(t, l, sig_argmax_abs(&l), 0, t->g - uni(&t->r, 3, 5));
}
// COVER: the cover's landing, on a card.
static void g_cover(Take *t, int level) {
    (void)level;
    Sig l = m_cover(&t->r); put_peak(t, l, sig_argmax_abs(&l), 0, t->g - uni(&t->r, 4, 6));
}
// PASS (pass/an/pass_game.py): struck at the transfer's landing, beside its
// PLACE; the "shfff" starts 150 ms after it.
static void g_pass(Take *t, int level) {
    (void)level;
    Sig p = m_pass(&t->r); put_peak(t, p, S_(PASS_REF_S), 0.15, t->g - uni(&t->r, 11, 13));
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
        put_rms50(t, y, S_(STROKE_PRE_S), at, sc - 1.5 + 3.0 * s / (count > 1 ? count - 1 : 1) - uni(&t->r, 0, 1.5));
        at += d[s] + gp[s];
    }
}
// SWEEP: the pile lifted and turned, as the sweep opens (its strokes are the
// prelude below). The levels are disc_game.py's: the strokes at g-30.5, the
// lift 5 dB over them.
static void g_sweep(Take *t, int level) {
    (void)level;
    Sig l = m_lift(&t->r); put_rms50(t, l, S_(EVENT_PRE_S), 0, t->g - 30.5 + 5);
}
// ...its prelude: `level` strokes, the last 100-180 ms before the lift.
static void g_sweep_strokes(Take *t, int level) { strokes(t, level, uni(&t->r, 0.10, 0.18), t->g - 30.5); }
// PILE: the swept cards set softly on the discard pile, as they land.
static void g_pile(Take *t, int level) {
    (void)level;
    Sig p = m_place(&t->r); put_rms50(t, p, S_(EVENT_PRE_S), 0, t->g - 30.5 + 3);
}
// PICKUP: the one soft lift, as the table lifts, 10 dB under the strokes.
static void g_pickup(Take *t, int level) {
    (void)level;
    Sig l = m_plift(&t->r); put_rms50(t, l, S_(EVENT_PRE_S), 0, t->g - 30.5 - 10);
}
// ...its prelude: `level` strokes, the last 50-120 ms before the lift.
static void g_pickup_strokes(Take *t, int level) { strokes(t, level, uni(&t->r, 0.05, 0.12), t->g - 30.5); }
// INTO_HAND: the cards slid into the fanned hand as they land, its onset there.
static void g_into_hand(Take *t, int level) {
    (void)level;
    Sig s = m_insert(&t->r); put_rms50(t, s, S_(STROKE_PRE_S), 0, t->g - 30.5 - 11);
}

// ---- the table: what every cue is written as ------------------------------------
// `name` is the file stem and the kernel's own name for the cue in lower case.
// `rate`: 48 kHz where the deal's thumb slide is in the take (its recordings
// reach 20 kHz and a third of its energy is over 16 kHz), 32 kHz everywhere else
// (every other model cuts at 15 kHz, so 32 kHz holds all of it).
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
} Set;
typedef struct {
    const char *name;
    int         rate;
    Set         main, tight, prelude;
    double      cards_per_level;
} Recipe;
#define ONE(f, n) { f, n, 0, 0 }
#define NONE      { NULL, 0, 0, 0 }

static const Recipe RECIPES[] = {
    [ANIM_CUE_NONE]      = { "none",      0,     NONE, NONE, NONE, 0 },
    [ANIM_CUE_DEAL]      = { "deal",      48000, ONE(g_deal, 8), ONE(g_deal_tight, 4), NONE, 0 },
    [ANIM_CUE_FLIP]      = { "flip",      32000, ONE(g_flip_land, 8), NONE, NONE, 0 },
    [ANIM_CUE_LIFT]      = { "lift",      48000, ONE(g_lift, 8), ONE(g_lift_tight, 4), NONE, 0 },
    [ANIM_CUE_PLACE]     = { "place",     32000, ONE(g_place, 8), NONE, NONE, 0 },
    [ANIM_CUE_COVER]     = { "cover",     32000, ONE(g_cover, 8), NONE, NONE, 0 },
    [ANIM_CUE_PICKUP]    = { "pickup",    32000, ONE(g_pickup, 8), NONE, { g_pickup_strokes, 3, 1, MAX_LEVEL }, 2.5 },
    [ANIM_CUE_SWEEP]     = { "sweep",     32000, ONE(g_sweep, 8), NONE, { g_sweep_strokes, 3, 1, MAX_LEVEL }, 2.5 },
    [ANIM_CUE_PILE]      = { "pile",      32000, ONE(g_pile, 8), NONE, NONE, 0 },
    [ANIM_CUE_OUT]       = { "out",       0,     NONE, NONE, NONE, 0 },
    [ANIM_CUE_REVERT]    = { "revert",    0,     NONE, NONE, NONE, 0 },
    [ANIM_CUE_GOOD]      = { "good",      0,     NONE, NONE, NONE, 0 },
    [ANIM_CUE_HAND_OFF]  = { "hand_off",  0,     NONE, NONE, NONE, 0 },
    [ANIM_CUE_FINISH]    = { "finish",    0,     NONE, NONE, NONE, 0 },
    [ANIM_CUE_FOOL]      = { "fool",      0,     NONE, NONE, NONE, 0 },
    [ANIM_CUE_DEAL_LAND] = { "deal_land", 32000, ONE(g_deal_land, 8), NONE, NONE, 0 },
    [ANIM_CUE_TURN]      = { "turn",      32000, ONE(g_turn, 8), NONE, NONE, 0 },
    [ANIM_CUE_PASS]      = { "pass",      32000, ONE(g_pass, 8), NONE, NONE, 0 },
    [ANIM_CUE_INTO_HAND] = { "into_hand", 32000, ONE(g_into_hand, 8), NONE, NONE, 0 },
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

// ---- one rendered file -------------------------------------------------------------
typedef struct {
    char   name[48];
    int    cue, level, role, rate, n;
    double anchor_ms;      // from the file's first sample to the cue's moment
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
    o->pcm = malloc(sizeof(int16_t) * (size_t)sh.n);
    for (int i = 0; i < sh.n; i++) o->pcm[i] = to16(sh.x[i]);
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
    if (role == ROLE_PRELUDE) snprintf(o->name, sizeof o->name, "%s_strokes_s%d_%d.wav", r->name, level, v);
    else if (role == ROLE_TIGHT) snprintf(o->name, sizeof o->name, "%s_tight_%d.wav", r->name, v);
    else snprintf(o->name, sizeof o->name, "%s_%d.wav", r->name, v);
    o->cue = cue; o->level = level; o->role = role;
}

static int render_placeholder(int cue, int v, Out *o) {
    for (int i = 0; i < N_PLACEHOLDERS; i++) {
        if (PLACEHOLDERS[i].cue != cue) continue;
        const float *x; int n;
        placeholder_render(PLACEHOLDERS[i].make, PLACEHOLDERS[i].rate, cue, v, 1, &x, &n);
        o->pcm = malloc(sizeof(int16_t) * (size_t)n);
        for (int k = 0; k < n; k++) o->pcm[k] = to16(x[k]);
        o->n = n; o->rate = PLACEHOLDERS[i].rate; o->anchor_ms = 0; o->cue = cue; o->level = 0; o->role = ROLE_MAIN;
        snprintf(o->name, sizeof o->name, "%s_%d.wav", RECIPES[cue].name, v);
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
            fprintf(f, "        { file: 'gen/%s', anchorMs: %.1f, role: '%s', level: %d },\n", outs[i].name, outs[i].anchor_ms,
                    ROLE_NAME[outs[i].role], outs[i].level);
            if (outs[i].anchor_ms > lead) lead = outs[i].anchor_ms;
        }
        fprintf(f, "    ],\n");
    }
    fprintf(f, "};\n\n/** How many cards one level (one stroke) of a levelled cue gathers. */\n"
               "export const CUE_CARDS_PER_LEVEL: Readonly<Record<number, number>> = {\n");
    for (int c = 1; c < ANIM_N_CUES; c++)
        if (RECIPES[c].prelude.make) fprintf(f, "    [%s]: %.1f,\n", CUE_CONST[c], RECIPES[c].cards_per_level);
    fprintf(f, "};\n\n/** The longest anchor of all: how early the earliest file must start. */\n"
               "export const CUE_MAX_ANCHOR_MS = %.1f;\n", lead);
    return fclose(f);
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "parts") && argc > 2) return parts(argv[2]);
    const int writing = !strcmp(mode, "write") && argc > 3;
    const int stats = !strcmp(mode, "stats");
    if (!writing && !stats && strcmp(mode, "list")) {
        fprintf(stderr, "usage: soundgen write <dir> <cueTakes.ts> | list | stats | parts <dir>\n");
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
                    for (int i = 0; i < o->n; i++) if (abs(o->pcm[i]) / 32767.0 > loudest) loudest = abs(o->pcm[i]) / 32767.0;
                }
            }
        }
    }
    for (int i = 0; i < n; i++) {
        const Out *o = &outs[i];
        if (writing) {
            char path[1024];
            snprintf(path, sizeof path, "%s/%s", argv[2], o->name);
            if (write_pcm(path, o->pcm, o->n, o->rate) != 0) { fprintf(stderr, "soundgen: cannot write %s\n", path); return 1; }
        } else if (stats) {
            double sq = 0; int pk = 0;
            for (int k = 0; k < o->n; k++) { sq += (double)o->pcm[k] * o->pcm[k]; if (abs(o->pcm[k]) > pk) pk = abs(o->pcm[k]); }
            printf("%-18s cue %2d  %5d Hz  %5d ms  %7ld B  anchor %7.1f ms  peak %6.1f dBFS  rms %6.1f dBFS\n",
                   o->name, o->cue, o->rate, o->n * 1000 / o->rate, 44 + 2L * o->n, o->anchor_ms,
                   20 * log10(pk / 32767.0 + 1e-9), 10 * log10(sq / (o->n ? o->n : 1) / (32767.0 * 32767.0) + 1e-12));
        } else {
            printf("%s\t%d\t%d\t%d\t%ld\t%.1f\t%s\t%d\n", o->name, o->cue, o->rate, o->n * 1000 / o->rate, 44 + 2L * o->n,
                   o->anchor_ms, ROLE_NAME[o->role], o->level);
        }
    }
    if (writing && write_table(argv[3], outs, n) != 0) { fprintf(stderr, "soundgen: cannot write %s\n", argv[3]); return 1; }
    if (writing || stats)
        fprintf(writing ? stderr : stdout, "soundgen: %d files, %ld bytes, loudest card take %.1f dBFS\n",
                n, total, 20 * log10(loudest + 1e-9));
    if (loudest > 0.999) { fprintf(stderr, "soundgen: a card take clips; lower LIFT_DB\n"); return 1; }
    return 0;
}
