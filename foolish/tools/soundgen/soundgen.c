// soundgen - the PLACEHOLDER sounds, synthesised.
//
// Every sound cue the kernel's animation plan can make (c/src/anim_plan.h
// "sound cues") needs something to be heard as before anybody has recorded a
// card. This writes one small WAV per cue (two for the ones heard most), from
// nothing but filtered noise, short sines and envelopes, so the game is audible
// end to end and the owner has something to dislike precisely.
//
// THESE FILES ARE STAND-INS, AND THE HOSTS DO NOT KNOW THAT. A host plays a
// file its cue table names (src/audio/cueSounds.ts); whether a human recorded
// it or this program wrote it is not something the playback path can tell,
// which is the whole point: replacing a placeholder with a recording is naming
// a different file in that table and touches no logic. docs/AUDIO.md.
//
// NOTHING IT WRITES IS COMMITTED. The output is a build artifact, like the
// generated layouts and the wasm: `npm run sounds` writes public/audio/gen/,
// which is ignored, and the hooks that build the site run it. A recording is
// the opposite - a source file, committed under public/audio/ - so the two can
// never be confused for each other on disk.
//
// TIED TO THE KERNEL'S CUE LIST BY THE COMPILER. The recipe table below is
// indexed by ANIM_CUE_* and sized by ANIM_N_CUES, so a cue added to
// anim_plan.h without a line here does not build (see the _Static_assert).
//
// DETERMINISTIC: the noise is a seeded xorshift and nothing reads a clock, so
// two runs on one machine write the same bytes. Across machines the bytes can
// differ in the last bit (libm's sin and exp are not bit-identical everywhere),
// which is one more reason the output is not committed.
//
//   soundgen write <dir>   write every file into <dir>
//   soundgen list          one line a file: name, cue id, rate, length, bytes
//   soundgen stats         the same, measured: peak, RMS and spectral centroid,
//                          the three numbers a recording is compared against
//                          when a recipe is tuned to replicate one
//
// HOW A SOUND IS BUILT. Four layers, added into one buffer:
//   burst   band-passed noise with a sharp attack and an exponential tail,
//           its band sweeping - paper leaving paper, a card's edge on felt
//   swell   the same noise under a slow rise and fall - cards sliding
//   thump   a short sine falling in pitch - the table answering a card
//   tone    a soft mallet note - the sounds that are not cards at all
// plus `tick`, two cycles of a windowed sine, for the first millisecond of a
// contact. A card sound is a tick and a burst over a thump; everything else is
// arithmetic on those.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anim_plan.h"

#define PI 3.14159265358979f
#define MAX_SAMPLES 48000   // two seconds at the highest rate used; nothing is near it

typedef struct {
    float    x[MAX_SAMPLES];
    int      n;      // samples in use
    int      rate;   // Hz
    uint32_t rng;
} Buf;

// ---- noise ----------------------------------------------------------------
static float noise(Buf *b) {
    uint32_t s = b->rng;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    b->rng = s;
    return (float)(int32_t)s / 2147483648.0f;
}

