// seoul11_intro.c: surgical pure-C recreation of se_11_song_club_mix Part 1 (Intro & Build, Bars 0-11, 0:00-0:18.5).
// Transcribed directly from se_11_song_club_mix: exact stop-start syncopated future-bounce stab rhythm,
// exact Royal Road / Bmaj7-C#9 voicings, lush layered supersaw+piano-attack stabs, airy K-pop vocal-chop echoes,
// Bar 4 percussion/brightness lift, Bar 8-10 build, and exact Bar 11 8th-note sparkling cascade.
// Build: clang -O2 -Wall -Werror -I../engine -o seoul11_intro seoul11_intro.c -lm
#pragma clang diagnostic ignored "-Wunused-function"
#include "mix.h"

#define BPM 160.0
#define BEAT_S (60.0 / BPM)          // 0.375 s
#define BAR_S (4.0 * BEAT_S)         // 1.500 s
#define SIX_S (BEAT_S / 4.0)         // 0.09375 s
#define T0 0.054                     // exact grid offset matching se_11
#define TOTAL_S 19.2
#define SEED_INTRO 0x5e001102ULL

static double polyblep(double t, double dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0; }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
    return 0.0;
}

static void set_bq(Bq *f, Bq c) {
    f->b0 = c.b0; f->b1 = c.b1; f->b2 = c.b2; f->a1 = c.a1; f->a2 = c.a2;
}

// ---- 1. Tight, Punchy Future-Bounce Club Kick ----
static void add_kick(float *L, float *R, long N, double t0, double amp) {
    long i0 = lround(t0 * SR);
    double dur = 0.24;
    long n = lround(dur * SR);
    double ph = 0.0;
    for (long i = 0; i < n && i0 + i < N; i++) {
        if (i0 + i < 0) continue;
        double t = (double)i / SR;
        double f = 48.0 + 190.0 * exp(-t / 0.023);
        ph += TAU * f / SR;
        double body = sin(ph) * exp(-t / 0.092);
        double click = 0.44 * sin(TAU * 2500.0 * t) * exp(-t / 0.0022);
        double env = t < 0.001 ? t / 0.001 : (t > dur - 0.012 ? (dur - t) / 0.012 : 1.0);
        double y = tanh((body + click) * 1.85) * amp * env;
        L[i0 + i] += (float)y;
        R[i0 + i] += (float)y;
    }
}

// ---- 2. Huge Layered Stereo Clap + Snare ----
static void add_clap(float *L, float *R, long N, double t0, double amp, double pitch_mul, int tight, Rng *rg) {
    long i0 = lround(t0 * SR);
    double dur = tight ? 0.12 : 0.24;
    long n = lround(dur * SR);
    Bq bpL = bq_bp(1950.0 * pitch_mul, 0.95), bpR = bq_bp(2120.0 * pitch_mul, 0.95);
    Bq hpL = bq_hp(440.0, 0.7), hpR = bq_hp(440.0, 0.7);
    double taps[3] = {0.0, 0.009, 0.019};
    for (long i = 0; i < n && i0 + i < N; i++) {
        if (i0 + i < 0) continue;
        double t = (double)i / SR;
        double burst = 0.0;
        for (int k = 0; k < 3; k++) {
            if (t >= taps[k]) burst += exp(-(t - taps[k]) / (k == 2 ? (tight ? 0.036 : 0.075) : 0.008));
        }
        double nL = bq(&hpL, bq(&bpL, rn(rg))) * burst * 0.56;
        double nR = bq(&hpR, bq(&bpR, rn(rg))) * burst * 0.56;
        double f_sn = (200.0 + 115.0 * exp(-t / 0.010)) * pitch_mul;
        double body = sin(TAU * f_sn * t) * exp(-t / (tight ? 0.025 : 0.048)) * 0.58;
        L[i0 + i] += (float)(tanh((nL + body) * 1.4) * amp);
        R[i0 + i] += (float)(tanh((nR + body) * 1.4) * amp);
    }
}

