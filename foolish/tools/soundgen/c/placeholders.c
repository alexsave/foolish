// placeholders.c - the five cues no recording has been modelled for yet.
//
// OUT, GOOD, HAND_OFF, FINISH and FOOL are not a card moving, and nobody has
// recorded or modelled them, so they keep the first set of synthesised
// stand-ins (written 2026-10-13, never listened to by their author: see
// docs/AUDIO.md). Every card cue is a model now (models.c).
//
// HOW A SOUND IS BUILT. Layers added into one buffer:
//   burst   band-passed noise with a sharp attack and an exponential tail
//   swell   the same noise under a slow rise and fall
//   thump   a short sine falling in pitch
//   tone    a soft mallet note - the sounds that are not cards at all
// Each is finished to one peak level (finish, below), so how loud a cue is in
// the game is the hosts' table's to say.
#include "placeholders.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "rng.h"

#define PI 3.14159265358979f
#define MAX_SAMPLES 48000   // two seconds at the highest rate used; nothing is near it

struct Buf {
    float    x[MAX_SAMPLES];
    int      n;      // samples in use
    int      rate;   // Hz
    uint64_t rng;    // shared/c/rng.h state
};

// ---- noise ----------------------------------------------------------------
static float pnoise(Buf *b) {
    return (float)(int32_t)rng_u32(&b->rng) / 2147483648.0f;
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
        b->x[i0 + i] += gain * env * band_run(&f, pnoise(b));
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
        const float shake = 1 + rough * 9.0f * band_run(&r, pnoise(b));
        b->x[i0 + i] += gain * e * e * (shake < 0 ? 0 : shake) * band_run(&g, band_run(&f, pnoise(b)));
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
// One function a cue. `k` is a factor near 1 that differs between variants.

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

int placeholder_render(void (*make)(Buf *, int, float), int rate, int cue, int v, int variants,
                       const float **x, int *n) {
    memset(&g_buf, 0, sizeof g_buf);
    g_buf.rate = rate;
    g_buf.rng = rng_seed64((uint64_t)(cue * 7919 + v * 104729 + 1));
    const float k = 1 + 0.08f * ((float)v - (float)(variants - 1) / 2);
    make(&g_buf, v, k);
    finish(&g_buf);
    *x = g_buf.x; *n = g_buf.n;
    return 0;
}

const Placeholder PLACEHOLDERS[] = {
    { ANIM_CUE_OUT,      r_out,      8000 },
    { ANIM_CUE_GOOD,     r_good,     12000 },
    { ANIM_CUE_HAND_OFF, r_hand_off, 12000 },
    { ANIM_CUE_FINISH,   r_finish,   8000 },
    { ANIM_CUE_FOOL,     r_fool,     8000 },
};
const int N_PLACEHOLDERS = (int)(sizeof PLACEHOLDERS / sizeof PLACEHOLDERS[0]);