// ---- a band-pass, RBJ's, constant peak gain --------------------------------
typedef struct { float b0, b2, a1, a2, z1, z2; } Band;
static void band_set(Band *f, float hz, float q, int rate) {
    if (hz > 0.45f * rate) hz = 0.45f * rate;
    const float w = 2 * PI * hz / rate, alpha = sinf(w) / (2 * q), a0 = 1 + alpha;
    f->b0 = alpha / a0; f->b2 = -alpha / a0;
    f->a1 = -2 * cosf(w) / a0; f->a2 = (1 - alpha) / a0;
}
static float band_run(Band *f, float x) {
    const float y = f->b0 * x + f->z1;
    f->z1 = -f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

static int at(const Buf *b, float ms) { return (int)(ms * b->rate / 1000.0f); }

// ---- the layers -------------------------------------------------------------

// Band-passed noise from `at_ms`, `len_ms` long, the band's centre sliding
// f0 -> f1. The envelope is a fast rise (atk_ms) into an exponential tail
// (dec_ms): the shape of anything that is struck.
static void burst(Buf *b, float at_ms, float len_ms, float f0, float f1, float q,
                  float atk_ms, float dec_ms, float gain) {
    Band f = {0};
    const int i0 = at(b, at_ms), n = at(b, len_ms);
    const float atk = atk_ms * b->rate / 1000.0f, dec = dec_ms * b->rate / 1000.0f;
    for (int i = 0; i < n && i0 + i < MAX_SAMPLES; i++) {
        if ((i & 15) == 0) band_set(&f, f0 + (f1 - f0) * (float)i / (float)n, q, b->rate);
        const float env = (1 - expf(-(float)i / atk)) * expf(-(float)i / dec);
        b->x[i0 + i] += gain * env * band_run(&f, noise(b));
    }
    if (i0 + n > b->n) b->n = i0 + n < MAX_SAMPLES ? i0 + n : MAX_SAMPLES;
}

// The same noise under a slow rise to `peak` (0..1 of the way through) and a
// fall after it. `rough` (0..1) is how much a second, slow noise shakes the
// level: a slide over felt is not smooth, and one that is sounds like a hiss.
static void swell(Buf *b, float at_ms, float len_ms, float f0, float f1, float q,
                  float peak, float rough, float gain) {
    // Two bands in series: a slide has no top end to speak of, and one band's
    // skirts let enough of it through to turn felt into a hiss.
    Band f = {0}, g = {0}, r = {0};
    band_set(&r, 38.0f, 0.7f, b->rate);
    const int i0 = at(b, at_ms), n = at(b, len_ms);
    for (int i = 0; i < n && i0 + i < MAX_SAMPLES; i++) {
        if ((i & 15) == 0) {
            band_set(&f, f0 + (f1 - f0) * (float)i / (float)n, q, b->rate);
            g.b0 = f.b0; g.b2 = f.b2; g.a1 = f.a1; g.a2 = f.a2;
        }
        const float t = (float)i / (float)n;
        const float e = t < peak ? t / peak : (1 - t) / (1 - peak);
        const float shake = 1 + rough * 9.0f * band_run(&r, noise(b));
        b->x[i0 + i] += gain * e * e * (shake < 0 ? 0 : shake) * band_run(&g, band_run(&f, noise(b)));
    }
    if (i0 + n > b->n) b->n = i0 + n < MAX_SAMPLES ? i0 + n : MAX_SAMPLES;
}

// A sine falling f0 -> f1 under an exponential tail: the low end of a contact.
static void thump(Buf *b, float at_ms, float f0, float f1, float dec_ms, float gain) {
    const int i0 = at(b, at_ms), n = at(b, dec_ms * 5);
    const float dec = dec_ms * b->rate / 1000.0f;
    float ph = 0;
    for (int i = 0; i < n && i0 + i < MAX_SAMPLES; i++) {
        const float k = expf(-(float)i / dec);
        ph += 2 * PI * (f1 + (f0 - f1) * k) / b->rate;
        b->x[i0 + i] += gain * k * sinf(ph);
    }
    if (i0 + n > b->n) b->n = i0 + n < MAX_SAMPLES ? i0 + n : MAX_SAMPLES;
}

// Two cycles of a sine under a Hann window: the first millisecond of a touch.
static void tick(Buf *b, float at_ms, float hz, float gain) {
    const int i0 = at(b, at_ms), n = (int)(2.0f * b->rate / hz);
    for (int i = 0; i < n && i0 + i < MAX_SAMPLES; i++) {
        const float w = 0.5f - 0.5f * cosf(2 * PI * (float)i / (float)n);
        b->x[i0 + i] += gain * w * sinf(2 * PI * hz * (float)i / b->rate);
    }
    if (i0 + n > b->n) b->n = i0 + n < MAX_SAMPLES ? i0 + n : MAX_SAMPLES;
}

// A soft mallet note: a sine and a quiet third partial, a 2ms rise and an
// exponential tail. `bend` is how far the pitch has fallen by the end of the
// tail, as a fraction (0 for none) - the one thing that makes a note sag.
static void tone(Buf *b, float at_ms, float hz, float dec_ms, float bend, float gain) {
    const int i0 = at(b, at_ms), n = at(b, dec_ms * 4.5f);
    const float dec = dec_ms * b->rate / 1000.0f, atk = 0.002f * b->rate;
    float ph = 0;
    for (int i = 0; i < n && i0 + i < MAX_SAMPLES; i++) {
        const float k = expf(-(float)i / dec);
        ph += 2 * PI * hz * (1 - bend * (float)i / (float)n) / b->rate;
        const float env = (1 - expf(-(float)i / atk)) * k;
        b->x[i0 + i] += gain * env * (sinf(ph) + 0.18f * k * sinf(3 * ph));
    }
    if (i0 + n > b->n) b->n = i0 + n < MAX_SAMPLES ? i0 + n : MAX_SAMPLES;
}

// ---- the recipes ------------------------------------------------------------
//
// One function a cue. `v` is the variant (0, 1, ...) and `k` a factor near 1
// that differs between variants: every frequency is scaled by it, so two
// variants of one cue are the same gesture made by a slightly different hand.
// The hosts add their own small pitch and level jitter a play on top of this.

// A card off the stock: the edge dragging over the card under it, then free.
static void r_deal(Buf *b, int v, float k) {
    (void)v;
    burst(b, 0, 85, 2600 * k, 5200 * k, 0.9f, 6, 26, 1.0f);
    burst(b, 0, 60, 7000 * k, 8200 * k, 1.2f, 3, 13, 0.32f);
    tick(b, 1, 3200 * k, 0.22f);
}

// The trump turned up: the turn, then the card laid flat.
static void r_flip(Buf *b, int v, float k) {
    (void)v;
    swell(b, 0, 85, 2200 * k, 5200 * k, 0.8f, 0.6f, 0.2f, 0.5f);
    tick(b, 85, 2600 * k, 0.8f);
    burst(b, 85, 45, 3600 * k, 2400 * k, 0.8f, 0.6f, 10, 1.0f);
    thump(b, 85, 200 * k, 120 * k, 22, 0.5f);
}

// A card drawn out of a held fan: soft, no contact at all.
static void r_lift(Buf *b, int v, float k) {
    (void)v;
    burst(b, 0, 110, 1800 * k, 3400 * k, 0.7f, 26, 34, 1.0f);
}

// A card put down on the table: the snap, and the table under it.
static void r_place(Buf *b, int v, float k) {
    (void)v;
    tick(b, 0, 2400 * k, 0.9f);
    burst(b, 0, 45, 3800 * k, 2600 * k, 0.8f, 0.6f, 9, 1.0f);
    burst(b, 4, 60, 900 * k, 700 * k, 1.0f, 1, 17, 0.35f);
    thump(b, 0, 190 * k, 110 * k, 24, 0.75f);
}

// A card put down on a card: two contacts a few milliseconds apart (card on
// card, then the pair on the table), brighter and with less table in it.
static void r_cover(Buf *b, int v, float k) {
    (void)v;
    tick(b, 0, 3000 * k, 0.7f);
    burst(b, 0, 40, 4200 * k, 3000 * k, 0.9f, 0.5f, 8, 1.0f);
    tick(b, 7, 2200 * k, 0.5f);
    burst(b, 7, 50, 2400 * k, 1800 * k, 0.8f, 0.8f, 12, 0.7f);
    thump(b, 7, 160 * k, 105 * k, 19, 0.4f);
}

// The table gathered into a hand: one slide with the cards catching on each
// other along it, squared up at the end.
static void r_pickup(Buf *b, int v, float k) {
    swell(b, 0, 300, 1500 * k, 2600 * k, 0.6f, 0.45f, 0.4f, 0.4f);
    for (int i = 0; i < 5; i++) {
        const float t = 15 + i * 52 + 9 * noise(b) + 4 * v;
        burst(b, t, 70, (2000 + 300 * i) * k, 3400 * k, 0.8f, 7, 19, 0.55f + 0.1f * noise(b));
        tick(b, t, 2600 * k, 0.2f);
    }
    thump(b, 285, 170 * k, 110 * k, 20, 0.3f);
}

// The table swept off to the pile: a long slide over felt, nothing struck.
static void r_sweep(Buf *b, int v, float k) {
    (void)v;
    swell(b, 0, 360, 1100 * k, 2600 * k, 0.6f, 0.4f, 0.5f, 1.0f);
    swell(b, 40, 280, 5000 * k, 6500 * k, 1.0f, 0.4f, 0.3f, 0.22f);
}

// The swept cards coming to rest on the pile: soft and low, a thud with a
// little paper on top of it.
static void r_pile(Buf *b, int v, float k) {
    (void)v;
    thump(b, 0, 120 * k, 70 * k, 30, 1.0f);
    burst(b, 0, 70, 700 * k, 450 * k, 0.7f, 1.5f, 22, 0.6f);
    burst(b, 0, 40, 3200 * k, 2400 * k, 0.9f, 1, 9, 0.3f);
    tick(b, 0, 1800 * k, 0.25f);
}

// A seat out of the game: two soft notes stepping down. Not a card sound,
// because it is not a card moving - it is a fact about a player.
static void r_out(Buf *b, int v, float k) {
    (void)v;
    tone(b, 0, 587.33f * k, 60, 0, 0.8f);
    tone(b, 130, 392.00f * k, 42, 0, 1.0f);
}

// A good said: two small bright notes, the second a fifth up. Agreement.
static void r_good(Buf *b, int v, float k) {
    (void)v;
    tone(b, 0, 1046.50f * k, 34, 0, 0.8f);
    tone(b, 70, 1567.98f * k, 42, 0, 1.0f);
}

// The shield or the sword crossing the table: air, then it is set down.
static void r_hand_off(Buf *b, int v, float k) {
    (void)v;
    swell(b, 0, 230, 700 * k, 2200 * k, 0.7f, 0.55f, 0.2f, 1.0f);
    thump(b, 215, 150 * k, 95 * k, 14, 0.5f);
    burst(b, 215, 40, 1500 * k, 1000 * k, 0.8f, 1, 10, 0.35f);
}

// The game over, and not for the fool: four notes up a major chord.
static void r_finish(Buf *b, int v, float k) {
    (void)v;
    tone(b, 0, 523.25f * k, 50, 0, 0.7f);
    tone(b, 105, 659.25f * k, 50, 0, 0.8f);
    tone(b, 210, 783.99f * k, 55, 0, 0.9f);
    tone(b, 315, 1046.50f * k, 72, 0, 1.0f);
}

// ...and for the fool: four notes down by semitones, the last one sagging.
static void r_fool(Buf *b, int v, float k) {
    (void)v;
    tone(b, 0, 392.00f * k, 55, 0, 0.9f);
    tone(b, 160, 369.99f * k, 55, 0, 0.9f);
    tone(b, 320, 349.23f * k, 55, 0, 0.9f);
    tone(b, 480, 329.63f * k, 60, 0.06f, 1.0f);
}

// What a cue is written as. `name` is the file's stem and is the kernel's own
// name for the cue in lower case, so a file can be found from a cue id with no
// table in between. `rate` is per recipe because a sample is two bytes: the
// card sounds need the top octave and the rest do not. The rate is the lowest
// that still holds the recipe's highest partial under Nyquist: 8 kHz for the
// notes that top out near 3.1 kHz, 12 kHz for `good` (4.7 kHz) and for the two
// sounds that are all low end (the pile's thud, the hand-off's air).
//
// A NULL `make` IS A CUE WITH NO PLACEHOLDER. REVERT is one for good: it has
// no sound of its own, a host plays the undone cue's sound backwards
// (anim_plan.h ANIM_CUE_REVERT). A cue whose row in the hosts' table now names
// a recording becomes one too.
typedef struct {
    const char *name;
    void      (*make)(Buf *, int v, float k);
    int         variants;
    int         rate;
} Recipe;

static const Recipe RECIPES[] = {
    [ANIM_CUE_DEAL]     = { "deal",     r_deal,     2, 24000 },
    [ANIM_CUE_FLIP]     = { "flip",     r_flip,     1, 24000 },
    [ANIM_CUE_LIFT]     = { "lift",     r_lift,     1, 24000 },
    [ANIM_CUE_PLACE]    = { "place",    r_place,    2, 24000 },
    [ANIM_CUE_COVER]    = { "cover",    r_cover,    2, 24000 },
    [ANIM_CUE_PICKUP]   = { "pickup",   r_pickup,   1, 24000 },
    [ANIM_CUE_SWEEP]    = { "sweep",    r_sweep,    1, 24000 },
    [ANIM_CUE_PILE]     = { "pile",     r_pile,     1, 12000 },
    [ANIM_CUE_OUT]      = { "out",      r_out,      1,  8000 },
    [ANIM_CUE_REVERT]   = { "revert",   NULL,       0,     0 },
    [ANIM_CUE_GOOD]     = { "good",     r_good,     1, 12000 },
    [ANIM_CUE_HAND_OFF] = { "hand_off", r_hand_off, 1, 12000 },
    [ANIM_CUE_FINISH]   = { "finish",   r_finish,   1,  8000 },
    [ANIM_CUE_FOOL]     = { "fool",     r_fool,     1,  8000 },
};
// A cue added to anim_plan.h grows ANIM_N_CUES, and this stops building until
// it has a line above (a cue that is meant to be silent gets a NULL `make`).
_Static_assert(sizeof RECIPES / sizeof RECIPES[0] == ANIM_N_CUES,
               "every sound cue of anim_plan.h has a recipe line");

// ---- finishing: what every sound gets, whatever it is ----------------------
//
// The DC is taken out (a thump leaves some, and DC is a click at both ends),
// the last few milliseconds are faded to nothing, trailing near-silence is cut
// (it is bytes), and the whole thing is scaled to one peak level, so that how
// LOUD a cue is in the game is decided in exactly one place: the hosts' table.
#define PEAK 0.89f

static void finish(Buf *b) {
    float dc = 0, prev = 0;
    for (int i = 0; i < b->n; i++) {            // one-pole high-pass at ~25 Hz
        const float x = b->x[i];
        dc = x - prev + (1 - 2 * PI * 25.0f / b->rate) * dc;
        prev = x;
        b->x[i] = dc;
    }
    float peak = 0;
    for (int i = 0; i < b->n; i++) if (fabsf(b->x[i]) > peak) peak = fabsf(b->x[i]);
    if (peak <= 0) return;
    while (b->n > 1 && fabsf(b->x[b->n - 1]) < 0.004f * peak) b->n--;
    const int fade = at(b, 6) < b->n ? at(b, 6) : b->n;
    for (int i = 0; i < fade; i++) b->x[b->n - 1 - i] *= (float)i / (float)fade;
    for (int i = 0; i < b->n; i++) b->x[i] *= PEAK / peak;
}

static Buf g_buf;

static void render(const Recipe *r, int cue, int v) {
    memset(&g_buf, 0, sizeof g_buf);
    g_buf.rate = r->rate;
    g_buf.rng = 0x9E3779B9u ^ (uint32_t)(cue * 7919 + v * 104729 + 1);
    // Variants straddle 1: -4% and +4% for two, nothing for one.
    const float k = 1 + 0.08f * ((float)v - (float)(r->variants - 1) / 2);
    r->make(&g_buf, v, k);
    finish(&g_buf);
}

// ---- WAV: 16-bit PCM, mono, the 44-byte header and nothing else ------------
static void le(FILE *f, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; i++) fputc((int)((v >> (8 * i)) & 0xff), f);
}
static int write_wav(const char *path, const Buf *b) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    const uint32_t data = (uint32_t)b->n * 2;
    fwrite("RIFF", 1, 4, f); le(f, 36 + data, 4); fwrite("WAVEfmt ", 1, 8, f);
    le(f, 16, 4); le(f, 1, 2); le(f, 1, 2); le(f, (uint32_t)b->rate, 4);
    le(f, (uint32_t)b->rate * 2, 4); le(f, 2, 2); le(f, 16, 2);
    fwrite("data", 1, 4, f); le(f, data, 4);
    for (int i = 0; i < b->n; i++) {
        float x = b->x[i] * 32767.0f;
        if (x > 32767) x = 32767;
        if (x < -32768) x = -32768;
        le(f, (uint32_t)(uint16_t)(int16_t)lrintf(x), 2);
    }
    return fclose(f);
}