// ---- 3. Crisp Air Hi-Hat / Shaker ----
static void add_hat(float *L, float *R, long N, double t0, double amp, int open, double pan, Rng *rg) {
    long i0 = lround(t0 * SR);
    double dur = open ? 0.20 : 0.046;
    double tau = open ? 0.068 : 0.014;
    long n = lround(dur * SR);
    Bq hp = bq_hp(8000.0, 0.7);
    double gl = cos((pan + 1.0) * M_PI / 4.0), gr = sin((pan + 1.0) * M_PI / 4.0);
    for (long i = 0; i < n && i0 + i < N; i++) {
        if (i0 + i < 0) continue;
        double t = (double)i / SR;
        double x = bq(&hp, rn(rg)) * exp(-t / tau);
        if (t < 0.0008) x *= t / 0.0008;
        L[i0 + i] += (float)(x * amp * gl);
        R[i0 + i] += (float)(x * amp * gr);
    }
}

// ---- 4. Noise Riser & Crash ----
static void add_riser(float *L, float *R, long N, double t0, double dur, double amp, Rng *rg) {
    long i0 = lround(t0 * SR);
    long n = lround(dur * SR);
    Bq bpL = bq_bp(350.0, 1.7), bpR = bq_bp(370.0, 1.7);
    for (long i = 0; i < n && i0 + i < N; i++) {
        if (i0 + i < 0) continue;
        double u = (double)i / n;
        if (i % 64 == 0) {
            double fc = 300.0 * pow(36.0, u);
            set_bq(&bpL, bq_bp(fc, 1.6));
            set_bq(&bpR, bq_bp(fc * 1.05, 1.6));
        }
        double e = u * u * (u > 0.96 ? (1.0 - u) / 0.04 : 1.0);
        L[i0 + i] += (float)(bq(&bpL, rn(rg)) * e * amp);
        R[i0 + i] += (float)(bq(&bpR, rn(rg)) * e * amp);
    }
}

static void add_crash(float *L, float *R, long N, double t0, double amp, Rng *rg) {
    long i0 = lround(t0 * SR);
    long n = lround(1.6 * SR);
    Bq hpL = bq_hp(4800.0, 0.7), hpR = bq_hp(5200.0, 0.7);
    for (long i = 0; i < n && i0 + i < N; i++) {
        if (i0 + i < 0) continue;
        double t = (double)i / SR;
        double e = (t < 0.003 ? t / 0.003 : 1.0) * exp(-t / 0.62);
        L[i0 + i] += (float)(bq(&hpL, rn(rg)) * e * amp);
        R[i0 + i] += (float)(bq(&hpR, rn(rg)) * e * amp);
    }
}

// ---- 5. Fat Gated 808 Sub + Lush Layered Future-Bass Supersaw & Glossy Piano-Attack Chord Stab ----
static void add_gated_sub(float *L, float *R, long N, double t0, double dur, double midi, double amp) {
    long i0 = lround(t0 * SR);
    double rel = 0.025;
    long n = lround((dur + rel) * SR);
    double w = TAU * mtof(midi) / SR;
    double ph = 0.0;
    for (long i = 0; i < n && i0 + i < N; i++) {
        if (i0 + i < 0) continue;
        double t = (double)i / SR;
        double env = (t < 0.005 ? t / 0.005 : 1.0) * (t > dur ? fmax(0.0, 1.0 - (t - dur) / rel) : 1.0);
        double duck = 1.0 - 0.62 * exp(-t / 0.028);
        double y = tanh(1.75 * sin(ph) + 0.32 * sin(2.0 * ph) + 0.10 * sin(3.0 * ph)) / tanh(1.75);
        ph += w;
        L[i0 + i] += (float)(y * env * duck * amp);
        R[i0 + i] += (float)(y * env * duck * amp);
    }
}

