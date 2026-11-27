// seoul11_repro.c: faithful from-scratch C synthesis recreation of se_11_song_club_mix (160 BPM, 80 bars, ~121 s).
// Key: C# major / A# minor (Royal Road: F#maj9 -> F9 -> A#m9 -> G#m9/C#9).
// Half-time K-pop future-bass groove (160 BPM / 80 BPM feel) with 4-on-the-floor pre-drop build.
// Deterministic seeded RNG, zero external samples, pure DSP synthesis using engine/mix.h & engine/dsp.h.
// Build: clang -O2 -Wall -Werror -I../engine -o seoul11_repro seoul11_repro.c -lm
#pragma clang diagnostic ignored "-Wunused-function"
#include "mix.h"

#define BPM 160.0
#define BEAT_S (60.0 / BPM)          // 0.375 s
#define BAR_S (4.0 * BEAT_S)         // 1.500 s
#define SIX_S (BEAT_S / 4.0)         // 0.09375 s
#define NBARS 80
#define TOTAL_S (NBARS * BAR_S + 2.5)
#define REPRO_SEED 0x5e001100ULL

static double polyblep(double t, double dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0; }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
    return 0.0;
}

static void set_bq(Bq *f, Bq c) {
    f->b0 = c.b0; f->b1 = c.b1; f->b2 = c.b2; f->a1 = c.a1; f->a2 = c.a2;
}

// ---- Sidechain envelope synced to kick period ----
static double sc_env(double t, double depth, double tau, double period) {
    if (depth <= 0.0) return 1.0;
    double ph = fmod(t, period);
    if (ph < 0.0) ph += period;
    return 1.0 - depth * exp(-ph / tau);
}

// ---- 1. Punchy K-Pop / Future-Bass Kick ----
static void add_kick(float *L, float *R, long N, double t0, double amp, int soft) {
    long i0 = lround(t0 * SR);
    double dur = soft ? 0.34 : 0.26;
    long n = lround(dur * SR);
    double ph = 0.0;
    double f_hi = soft ? 120.0 : 195.0;
    double f_lo = soft ? 43.0 : 48.0;
    double tau_f = soft ? 0.044 : 0.026;
    double tau_a = soft ? 0.15 : 0.10;
    for (long i = 0; i < n && i0 + i < N; i++) {
        if (i0 + i < 0) continue;
        double t = (double)i / SR;
        double f = f_lo + (f_hi - f_lo) * exp(-t / tau_f);
        ph += TAU * f / SR;
        double body = sin(ph) * exp(-t / tau_a);
        double click = (soft ? 0.07 : 0.38) * sin(TAU * 2200.0 * t) * exp(-t / 0.0024);
        double env = t < 0.0012 ? t / 0.0012 : (t > dur - 0.015 ? (dur - t) / 0.015 : 1.0);
        double y = tanh((body + click) * (soft ? 1.25 : 1.75)) * amp * env;
        L[i0 + i] += (float)y;
        R[i0 + i] += (float)y;
    }
}

// ---- 2. Huge Layered Future-Bass Snare + Stereo Clap (or Crisp Rim/Snap in Chill) ----
static void add_snare(float *L, float *R, long N, double t0, double amp, int mode, double pitch_mul, Rng *rg) {
    // mode: 0 = big future-bass snare+clap, 1 = chill rimshot/snap, 2 = tight build roll snare
    long i0 = lround(t0 * SR);
    if (mode == 1) {
        long n = lround(0.12 * SR);
        Bq bp = bq_bp(1750.0, 2.0);
        for (long i = 0; i < n && i0 + i < N; i++) {
            if (i0 + i < 0) continue;
            double t = (double)i / SR;
            double tone = 0.55 * sin(TAU * 430.0 * t) * exp(-t / 0.014) + 0.40 * sin(TAU * 1050.0 * t) * exp(-t / 0.007);
            double nz = bq(&bp, rn(rg)) * exp(-t / 0.032) * 0.85;
            double y = (tone + nz) * amp;
            L[i0 + i] += (float)(y * 0.94);
            R[i0 + i] += (float)(y * 1.06);
        }
        return;
    }
    double dur = (mode == 2) ? 0.14 : 0.30;
    long n = lround(dur * SR);
    Bq bpL = bq_bp(1850.0 * pitch_mul, 0.95), bpR = bq_bp(1980.0 * pitch_mul, 0.95);
    Bq hpL = bq_hp(420.0, 0.7), hpR = bq_hp(420.0, 0.7);
    double taps[3] = {0.0, 0.010, 0.021};
    double body_tau = (mode == 2) ? 0.032 : 0.055;
    double nz_tau = (mode == 2) ? 0.045 : 0.088;
    for (long i = 0; i < n && i0 + i < N; i++) {
        if (i0 + i < 0) continue;
        double t = (double)i / SR;
        double burst = 0.0;
        for (int k = 0; k < 3; k++) {
            if (t >= taps[k]) burst += exp(-(t - taps[k]) / (k == 2 ? nz_tau : 0.009));
        }
        double nL = bq(&hpL, bq(&bpL, rn(rg))) * burst * 0.52;
        double nR = bq(&hpR, bq(&bpR, rn(rg))) * burst * 0.52;
        double f_sn = (195.0 + 115.0 * exp(-t / 0.011)) * pitch_mul;
        double body = sin(TAU * f_sn * t) * exp(-t / body_tau) * 0.62;
        double crack = sin(TAU * 920.0 * pitch_mul * t) * exp(-t / 0.006) * 0.25;
        L[i0 + i] += (float)(tanh((nL + body + crack) * 1.35) * amp);
        R[i0 + i] += (float)(tanh((nR + body + crack) * 1.35) * amp);
    }
}