// ---- measuring ---------------------------------------------------------------
// The spectral centroid, in Hz: where the weight of the sound sits. 96 probe
// frequencies spaced by ratio from 60 Hz to just under Nyquist, each one a
// plain correlation against the buffer - slow per bin and nothing to get wrong.
static float centroid(const Buf *b) {
    const int bins = 96;
    const float lo = 60.0f, hi = 0.49f * b->rate;
    double num = 0, den = 0;
    for (int j = 0; j < bins; j++) {
        const float hz = lo * powf(hi / lo, (float)j / (bins - 1));
        double re = 0, im = 0;
        for (int i = 0; i < b->n; i++) {
            const double p = 2.0 * 3.14159265358979 * hz * i / b->rate;
            re += b->x[i] * cos(p); im += b->x[i] * sin(p);
        }
        // Weighted by the bin's width, which grows with its frequency.
        const double mag = sqrt(re * re + im * im) * hz;
        num += mag * hz; den += mag;
    }
    return den > 0 ? (float)(num / den) : 0;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    const int writing = !strcmp(mode, "write") && argc > 2;
    const int stats = !strcmp(mode, "stats");
    if (!writing && !stats && strcmp(mode, "list")) {
        fprintf(stderr, "usage: soundgen write <dir> | list | stats\n");
        return 2;
    }
    long total = 0;
    int files = 0;
    for (int cue = 1; cue < ANIM_N_CUES; cue++) {
        const Recipe *r = &RECIPES[cue];
        if (!r->make) continue;
        for (int v = 0; v < r->variants; v++) {
            render(r, cue, v);
            char name[64];
            snprintf(name, sizeof name, "%s_%d.wav", r->name, v);
            const long bytes = 44 + 2L * g_buf.n;
            total += bytes; files++;
            if (writing) {
                char path[1024];
                snprintf(path, sizeof path, "%s/%s", argv[2], name);
                if (write_wav(path, &g_buf) != 0) { fprintf(stderr, "soundgen: cannot write %s\n", path); return 1; }
            } else if (stats) {
                double sq = 0; float peak = 0;
                for (int i = 0; i < g_buf.n; i++) {
                    sq += (double)g_buf.x[i] * g_buf.x[i];
                    if (fabsf(g_buf.x[i]) > peak) peak = fabsf(g_buf.x[i]);
                }
                printf("%-14s cue %2d  %5d Hz  %4d ms  %6ld B  peak %.2f  rms %6.1f dBFS  centroid %5.0f Hz\n",
                       name, cue, g_buf.rate, g_buf.n * 1000 / g_buf.rate, bytes, peak,
                       10 * log10(sq / (g_buf.n ? g_buf.n : 1) + 1e-12), centroid(&g_buf));
            } else {
                printf("%s\t%d\t%d\t%d\t%ld\n", name, cue, g_buf.rate, g_buf.n * 1000 / g_buf.rate, bytes);
            }
        }
    }
    if (writing || stats) fprintf(writing ? stderr : stdout, "soundgen: %d files, %ld bytes\n", files, total);
    return 0;
}