static void add_gated_chord(float *L, float *R, long N, double t0, double dur, const int *notes, int nn,
                            double amp, double fc_peak, double fc_base, Rng *rg) {
    long i0 = lround(t0 * SR);
    double rel = 0.032;
    long n = lround((dur + rel) * SR);
    double ph[8][7], dt[8][7], gl[7], gr[7], ph_att[8], w_att[8];
    for (int v = 0; v < 7; v++) {
        double pan = (v - 3.0) / 3.0 * 0.92;
        double th = (pan + 1.0) * M_PI / 4.0;
        gl[v] = cos(th); gr[v] = sin(th);
    }
    for (int k = 0; k < nn && k < 8; k++) {
        double f0 = mtof(notes[k]);
        w_att[k] = TAU * f0 / SR;
        ph_att[k] = 0.0;
        for (int v = 0; v < 7; v++) {
            double cents = -22.0 + 7.333 * v;
            dt[k][v] = f0 * pow(2.0, cents / 1200.0) / SR;
            ph[k][v] = r01(rg);
        }
    }
    Bq fl[2], fr[2];
    static const double Q[2] = {0.5411961, 1.3065630};
    for (int j = 0; j < 2; j++) { fl[j] = bq_lp(fc_base, Q[j]); fr[j] = fl[j]; }
    Bq hpL = bq_hp(160.0, 0.7), hpR = bq_hp(160.0, 0.7);
    double norm = 1.0 / (nn * 7.0);
    double att_norm = 1.0 / sqrt((double)nn);
    for (long i = 0; i < n && i0 + i < N; i++) {
        if (i0 + i < 0) continue;
        double t = (double)i / SR;
        if (i % 32 == 0) {
            // Brassy future-bass bloom: opens fast in 22 ms, stays lush and bright!
            double open = (t < 0.022) ? (t / 0.022) : exp(-(t - 0.022) / fmax(0.11, dur * 0.65));
            double fc = fc_base + (fc_peak - fc_base) * open;
            if (fc < 180.0) fc = 180.0;
            if (fc > 14000.0) fc = 14000.0;
            for (int j = 0; j < 2; j++) { Bq c = bq_lp(fc, Q[j]); set_bq(&fl[j], c); set_bq(&fr[j], c); }
        }
        double sl = 0.0, sr = 0.0, att = 0.0;
        for (int k = 0; k < nn && k < 8; k++) {
            for (int v = 0; v < 7; v++) {
                double s = 2.0 * ph[k][v] - 1.0 - polyblep(ph[k][v], dt[k][v]);
                sl += s * gl[v]; sr += s * gr[v];
                ph[k][v] += dt[k][v]; if (ph[k][v] >= 1.0) ph[k][v] -= 1.0;
            }
            // Glossy FM piano/synth attack transient on onset (gives that polished K-pop studio bite!)
            if (t < 0.09) {
                double p = ph_att[k];
                att += sin(p + 1.4 * exp(-t / 0.025) * sin(2.0 * p)) * exp(-t / 0.032);
                ph_att[k] += w_att[k];
            }
        }
        sl *= norm; sr *= norm;
        for (int j = 0; j < 2; j++) { sl = bq(&fl[j], sl); sr = bq(&fr[j], sr); }
        sl = bq(&hpL, sl); sr = bq(&hpR, sr);
        double env = (t < 0.004 ? t / 0.004 : 1.0) * (t > dur ? fmax(0.0, 1.0 - (t - dur) / rel) : 1.0);
        double duck = 1.0 - 0.40 * exp(-t / 0.024);
        double a_sig = att * att_norm * 0.24 * (fc_peak / 9000.0);
        L[i0 + i] += (float)((sl * duck + a_sig) * env * amp);
        R[i0 + i] += (float)((sr * duck + a_sig) * env * amp);
    }
}