// ---- 3. Crisp Air Hi-Hat (closed / open) ----
static void add_hat(float *L, float *R, long N, double t0, double amp, int open, double pan, Rng *rg) {
    long i0 = lround(t0 * SR);
    double dur = open ? 0.24 : 0.052;
    double tau = open ? 0.082 : 0.015;
    long n = lround(dur * SR);
    Bq hp = bq_hp(7800.0, 0.7);
    double gl = cos((pan + 1.0) * M_PI / 4.0), gr = sin((pan + 1.0) * M_PI / 4.0);
    for (long i = 0; i < n && i0 + i < N; i++) {
        if (i0 + i < 0) continue;
        double t = (double)i / SR;
        double x = bq(&hp, rn(rg)) * exp(-t / tau);
        if (t < 0.001) x *= t / 0.001;
        L[i0 + i] += (float)(x * amp * gl);
        R[i0 + i] += (float)(x * amp * gr);
    }
}

// ---- 4. Crash Cymbal & Stereo Noise Riser / Downlifter ----
static void add_crash(float *L, float *R, long N, double t0, double amp, Rng *rg) {
    long i0 = lround(t0 * SR);
    long n = lround(2.6 * SR);
    Bq hpL = bq_hp(4800.0, 0.7), hpR = bq_hp(5200.0, 0.7);
    for (long i = 0; i < n && i0 + i < N; i++) {
        if (i0 + i < 0) continue;
        double t = (double)i / SR;
        double e = (t < 0.004 ? t / 0.004 : 1.0) * exp(-t / 0.72);
        L[i0 + i] += (float)(bq(&hpL, rn(rg)) * e * amp);
        R[i0 + i] += (float)(bq(&hpR, rn(rg)) * e * amp);
    }
}

static void add_sweep(float *L, float *R, long N, double t0, double dur, double amp, int riser, Rng *rg) {
    long i0 = lround(t0 * SR);
    long n = lround(dur * SR);
    Bq bpL = bq_bp(400.0, 1.8), bpR = bq_bp(400.0, 1.8);
    for (long i = 0; i < n && i0 + i < N; i++) {
        if (i0 + i < 0) continue;
        double u = (double)i / n;
        if (i % 64 == 0) {
            double fc = riser ? 260.0 * pow(38.0, u) : 8200.0 * pow(0.035, u);
            set_bq(&bpL, bq_bp(fc, 1.7));
            set_bq(&bpR, bq_bp(fc * 1.05, 1.7));
        }
        double e = riser ? (u * u * (u > 0.96 ? (1.0 - u) / 0.04 : 1.0)) : ((1.0 - u) * (1.0 - u) * (u < 0.03 ? u / 0.03 : 1.0));
        L[i0 + i] += (float)(bq(&bpL, rn(rg)) * e * amp);
        R[i0 + i] += (float)(bq(&bpR, rn(rg)) * e * amp);
    }
}

// ---- 5. Continuous Gliding 808 Sub-Bass Renderer (warm harmonic punch + smooth portamento) ----
typedef struct { double t, dur, midi, amp, sc_dep, sc_per; } SubNote;
static void render_sub_bass(float *L, float *R, long N, const SubNote *ev, int nev) {
    if (nev <= 0) return;
    double ph = 0.0, cur_midi = ev[0].midi;
    int idx = 0;
    for (long i = 0; i < N; i++) {
        double t = (double)i / SR;
        while (idx + 1 < nev && t >= ev[idx + 1].t) idx++;
        const SubNote *e = &ev[idx];
        double dt = t - e->t;
        if (dt < 0.0 || dt > e->dur + 0.06) continue;
        // 32 ms smooth 808 pitch glide between bass notes
        cur_midi += (e->midi - cur_midi) * (1.0 - exp(-1.0 / (0.032 * SR)));
        double f = mtof(cur_midi);
        ph += TAU * f / SR;
        if (ph >= TAU) ph -= TAU;
        double env = (dt < 0.008 ? dt / 0.008 : 1.0) * (dt > e->dur ? fmax(0.0, 1.0 - (dt - e->dur) / 0.06) : 1.0);
        double sc = sc_env(t, e->sc_dep, 0.095, e->sc_per);
        // Warm 808 with 2nd + subtle 3rd harmonic so it translates on all speakers
        double y = tanh(1.65 * sin(ph) + 0.28 * sin(2.0 * ph) + 0.08 * sin(3.0 * ph)) / tanh(1.65);
        y *= env * sc * e->amp;
        L[i] += (float)y;
        R[i] += (float)y;
    }
}