// ---- 6. Airy, Glossy K-Pop Vocal-Chop Accent (soft formant whisper, NOT a cartoon laser squeak!) ----
static void add_vocal_chop(float *L, float *R, long N, double t0, double dur, double m0, double m1, double amp, double pan, double bright, Rng *rg) {
    long i0 = lround(t0 * SR);
    double rel = 0.08;
    long n = lround((dur + rel) * SR);
    double ph1 = 0.0, ph2 = 0.0;
    // Soft airy vocal formant bands ("ahh / ohh")
    Bq fm1 = bq_bp(780.0 * bright, 2.6), fm2 = bq_bp(1950.0 * bright, 3.0);
    Bq air = bq_bp(4600.0, 1.5);
    double gl = cos((pan + 1.0) * M_PI / 4.0), gr = sin((pan + 1.0) * M_PI / 4.0);
    for (long i = 0; i < n && i0 + i < N; i++) {
        if (i0 + i < 0) continue;
        double t = (double)i / SR;
        double u = fmin(1.0, t / fmax(0.03, dur));
        double midi = m0 + (m1 - m0) * (1.0 - exp(-u * 5.0));
        double f = mtof(midi);
        double d1 = f * pow(2.0, -7.0 / 1200.0) / SR;
        double d2 = f * pow(2.0, +7.0 / 1200.0) / SR;
        double s1 = 2.0 * ph1 - 1.0 - polyblep(ph1, d1);
        double s2 = 2.0 * ph2 - 1.0 - polyblep(ph2, d2);
        double tri = 2.0 * fabs(2.0 * ph1 - 1.0) - 1.0;
        double raw = 0.45 * (s1 + s2) * 0.5 + 0.55 * tri;
        ph1 += d1; if (ph1 >= 1.0) ph1 -= 1.0;
        ph2 += d2; if (ph2 >= 1.0) ph2 -= 1.0;
        double breath = bq(&air, rn(rg)) * 0.14;
        double y = 0.72 * bq(&fm1, raw) + 0.52 * bq(&fm2, raw) + breath;
        double env = (t < 0.012 ? t / 0.012 : 1.0) * (t > dur ? fmax(0.0, 1.0 - (t - dur) / rel) : 1.0);
        L[i0 + i] += (float)(y * env * amp * gl);
        R[i0 + i] += (float)(y * env * amp * gr);
    }
}

// ---- 7. Glassy Rhodes/Harp Chime for Exact Bar 11 Cascade ----
static void add_chime(float *L, float *R, long N, double t0, double midi, double amp, double pan) {
    long i0 = lround(t0 * SR);
    long n = lround(0.75 * SR);
    double f = mtof(midi), w = TAU * f / SR;
    double gl = cos((pan + 1.0) * M_PI / 4.0), gr = sin((pan + 1.0) * M_PI / 4.0);
    for (long i = 0; i < n && i0 + i < N; i++) {
        if (i0 + i < 0) continue;
        double t = (double)i / SR, th = w * i;
        double y = sin(th + 1.05 * exp(-t / 0.075) * sin(2.0 * th)) * exp(-t / 0.24)
                 + 0.34 * sin(2.0 * th) * exp(-t / 0.16)
                 + 0.12 * sin(3.0 * th) * exp(-t / 0.055);
        if (t < 0.002) y *= t / 0.002;
        L[i0 + i] += (float)(y * amp * gl);
        R[i0 + i] += (float)(y * amp * gr);
    }
}

// ---- Transcribed Exact Chord Voicings for Bars 0-3 (repeated in 4-7 and 8-10) ----
static const int V_FSHARP_HI[5]  = {54, 61, 65, 70, 77}; // F#3 C#4 F4 A#4 F5
static const int V_FSHARP_TOP[5] = {54, 61, 65, 70, 82}; // F#3 C#4 F4 A#4 A#5
static const int V_F7_A[5]       = {53, 60, 65, 70, 73}; // F3 C4 F4 A#4 C#5
static const int V_F7_B[5]       = {53, 60, 63, 70, 73}; // F3 C4 D#4 A#4 C#5
static const int V_PICKUP[4]     = {58, 63, 70, 75};     // A#3 D#4 A#4 D#5
static const int V_ASHARP_A[5]   = {58, 65, 70, 73, 82}; // A#3 F4 A#4 C#5 A#5
static const int V_ASHARP_B[5]   = {58, 61, 65, 68, 73}; // A#3 C#4 F4 G#4 C#5
static const int V_BMAJ7[5]      = {59, 66, 70, 75, 82}; // B3 F#4 A#4 D#5 A#5
static const int V_CSHARP_B[5]   = {61, 66, 71, 75, 83}; // C#4 F#4 B4 D#5 B5
static const int V_CSHARP7[5]    = {61, 65, 68, 73, 80}; // C#4 F4 G#4 C#5 G#5
static const int V_GSHARP_PK[5]  = {56, 63, 68, 73, 82}; // G#3 D#4 G#4 C#5 A#5