// ---- 6. Massive 7-Voice Wide Future-Bass Supersaw Chords (with "WAAH!" filter envelope punch!) ----
static void add_supersaw(float *L, float *R, long N, double t0, double dur, const int *notes, int nn,
                         double amp, double fc_peak, double fc_tail, double sc_dep, double sc_per, int env_punch, Rng *rg) {
    long i0 = lround(t0 * SR);
    long n = lround((dur + 0.16) * SR);
    double ph[8][7], dt[8][7], gl[7], gr[7];
    for (int v = 0; v < 7; v++) {
        double pan = (v - 3.0) / 3.0 * 0.90;
        double th = (pan + 1.0) * M_PI / 4.0;
        gl[v] = cos(th); gr[v] = sin(th);
    }
    for (int k = 0; k < nn && k < 8; k++) {
        double f0 = mtof(notes[k]);
        for (int v = 0; v < 7; v++) {
            double cents = -21.0 + 7.0 * v;
            dt[k][v] = f0 * pow(2.0, cents / 1200.0) / SR;
            ph[k][v] = r01(rg);
        }
    }
    Bq fl[2], fr[2];
    static const double Q[2] = {0.5411961, 1.3065630};
    double fc_init = env_punch ? fc_tail * 0.45 : fc_peak;
    for (int j = 0; j < 2; j++) { fl[j] = bq_lp(fc_init, Q[j]); fr[j] = fl[j]; }
    Bq hpL = bq_hp(160.0, 0.7), hpR = bq_hp(160.0, 0.7);
    double norm = 1.0 / (nn * 7.0);
    for (long i = 0; i < n && i0 + i < N; i++) {
        if (i0 + i < 0) continue;
        double t = (double)i / SR;
        if (i % 48 == 0) {
            double fc;
            if (env_punch) {
                // Iconic future-bass chord "WAAH!" bloom: opens in 28 ms, glides down smoothly
                double open = (t < 0.028) ? (t / 0.028) : exp(-(t - 0.028) / fmax(0.12, dur * 0.65));
                fc = fc_tail + (fc_peak - fc_tail) * open;
            } else {
                double u = fmin(1.0, t / fmax(0.05, dur));
                fc = fc_peak * pow(fc_tail / fmax(80.0, fc_peak), u);
            }
            if (fc < 140.0) fc = 140.0;
            if (fc > 14000.0) fc = 14000.0;
            for (int j = 0; j < 2; j++) { Bq c = bq_lp(fc, Q[j]); set_bq(&fl[j], c); set_bq(&fr[j], c); }
        }
        double sl = 0.0, sr = 0.0;
        for (int k = 0; k < nn && k < 8; k++) {
            for (int v = 0; v < 7; v++) {
                double s = 2.0 * ph[k][v] - 1.0 - polyblep(ph[k][v], dt[k][v]);
                sl += s * gl[v]; sr += s * gr[v];
                ph[k][v] += dt[k][v]; if (ph[k][v] >= 1.0) ph[k][v] -= 1.0;
            }
        }
        sl *= norm; sr *= norm;
        for (int j = 0; j < 2; j++) { sl = bq(&fl[j], sl); sr = bq(&fr[j], sr); }
        sl = bq(&hpL, sl); sr = bq(&hpR, sr);
        double env = (t < 0.008 ? t / 0.008 : 1.0) * (t > dur ? fmax(0.0, 1.0 - (t - dur) / 0.16) : 1.0);
        double sc = sc_env(t0 + t, sc_dep, 0.095, sc_per);
        L[i0 + i] += (float)(sl * env * sc * amp);
        R[i0 + i] += (float)(sr * env * sc * amp);
    }
}

// ---- 7. Lush Emotional Chill Electric Piano (Rhodes) ----
static void add_ep_chord(float *L, float *R, long N, double t0, double dur, const int *notes, int nn, double amp) {
    long i0 = lround(t0 * SR);
    long n = lround((dur + 0.40) * SR);
    double norm = 1.0 / sqrt((double)nn);
    for (int k = 0; k < nn; k++) {
        double f = mtof(notes[k]), w = TAU * f / SR;
        double tine_ok = (14.0 * f < 0.45 * SR) ? 1.0 : 0.0;
        for (long i = 0; i < n && i0 + i < N; i++) {
            if (i0 + i < 0) continue;
            double t = (double)i / SR, th = w * i;
            double idx = 0.25 + 1.25 * exp(-t / 0.48);
            double y = sin(th + idx * sin(th)) + tine_ok * 0.065 * sin(14.0 * th) * exp(-t / 0.055);
            double env = (t < 0.006 ? t / 0.006 : 1.0) * exp(-t / 2.2);
            if (t > dur) env *= fmax(0.0, 1.0 - (t - dur) / 0.40);
            double tr = 1.5 * sin(TAU * 4.0 * t);
            L[i0 + i] += (float)(y * env * db2a(tr) * amp * norm);
            R[i0 + i] += (float)(y * env * db2a(-tr) * amp * norm);
        }
    }
}

// ---- 8. Iconic "Quacky / Bubbly" K-Pop Formant Pluck ----
static void add_quack_pluck(float *L, float *R, long N, double t0, double dur, double midi, double amp, double cutoff_hi, double pan) {
    long i0 = lround(t0 * SR);
    double rel = 0.085;
    long n = lround((dur + rel) * SR);
    double f = mtof(midi), dt = f / SR, ph = 0.0;
    Bq lp1 = bq_lp(cutoff_hi, 2.4), lp2 = bq_lp(cutoff_hi, 0.9);
    Bq hp = bq_hp(240.0, 0.7);
    double gl = cos((pan + 1.0) * M_PI / 4.0), gr = sin((pan + 1.0) * M_PI / 4.0);
    for (long i = 0; i < n && i0 + i < N; i++) {
        if (i0 + i < 0) continue;
        double t = (double)i / SR;
        if (i % 16 == 0) {
            double fc = 560.0 + (cutoff_hi - 560.0) * exp(-t / 0.042);
            set_bq(&lp1, bq_lp(fc, 2.3));
            set_bq(&lp2, bq_lp(fc * 1.28, 0.85));
        }
        // Fast bubbly pitch-chirp attack
        double f_inst = f * (1.0 + 0.032 * exp(-t / 0.011));
        dt = f_inst / SR;
        double saw = 2.0 * ph - 1.0 - polyblep(ph, dt);
        double sq = (ph < 0.32 ? 1.0 : -1.0) * 0.40;
        double y = bq(&hp, bq(&lp2, bq(&lp1, (saw + sq) * 0.72)));
        ph += dt; if (ph >= 1.0) ph -= 1.0;
        double env = (t < 0.0025 ? t / 0.0025 : 1.0) * exp(-t / 0.13);
        if (t > dur) env *= fmax(0.0, 1.0 - (t - dur) / rel);
        L[i0 + i] += (float)(y * env * amp * gl);
        R[i0 + i] += (float)(y * env * amp * gr);
    }
}

// ---- 9. Glossy K-Pop Vocal-Chop / Formant Glidy Lead (pitch scoops + vocal formants + ping-pong delay) ----
typedef struct { double t, dur, midi, amp; int scoop; } LeadNote;
static void render_vocal_lead(float *L, float *R, long N, const LeadNote *ev, int nev, double lp_fc) {
    if (nev <= 0) return;
    float *dL = calloc(N, sizeof(float)), *dR = calloc(N, sizeof(float));
    if (!dL || !dR) return;
    double ph1 = 0.0, ph2 = 0.0, ph3 = 0.0;
    double cur_midi = ev[0].midi;
    Bq lp = bq_lp(lp_fc, 0.85), hp = bq_hp(300.0, 0.7);
    // Two vocal formant resonators ("ah/eh" bright K-pop vocal chop sheen)
    Bq fm1 = bq_bp(1080.0, 2.8), fm2 = bq_bp(2550.0, 3.2);
    int idx = 0;
    for (long i = 0; i < N; i++) {
        double t = (double)i / SR;
        while (idx + 1 < nev && t >= ev[idx + 1].t) idx++;
        const LeadNote *e = &ev[idx];
        double dt_note = t - e->t;
        if (dt_note < 0.0 || dt_note > e->dur + 0.08) continue;
        // 22 ms portamento glide + signature upward pitch scoop on accented notes
        cur_midi += (e->midi - cur_midi) * (1.0 - exp(-1.0 / (0.022 * SR)));
        double scoop = e->scoop ? -2.2 * exp(-dt_note / 0.024) : 0.0;
        double vib = (dt_note > 0.12 ? 0.14 * fmin(1.0, (dt_note - 0.12) / 0.14) : 0.0) * sin(TAU * 5.6 * dt_note);
        double f = mtof(cur_midi + scoop + vib);
        double d1 = f * pow(2.0, -9.0 / 1200.0) / SR;
        double d2 = f * pow(2.0, +9.0 / 1200.0) / SR;
        double d3 = f / SR;
        double s1 = 2.0 * ph1 - 1.0 - polyblep(ph1, d1);
        double s2 = 2.0 * ph2 - 1.0 - polyblep(ph2, d2);
        double pulse = (ph3 < 0.36 ? 1.0 : -1.0);
        double raw = 0.44 * (s1 + s2) * 0.5 + 0.36 * pulse + 0.20 * sin(TAU * ph3);
        ph1 += d1; if (ph1 >= 1.0) ph1 -= 1.0;
        ph2 += d2; if (ph2 >= 1.0) ph2 -= 1.0;
        ph3 += d3; if (ph3 >= 1.0) ph3 -= 1.0;
        double body = bq(&hp, bq(&lp, raw));
        double form = 0.85 * bq(&fm1, raw) + 0.65 * bq(&fm2, raw);
        double y = tanh((0.52 * body + 0.68 * form) * 1.2);
        double env = (dt_note < 0.006 ? dt_note / 0.006 : 1.0) *
                     (dt_note > e->dur ? fmax(0.0, 1.0 - (dt_note - e->dur) / 0.08) : 1.0);
        double sc = sc_env(t, 0.32, 0.08, 2.0 * BEAT_S);
        dL[i] = (float)(y * env * sc * e->amp * 0.96);
        dR[i] = (float)(y * env * sc * e->amp * 1.04);
    }
    // 3/16th stereo ping-pong delay (0.28125 s)
    long dly = lround(3.0 * SIX_S * SR);
    for (long i = 0; i < N; i++) {
        float xl = dL[i], xr = dR[i];
        if (i >= dly) { xl += dR[i - dly] * 0.30f; xr += dL[i - dly] * 0.24f; }
        L[i] += xl; R[i] += xr;
    }
    free(dL); free(dR);
}

// ---- 10. Sparkling Bell / Harp Cascade Fill (the iconic 1-bar break!) ----
static void add_bell(float *L, float *R, long N, double t0, double midi, double amp, double pan) {
    long i0 = lround(t0 * SR);
    long n = lround(0.60 * SR);
    double f = mtof(midi), w = TAU * f / SR;
    double gl = cos((pan + 1.0) * M_PI / 4.0), gr = sin((pan + 1.0) * M_PI / 4.0);
    for (long i = 0; i < n && i0 + i < N; i++) {
        if (i0 + i < 0) continue;
        double t = (double)i / SR, th = w * i;
        double y = sin(th + 0.95 * exp(-t / 0.08) * sin(2.0 * th)) * exp(-t / 0.18)
                 + 0.15 * sin(3.0 * th) * exp(-t / 0.045);
        if (t < 0.002) y *= t / 0.002;
        L[i0 + i] += (float)(y * amp * gl);
        R[i0 + i] += (float)(y * amp * gr);
    }
}

static void add_cascade_fill(float *L, float *R, long N, double bar_t, double amp, Rng *rg) {
    // Crisp sparkling pentatonic cascade over beats 1-3 (steps 0..10), breath on 11, snare pickup on 12 & 14!
    static const int steps[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10};
    static const int arp[10]   = {73, 75, 78, 80, 82, 85, 82, 80, 78, 85};
    for (int k = 0; k < 10; k++) {
        double pan = -0.55 + 1.1 * (k / 9.0);
        add_bell(L, R, N, bar_t + steps[k] * SIX_S, arp[k], amp * (0.65 + 0.35 * (k / 9.0)), pan);
    }
    add_snare(L, R, N, bar_t + 12 * SIX_S, 0.24, 2, 0.95, rg);
    add_snare(L, R, N, bar_t + 14 * SIX_S, 0.34, 0, 1.08, rg);
}

// ---- Royal Road 5-note 9th voicings: F#maj9 -> F9 -> A#m9 -> G#m9/C#9 ----
static const int CH_FSHARP[5] = {54, 58, 61, 65, 68};   // F#3 A#3 C#4 F4  G#4
static const int CH_F7[5]     = {53, 57, 60, 63, 67};   // F3  A3  C4  D#4 G4
static const int CH_ASHARP[5] = {58, 61, 65, 68, 72};   // A#3 C#4 F4  G#4 C5
static const int CH_GSHARP[5] = {56, 59, 63, 66, 70};   // G#3 B3  D#4 F#4 A#4
static const int CH_CSHARP[5] = {56, 61, 63, 65, 68};   // G#3 C#4 D#4 F4  G#4