static void fire_stab(float *L, float *R, float *revL, float *revR, long N,
                      double t, double dur, int bass_midi, const int *ch, int nn,
                      double amp, double fc_pk, double fc_bs, int with_kick, int with_hat, Rng *rg) {
    if (with_kick) add_kick(L, R, N, t, 0.62);
    add_gated_sub(L, R, N, t, dur, bass_midi, 0.44 * amp);
    add_gated_chord(revL, revR, N, t, dur, ch, nn, 0.60 * amp, fc_pk, fc_bs, rg);
    if (with_hat) add_hat(L, R, N, t, 0.15 * amp, 0, 0.25, rg);
}

int main(int argc, char **argv) {
    const char *out_path = argc > 1 ? argv[1] : "/Users/alex/Music/foolish_cities_v7_sampled/work/seoul11_repro/seoul11_intro.wav";
    long N = lround(TOTAL_S * SR);
    float *L = calloc(N, sizeof(float)), *R = calloc(N, sizeof(float));
    float *revL = calloc(N, sizeof(float)), *revR = calloc(N, sizeof(float));
    if (!L || !R || !revL || !revR) { fprintf(stderr, "out of memory\n"); return 1; }
    Rng rg = {SEED_INTRO};

    for (int b = 0; b < 12; b++) {
        double bt = T0 + b * BAR_S;
        int m = b % 4;

        // ============================================================
        // BAR 11 (16.55 - 18.05s): THE ICONIC 1-BAR BREAK & EXACT CASCADE
        // ============================================================
        if (b == 11) {
            add_kick(L, R, N, bt, 0.56);
            add_gated_sub(L, R, N, bt, 0.24, 30, 0.36);
            // Exact transcribed 8th-note sparkling cascade + 16th pickup:
            // D#5 -> F#5 -> A#5 -> F6 -> F#5 -> G#5 -> C#6 -> A#5 -> C#6
            static const int c_step[9] = {0, 2, 4, 6, 8, 10, 12, 14, 15};
            static const int c_midi[9] = {75, 78, 82, 89, 78, 80, 85, 82, 85};
            for (int k = 0; k < 9; k++) {
                double pan = -0.58 + 1.16 * (k / 8.0);
                add_chime(revL, revR, N, bt + c_step[k] * SIX_S, c_midi[k], 0.28 * (0.80 + 0.20 * (k / 8.0)), pan);
            }
            add_riser(revL, revR, N, bt + 3 * SIX_S, 13 * SIX_S, 0.17, &rg);
            continue;
        }

        // Lush, bright, glossy filter progression across the 3 four-bar phrases:
        // Phrase 1 (Bars 0-3): warm lush future-bounce (3800 -> 5200 Hz, NOT telephoney!)
        // Phrase 2 (Bars 4-7): wide open studio K-pop brightness (7600 -> 9400 Hz) + beat-3 claps
        // Phrase 3 (Bars 8-10): full hype build (9600 -> 12200 Hz) + accelerating snare roll
        double fc_pk, fc_bs, gain;
        if (b < 4) {
            fc_pk = 3800.0 + 460.0 * b;
            fc_bs = 950.0 + 140.0 * b;
            gain = 0.90;
        } else if (b < 8) {
            fc_pk = 7600.0 + 600.0 * (b - 4);
            fc_bs = 1800.0 + 220.0 * (b - 4);
            gain = 0.98;
        } else {
            fc_pk = 9600.0 + 1200.0 * (b - 8);
            fc_bs = 2400.0 + 450.0 * (b - 8);
            gain = 1.05;
        }

        // Subtle crisp 16th-note shaker/hat pulse keeping momentum alive through the pockets
        for (int s = 0; s < 16; s += 2) {
            double ha = (b < 4) ? 0.045 : 0.075;
            if (s % 4 == 2) ha *= 1.5;
            add_hat(L, R, N, bt + s * SIX_S, ha, (b >= 4 && s == 14), (s % 4 == 2 ? 0.35 : -0.35), &rg);
        }

        // ---- EXACT STOP-START SYNCOPATED STAB RHYTHM ----
        if (m == 0) {
            fire_stab(L, R, revL, revR, N, bt + 0 * SIX_S, 2.9 * SIX_S, 30, V_FSHARP_HI,  5, gain, fc_pk, fc_bs, 1, 1, &rg);
            fire_stab(L, R, revL, revR, N, bt + 6 * SIX_S, 2.0 * SIX_S, 30, V_FSHARP_TOP, 5, gain, fc_pk, fc_bs, 1, 1, &rg);
            // Soft airy vocal-chop echoes tucked into the stereo reverb tail
            double vb = (b < 4) ? 0.85 : 1.05;
            add_vocal_chop(revL, revR, N, bt + 10 * SIX_S, 1.8 * SIX_S, 73, 77, 0.14 * gain, -0.38, vb, &rg);
            add_vocal_chop(revL, revR, N, bt + 13 * SIX_S, 1.8 * SIX_S, 80, 77, 0.14 * gain,  0.38, vb, &rg);
            if (b >= 4) add_clap(L, R, N, bt + 8 * SIX_S, 0.38, 1.0, 0, &rg);
        } else if (m == 1) {
            if (b == 9) {
                fire_stab(L, R, revL, revR, N, bt + 0 * SIX_S, 1.8 * SIX_S, 30, V_FSHARP_HI, 5, gain, fc_pk, fc_bs, 1, 1, &rg);
            }
            fire_stab(L, R, revL, revR, N, bt + 2 * SIX_S, 2.0 * SIX_S, 30, V_F7_A,   5, gain, fc_pk, fc_bs, 1, 1, &rg);
            fire_stab(L, R, revL, revR, N, bt + 6 * SIX_S, 2.0 * SIX_S, 29, V_F7_B,   5, gain, fc_pk, fc_bs, 1, 1, &rg);
            double vb = (b < 4) ? 0.85 : 1.08;
            add_vocal_chop(revL, revR, N, bt + 10 * SIX_S, 2.0 * SIX_S, 73, 75, 0.16 * gain, 0.32, vb, &rg);
            fire_stab(L, R, revL, revR, N, bt + 14 * SIX_S, 2.0 * SIX_S, 34, V_PICKUP, 4, gain * 1.04, fc_pk, fc_bs, 1, 1, &rg);
            if (b >= 4) add_clap(L, R, N, bt + 8 * SIX_S, 0.38, 1.0, 0, &rg);
        } else if (m == 2) {
            if (b == 10) {
                fire_stab(L, R, revL, revR, N, bt +  0 * SIX_S, 3.0 * SIX_S, 34, V_ASHARP_A, 5, gain, fc_pk, fc_bs, 1, 1, &rg);
                fire_stab(L, R, revL, revR, N, bt +  4 * SIX_S, 3.0 * SIX_S, 34, V_ASHARP_B, 5, gain, fc_pk, fc_bs, 1, 1, &rg);
                fire_stab(L, R, revL, revR, N, bt +  8 * SIX_S, 3.0 * SIX_S, 35, V_BMAJ7,    5, gain, fc_pk, fc_bs, 1, 1, &rg);
                fire_stab(L, R, revL, revR, N, bt + 12 * SIX_S, 3.4 * SIX_S, 37, V_CSHARP7,  5, gain * 1.08, fc_pk, fc_bs, 1, 1, &rg);
            } else {
                fire_stab(L, R, revL, revR, N, bt + 0 * SIX_S, 2.9 * SIX_S, 34, V_ASHARP_A, 5, gain, fc_pk, fc_bs, 1, 1, &rg);
                fire_stab(L, R, revL, revR, N, bt + 6 * SIX_S, 2.0 * SIX_S, 34, V_ASHARP_B, 5, gain, fc_pk, fc_bs, 1, 1, &rg);
                double vb = (b < 4) ? 0.85 : 1.08;
                add_vocal_chop(revL, revR, N, bt +  9 * SIX_S, 1.8 * SIX_S, 77, 80, 0.15 * gain, -0.40, vb, &rg);
                add_vocal_chop(revL, revR, N, bt + 13 * SIX_S, 1.8 * SIX_S, 82, 80, 0.15 * gain,  0.40, vb, &rg);
                if (b >= 4) add_clap(L, R, N, bt + 8 * SIX_S, 0.38, 1.0, 0, &rg);
            }
        } else {
            fire_stab(L, R, revL, revR, N, bt +  0 * SIX_S, 2.9 * SIX_S, 35, V_BMAJ7,     5, gain, fc_pk, fc_bs, 1, 1, &rg);
            fire_stab(L, R, revL, revR, N, bt +  6 * SIX_S, 2.0 * SIX_S, 37, V_CSHARP_B,  5, gain, fc_pk, fc_bs, 1, 1, &rg);
            fire_stab(L, R, revL, revR, N, bt +  9 * SIX_S, 2.6 * SIX_S, 37, V_CSHARP7,   5, gain, fc_pk, fc_bs, 1, 1, &rg);
            fire_stab(L, R, revL, revR, N, bt + 13 * SIX_S, 2.8 * SIX_S, 32, V_GSHARP_PK, 5, gain * 1.06, fc_pk, fc_bs, 1, 1, &rg);
            if (b >= 4) {
                add_clap(L, R, N, bt + 8 * SIX_S, 0.38, 1.0, 0, &rg);
                add_clap(L, R, N, bt + 14 * SIX_S, 0.25, 1.08, 1, &rg);
            }
        }

        // ---- PRE-DROP SNARE ROLL + RISER (Bars 8, 9, 10) ----
        if (b >= 8 && b <= 10) {
            int step_inc = (b == 8) ? 4 : (b == 9) ? 2 : 1;
            for (int s = 0; s < 16; s += step_inc) {
                double u = ((b - 8) * 16 + s) / 48.0;
                add_clap(L, R, N, bt + s * SIX_S, 0.16 + 0.18 * u, 0.92 + 0.48 * u, 1, &rg);
            }
            if (b == 8) add_riser(revL, revR, N, bt, 3 * BAR_S, 0.19, &rg);
        }
    }

    // Resolve cleanly onto Bar 12 downbeat (18.054s) so the Bar 11 cascade lands naturally!
    double drop_t = T0 + 12 * BAR_S;
    add_crash(L, R, N, drop_t, 0.28, &rg);
    fire_stab(L, R, revL, revR, N, drop_t, 0.90, 30, V_FSHARP_TOP, 5, 1.15, 11800.0, 2800.0, 1, 1, &rg);

    // Lush, bright studio plate reverb (1.25 s, high-passed at 360 Hz so pockets stay clean yet atmospheric!)
    Rev rv;
    RevCfg rc = {1.25, 0.88, 10.0, 1.1, 360.0, 8200.0, 0.11, 1.0, 0.18, 0.22};
    rev_init(&rv, rc);
    for (long i = 0; i < N; i++) {
        double wl, wr;
        rev_tick(&rv, revL[i], revR[i], &wl, &wr);
        L[i] += revL[i] + (float)(wl * 3.4);
        R[i] += revR[i] + (float)(wr * 3.4);
    }

    // Master loudness normalize to -12.5 LUFS + true-peak ceiling -1.0 dBTP
    double cur_lufs = lufs(L, R, N, NULL, NULL, NULL);
    double g = db2a(-12.5 - cur_lufs);
    for (long i = 0; i < N; i++) { L[i] *= (float)g; R[i] *= (float)g; }
    limit(L, R, N, -1.0, 0.06);
    double tp = a2db(true_peak(L, R, N));

    wav_write24(out_path, L, R, N);
    fprintf(stderr, "seoul11_intro: %.2f s, raw %.1f LUFS -> -12.5 LUFS, tp %.2f dBTP -> %s\n",
            (double)N / SR, cur_lufs, tp, out_path);
    free(L); free(R); free(revL); free(revR);
    return 0;
}