static const int *chord_for(int m, int second_half) {
    if (m == 0) return CH_FSHARP;
    if (m == 1) return second_half ? CH_F7 : CH_FSHARP;
    if (m == 2) return CH_ASHARP;
    return second_half ? CH_CSHARP : CH_GSHARP;
}

static void add_bar_chords(float *L, float *R, long N, int b, double amp, double fc_pk, double fc_tl,
                           double sc_dep, double sc_per, int style, Rng *rg) {
    // style: 0 = sustained supersaw, 1 = Rhodes EP, 2 = bouncy future-bass stabs ("WAAH!")
    double bt = b * BAR_S;
    int m = b % 4;
    if (style == 1) {
        if (m == 0 || m == 2) add_ep_chord(L, R, N, bt, BAR_S * 0.96, chord_for(m, 0), 5, amp);
        else {
            add_ep_chord(L, R, N, bt, 2 * BEAT_S * 0.95, chord_for(m, 0), 5, amp);
            add_ep_chord(L, R, N, bt + 2 * BEAT_S, 2 * BEAT_S * 0.95, chord_for(m, 1), 5, amp);
        }
        return;
    }
    if (style == 2) {
        // Signature K-pop future-bass syncopated chord stabs (hits on 0, 3, 6, 8 [snare!], 11, 14)
        static const double off[6] = {0.0, 3.0 * SIX_S, 6.0 * SIX_S, 8.0 * SIX_S, 11.0 * SIX_S, 14.0 * SIX_S};
        static const double dur[6] = {2.7 * SIX_S, 2.7 * SIX_S, 1.8 * SIX_S, 2.7 * SIX_S, 2.7 * SIX_S, 1.8 * SIX_S};
        for (int k = 0; k < 6; k++) {
            const int *ch = chord_for(m, k >= 3);
            add_supersaw(L, R, N, bt + off[k], dur[k], ch, 5, amp, fc_pk, fc_tl, sc_dep, sc_per, 1, rg);
        }
        return;
    }
    if (m == 0 || m == 2) add_supersaw(L, R, N, bt, BAR_S * 0.98, chord_for(m, 0), 5, amp, fc_pk, fc_tl, sc_dep, sc_per, 0, rg);
    else {
        add_supersaw(L, R, N, bt, 2 * BEAT_S * 0.96, chord_for(m, 0), 5, amp, fc_pk, fc_tl, sc_dep, sc_per, 0, rg);
        add_supersaw(L, R, N, bt + 2 * BEAT_S, 2 * BEAT_S * 0.96, chord_for(m, 1), 5, amp, fc_pk, fc_tl, sc_dep, sc_per, 0, rg);
    }
}

static void push_bar_bass(SubNote *sub, int *nsub, int b, double amp, double sc_dep, double sc_per) {
    double bt = b * BAR_S;
    int m = b % 4;
#define ADD_SUB(t_, d_, mi_) do { if (*nsub < 1024) sub[(*nsub)++] = (SubNote){(t_), (d_), (mi_), amp, sc_dep, sc_per}; } while (0)
    if (m == 0) ADD_SUB(bt, BAR_S * 0.96, 30);
    else if (m == 1) {
        ADD_SUB(bt, 2 * BEAT_S * 0.95, 30);
        ADD_SUB(bt + 2 * BEAT_S, 2 * BEAT_S * 0.95, 29);
    } else if (m == 2) {
        ADD_SUB(bt, 3 * BEAT_S * 0.95, 34);
        ADD_SUB(bt + 3 * BEAT_S, 1 * BEAT_S * 0.93, 37);
    } else {
        ADD_SUB(bt, 2 * BEAT_S * 0.95, 32);
        ADD_SUB(bt + 2 * BEAT_S, 2 * BEAT_S * 0.95, 37);
    }
#undef ADD_SUB
}

// ---- Half-time Future-Bass Drum Pattern helper (160 BPM / 80 BPM feel) ----
static void add_halftime_drums(float *L, float *R, long N, int b, double kick_amp, double sn_amp, double hat_amp, int intense, Rng *rg) {
    double bt = b * BAR_S;
    int odd = b & 1;
    // Bouncy future-bass kicks: step 0, 6, 10 (even bars) and 0, 6, 11, 14 (odd bars)
    add_kick(L, R, N, bt + 0 * SIX_S, kick_amp, 0);
    add_kick(L, R, N, bt + 6 * SIX_S, kick_amp * 0.88, 0);
    if (!odd) {
        add_kick(L, R, N, bt + 10 * SIX_S, kick_amp * 0.94, 0);
    } else {
        add_kick(L, R, N, bt + 11 * SIX_S, kick_amp * 0.92, 0);
        add_kick(L, R, N, bt + 14 * SIX_S, kick_amp * 0.84, 0);
    }
    // HUGE layered snare+clap on Beat 3 (step 8)
    add_snare(L, R, N, bt + 8 * SIX_S, sn_amp, 0, 1.0, rg);
    // Crisp 16th hi-hats + K-pop 32nd-note hat roll on odd bar step 14-15
    for (int s = 0; s < 16; s++) {
        if (!intense && (s % 2 == 1)) continue;
        int op = (s == 4 || s == 12);
        double a = op ? hat_amp * 1.4 : ((s % 2 == 0) ? hat_amp * 0.85 : hat_amp * 0.50);
        add_hat(L, R, N, bt + s * SIX_S, a, op, (s % 2 ? 0.42 : -0.42), rg);
    }
    if (intense && odd) {
        add_hat(L, R, N, bt + 14.5 * SIX_S, hat_amp * 0.65, 0, 0.3, rg);
        add_hat(L, R, N, bt + 15.5 * SIX_S, hat_amp * 0.72, 0, -0.3, rg);
    }
}

int main(int argc, char **argv) {
    const char *out_path = argc > 1 ? argv[1] : "/Users/alex/Music/foolish_cities_v7_sampled/work/seoul11_repro/seoul11_repro.wav";
    long N = lround(TOTAL_S * SR);
    float *L = calloc(N, sizeof(float)), *R = calloc(N, sizeof(float));
    float *busL = calloc(N, sizeof(float)), *busR = calloc(N, sizeof(float)); // reverb bus (synths/leads/bells)
    if (!L || !R || !busL || !busR) { fprintf(stderr, "out of memory\n"); return 1; }
    Rng rg = {REPRO_SEED};

    LeadNote lead[2048]; int nlead = 0;
    SubNote sub[1024]; int nsub = 0;
#define ADD_LEAD(t_, d_, m_, a_, sc_) do { if (nlead < 2048) lead[nlead++] = (LeadNote){(t_), (d_), (m_), (a_), (sc_)}; } while (0)

    // ---- 4-bar Iconic Drop Hook (catchy syncopated vocal-chop lead + bubbly counter-plucks) ----
    static const int drop_lead_step[4][7] = {
        {0, 3, 6, 8, 11, 14, -1},   // bar 0 (F#maj9)
        {0, 3, 6, 8, 12, 14, -1},   // bar 1 (F#maj9 -> F9)
        {0, 3, 6, 8, 10, 12, 14},   // bar 2 (A#m9)
        {0, 4, 6, 8, 11, 14, -1},   // bar 3 (G#m9 -> C#9)
    };
    static const int drop_lead_midi[4][7] = {
        {73, 77, 78, 80, 77, 73, 0}, // C#5 F5 F#5 G#5 F5 C#5
        {75, 77, 78, 82, 80, 77, 0}, // D#5 F5 F#5 A#5 G#5 F5
        {73, 77, 78, 82, 80, 78, 77},// C#5 F5 F#5 A#5 G#5 F#5 F5
        {75, 78, 77, 75, 73, 70, 0}, // D#5 F#5 F5 D#5 C#5 A#4
    };
    static const int quack_step[4][6] = {
        {2, 5, 9, 10, 13, 15},
        {2, 5, 7, 10, 13, 15},
        {2, 4, 7, 11, 13, 15},
        {2, 5, 9, 10, 13, 15},
    };
    static const int quack_midi[4][6] = {
        {70, 73, 78, 77, 73, 70},
        {70, 73, 75, 77, 75, 72},
        {70, 73, 77, 80, 77, 73},
        {71, 75, 78, 77, 75, 73},
    };

    for (int b = 0; b < NBARS; b++) {
        double bt = b * BAR_S;
        int m = b % 4;

        // ============================================================
        // SECTION 1: INTRO & BUILD (Bars 0..11, 0:00 - 0:18)
        // ============================================================
        if (b < 12) {
            if (b == 11) {
                // Bar 12 (16.5-18.0s): dramatic break + sparkling cascade + riser + pickup snare!
                add_cascade_fill(busL, busR, N, bt, 0.14, &rg);
                add_sweep(busL, busR, N, bt, BAR_S, 0.16, 1, &rg);
                continue;
            }
            // Bouncy future-bass chord stabs opening from warm to bright + gliding 808 sub
            double fc_pk = 1800.0 + 480.0 * b;
            add_bar_chords(busL, busR, N, b, 0.34, fc_pk, fc_pk * 0.45, 0.65, 2.0 * BEAT_S, 2, &rg);
            push_bar_bass(sub, &nsub, b, 0.30, 0.70, 2.0 * BEAT_S);
            // Half-time K-pop future-bass drums right from bar 0
            add_halftime_drums(L, R, N, b, 0.50, 0.36, 0.09, b >= 4, &rg);
            // Bubbly/quacky intro melody (warm in bars 0-3, opening bright in bars 4-10)
            double qfc = (b < 4) ? 1650.0 + b * 220.0 : 2600.0 + (b - 4) * 320.0;
            for (int j = 0; j < 6; j++) {
                int st = quack_step[m][j];
                add_quack_pluck(busL, busR, N, bt + st * SIX_S, SIX_S * 1.25, quack_midi[m][j], 0.22, qfc, (j % 2 ? 0.38 : -0.38));
            }
            if (b >= 8) {
                // Pre-drop accelerating snare roll + riser (bars 9-11)
                int step_inc = (b == 8) ? 4 : (b == 9) ? 2 : 1;
                for (int s = 0; s < 16; s += step_inc) {
                    double pm = 0.92 + 0.35 * (((b - 8) * 16 + s) / 48.0);
                    add_snare(L, R, N, bt + s * SIX_S, 0.16 + 0.04 * (b - 8), 2, pm, &rg);
                }
                if (b == 8) add_sweep(busL, busR, N, bt, 3 * BAR_S, 0.16, 1, &rg);
            }
        }
        // ============================================================
        // SECTION 2: DROP 1 - MASSIVE HALF-TIME FUTURE-BASS DROP (Bars 12..27, 0:18 - 0:42)
        // ============================================================
        else if (b < 28) {
            if (b == 12 || b == 20) add_crash(L, R, N, bt, 0.22, &rg);
            if (b == 19 || b == 27) {
                // Iconic 1-bar break with sparkling cascade + snare pickup!
                add_kick(L, R, N, bt, 0.52, 0);
                add_cascade_fill(busL, busR, N, bt, 0.15, &rg);
                continue;
            }
            int lift = (b >= 20) ? 1 : 0;
            // HUGE bright wide future-bass supersaw stabs ("WAAH!")
            add_bar_chords(busL, busR, N, b, lift ? 0.54 : 0.48, lift ? 9600.0 : 7800.0, lift ? 2600.0 : 2100.0, 0.78, 2.0 * BEAT_S, 2, &rg);
            push_bar_bass(sub, &nsub, b, 0.42, 0.80, 2.0 * BEAT_S);
            add_halftime_drums(L, R, N, b, 0.62, 0.48, 0.12, 1, &rg);
            // Glossy K-pop vocal-chop glidy lead (with upward scoops on beat 1 & 3!)
            for (int j = 0; j < 7 && drop_lead_step[m][j] >= 0; j++) {
                int st = drop_lead_step[m][j];
                int nxt = (j + 1 < 7 && drop_lead_step[m][j + 1] >= 0) ? drop_lead_step[m][j + 1] : 16;
                double ndur = (nxt - st) * SIX_S * 0.72;
                int scoop = (st == 0 || st == 8);
                ADD_LEAD(bt + st * SIX_S, ndur, drop_lead_midi[m][j], lift ? 0.24 : 0.22, scoop);
            }
            // Sparkling bubbly counter-plucks weaving between lead phrases
            for (int j = 0; j < 6; j++) {
                int st = quack_step[m][j];
                add_quack_pluck(busL, busR, N, bt + st * SIX_S, SIX_S * 1.1, quack_midi[m][j] + (lift ? 12 : 0),
                                lift ? 0.15 : 0.17, 3800.0, (j % 2 ? -0.48 : 0.48));
            }
        }
        // ============================================================
        // SECTION 3: EMOTIONAL CHILL VERSE (Bars 28..43, 0:42 - 1:06)
        // ============================================================
        else if (b < 44) {
            if (b == 28) add_sweep(busL, busR, N, bt, 2.4, 0.16, 0, &rg);
            if (b == 43) {
                // Bar 44: quiet emotional breath before dreamy breakdown
                add_bar_chords(busL, busR, N, b, 0.14, 900.0, 700.0, 0.0, BAR_S, 1, &rg);
                continue;
            }
            // Lush Rhodes EP + soft low-passed air pad + pillowy gliding 808 sub
            add_bar_chords(busL, busR, N, b, 0.20, 1100.0, 1100.0, 0.0, BAR_S, 1, &rg);
            add_bar_chords(busL, busR, N, b, 0.10, 760.0, 820.0, 0.25, BAR_S, 0, &rg);
            push_bar_bass(sub, &nsub, b, 0.22, 0.35, BAR_S);
            // Laid-back half-time R&B groove (soft kicks on 0, 4, 10; crisp rimshot/snap on beat 3 = step 8)
            add_kick(L, R, N, bt + 0 * SIX_S, 0.44, 1);
            add_kick(L, R, N, bt + 4 * SIX_S, 0.25, 1);
            add_kick(L, R, N, bt + 10 * SIX_S, 0.36, 1);
            add_snare(L, R, N, bt + 8 * SIX_S, 0.28, 1, 1.0, &rg);
            for (int s = 0; s < 16; s += 2) add_hat(L, R, N, bt + s * SIX_S, (s == 8) ? 0.08 : 0.038, 0, (s % 4 ? 0.28 : -0.28), &rg);
            // Tender sparkling bell/vocal ear-candy in bars 36..42
            if (b >= 36) {
                static const int chill_bell[4][3] = {{78, 82, 85}, {77, 80, 84}, {82, 80, 77}, {78, 75, 73}};
                add_bell(busL, busR, N, bt + 0 * SIX_S, chill_bell[m][0], 0.12, -0.35);
                add_bell(busL, busR, N, bt + 6 * SIX_S, chill_bell[m][1], 0.10, 0.35);
                add_bell(busL, busR, N, bt + 12 * SIX_S, chill_bell[m][2], 0.10, 0.0);
            }
        }
        // ============================================================
        // SECTION 4: DREAMY BREAKDOWN & SWELL (Bars 44..55, 1:06 - 1:24)
        // ============================================================
        else if (b < 56) {
            int swell = (b >= 52);
            double fc = swell ? 1100.0 + (b - 52) * 650.0 : 900.0;
            add_bar_chords(busL, busR, N, b, swell ? 0.24 : 0.18, fc, fc * 1.15, swell ? 0.50 : 0.0, BEAT_S, 1, &rg);
            if (swell) {
                add_bar_chords(busL, busR, N, b, 0.18 + 0.05 * (b - 52), fc, fc * 1.2, 0.55, BEAT_S, 0, &rg);
                push_bar_bass(sub, &nsub, b, 0.28, 0.60, BEAT_S);
                for (int k = 0; k < 4; k++) add_kick(L, R, N, bt + k * BEAT_S, 0.40, 1);
                if (b == 52) add_sweep(busL, busR, N, bt, 4 * BAR_S, 0.17, 1, &rg);
            } else {
                add_snare(L, R, N, bt + 8 * SIX_S, 0.17, 1, 1.0, &rg);
                for (int s = 0; s < 16; s += 4) add_hat(L, R, N, bt + s * SIX_S, 0.048, 0, (s % 8 ? 0.3 : -0.3), &rg);
            }
            // Dreamy stereo-panned plucked synth arpeggio
            static const int brk_arp[4][8] = {
                {66, 70, 73, 77, 73, 70, 77, 73},
                {66, 70, 72, 75, 72, 69, 75, 72},
                {70, 73, 77, 80, 77, 73, 80, 77},
                {68, 71, 75, 78, 75, 73, 77, 75},
            };
            for (int j = 0; j < 8; j++) {
                double pan = -0.55 + 1.1 * ((j % 4) / 3.0);
                add_quack_pluck(busL, busR, N, bt + j * 2 * SIX_S, SIX_S * 1.5, brk_arp[m][j], 0.16, swell ? 3000.0 : 2000.0, pan);
                if (j % 2 == 0) add_bell(busL, busR, N, bt + j * 2 * SIX_S, brk_arp[m][j] + 12, 0.085, -pan);
            }
        }
        // ============================================================
        // SECTION 5: DRIVING 4-ON-THE-FLOOR PRE-DROP BUILD (Bars 56..71, 1:24 - 1:48)
        // ============================================================
        else if (b < 72) {
            if (b == 56) add_crash(L, R, N, bt, 0.20, &rg);
            if (b == 71) {
                // Bar 72 (106.5-108.0s): dramatic pre-finale break + sparkling cascade + riser + pickup snare!
                add_cascade_fill(busL, busR, N, bt, 0.16, &rg);
                add_sweep(busL, busR, N, bt, BAR_S, 0.19, 1, &rg);
                continue;
            }
            double prog = (double)(b - 56) / 15.0;
            double fc_pk = 2200.0 + 7200.0 * prog;
            add_bar_chords(busL, busR, N, b, 0.34 + 0.14 * prog, fc_pk, fc_pk * 0.42, 0.72, BEAT_S, 2, &rg);
            push_bar_bass(sub, &nsub, b, 0.34, 0.75, BEAT_S);
            // Driving 4-on-the-floor build kicks (maximizes contrast into the half-time Final Drop!)
            for (int k = 0; k < 4; k++) add_kick(L, R, N, bt + k * BEAT_S, 0.52, 0);
            for (int s = 0; s < 16; s += 2) add_hat(L, R, N, bt + s * SIX_S, (s % 4 == 2) ? 0.13 : 0.06, (s % 4 == 2), (s % 4 == 2 ? 0.38 : -0.38), &rg);
            if (b < 64) {
                add_snare(L, R, N, bt + 8 * SIX_S, 0.38, 0, 1.0, &rg);
            } else {
                // Accelerating pitch-rising EDM/K-pop snare roll (bars 64..70)
                int step_inc = (b < 68) ? 4 : (b < 70) ? 2 : 1;
                for (int s = 0; s < 16; s += step_inc) {
                    double u = ((b - 64) * 16 + s) / (7.0 * 16.0);
                    add_snare(L, R, N, bt + s * SIX_S, 0.18 + 0.18 * u, 2, 0.90 + 0.55 * u, &rg);
                }
                if (b == 64) add_sweep(busL, busR, N, bt, 7 * BAR_S, 0.20, 1, &rg);
            }
            for (int j = 0; j < 6; j++) {
                int st = quack_step[m][j];
                add_quack_pluck(busL, busR, N, bt + st * SIX_S, SIX_S * 1.2, quack_midi[m][j] + (b >= 64 ? 12 : 0),
                                0.16, fc_pk * 0.65, (j % 2 ? 0.42 : -0.42));
            }
        }
        // ============================================================
        // SECTION 6: FINAL DROP - MAXIMUM ENERGY HALF-TIME EXPLOSION (Bars 72..79, 1:48 - 2:00)
        // ============================================================
        else {
            if (b == 72) add_crash(L, R, N, bt, 0.25, &rg);
            if (b == 79) {
                // Final bar 80: sparkling cascade + crisp final chord impact!
                add_kick(L, R, N, bt, 0.54, 0);
                add_cascade_fill(busL, busR, N, bt, 0.16, &rg);
                add_supersaw(busL, busR, N, bt + 14 * SIX_S, 0.75, CH_FSHARP, 5, 0.46, 8600.0, 1800.0, 0.0, BEAT_S, 1, &rg);
                if (nsub < 1024) sub[nsub++] = (SubNote){bt + 14 * SIX_S, 0.75, 30, 0.38, 0.0, BEAT_S};
                continue;
            }
            add_bar_chords(busL, busR, N, b, 0.56, 10200.0, 2800.0, 0.80, 2.0 * BEAT_S, 2, &rg);
            push_bar_bass(sub, &nsub, b, 0.44, 0.82, 2.0 * BEAT_S);
            add_halftime_drums(L, R, N, b, 0.64, 0.50, 0.13, 1, &rg);
            for (int j = 0; j < 7 && drop_lead_step[m][j] >= 0; j++) {
                int st = drop_lead_step[m][j];
                int nxt = (j + 1 < 7 && drop_lead_step[m][j + 1] >= 0) ? drop_lead_step[m][j + 1] : 16;
                double ndur = (nxt - st) * SIX_S * 0.72;
                int scoop = (st == 0 || st == 8);
                ADD_LEAD(bt + st * SIX_S, ndur, drop_lead_midi[m][j], 0.25, scoop);
            }
            for (int j = 0; j < 6; j++) {
                int st = quack_step[m][j];
                add_quack_pluck(busL, busR, N, bt + st * SIX_S, SIX_S * 1.1, quack_midi[m][j] + 12,
                                0.17, 4200.0, (j % 2 ? -0.48 : 0.48));
            }
        }
    }

    // Render continuous gliding 808 sub-bass directly to dry master (keeps low end razor tight!)
    render_sub_bass(L, R, N, sub, nsub);
    // Render glossy K-pop vocal-chop glidy lead into reverb bus
    render_vocal_lead(busL, busR, N, lead, nlead, 5800.0);

    // Lush stereo hall reverb on melodic/synth bus
    Rev rv;
    RevCfg rc = {1.75, 0.88, 12.0, 1.2, 260.0, 7600.0, 0.14, 1.0, 0.2, 0.24};
    rev_init(&rv, rc);
    for (long i = 0; i < N; i++) {
        double wl, wr;
        rev_tick(&rv, busL[i], busR[i], &wl, &wr);
        L[i] += busL[i] + (float)(wl * 4.8);
        R[i] += busR[i] + (float)(wr * 4.8);
    }

    // Master loudness normalize to -13.0 LUFS (matching original se_11) + true-peak ceiling -1.0 dBTP
    double cur_lufs = lufs(L, R, N, NULL, NULL, NULL);
    double g = db2a(-13.0 - cur_lufs);
    for (long i = 0; i < N; i++) { L[i] *= (float)g; R[i] *= (float)g; }
    limit(L, R, N, -1.0, 0.08);
    double tp = a2db(true_peak(L, R, N));

    wav_write24(out_path, L, R, N);
    fprintf(stderr, "seoul11_repro: %.2f s, raw %.1f LUFS -> -13.0 LUFS, tp %.2f dBTP -> %s\n",
            (double)N / SR, cur_lufs, tp, out_path);
    free(L); free(R); free(busL); free(busR);
    return 0;
}
