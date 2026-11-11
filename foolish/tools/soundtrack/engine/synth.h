// synth.h: procedural instruments. Each renders one note into a track's stereo buffer.
// Conventions: n->t start (s), n->dur gate (s), n->pitch MIDI, n->vel 0..1, tr->sp[] instrument
// parameters, n->p[] per-note parameters. Every note owns its Rng (seeded from the note), so
// loop passes are bit-identical.
#pragma once
#include "score.h"

#define SINTAB 65536
static float g_sin[SINTAB + 1];
static void sin_init(void) { static int d; if (d) return; d = 1; for (int i = 0; i <= SINTAB; i++) g_sin[i] = (float)sin(TAU * i / SINTAB); }
static inline double fsin(double ph) { ph -= floor(ph); double x = ph * SINTAB; int i = (int)x; double f = x - i; return g_sin[i] + (g_sin[i + 1] - g_sin[i]) * f; }

static inline void put(float *L, float *R, long nf, long i, double v, double pan) {
    if (i < 0 || i >= nf) return; double a = (pan + 1) * M_PI / 4; L[i] += (float)(v * cos(a) * 1.41421356); R[i] += (float)(v * sin(a) * 1.41421356);
}
static inline void put2(float *L, float *R, long nf, long i, double l, double r) { if (i < 0 || i >= nf) return; L[i] += (float)l; R[i] += (float)r; }
#define NOTE_SPAN(n, maxs) long s0 = lround((n)->t * SR); long len = (long)((maxs) * SR); Rng rg = {(n)->seed}; (void)rg

// ---------------- drums ----------------
// kick: sine with exponential pitch sweep; sp: 0 f_end, 1 f_start, 2 sweep tau, 3 amp tau, 4 click, 5 drive, 6 tail tau, 7 tail level
static void syn_kick(Track *tr, const Note *n, float *L, float *R, long nf) {
    double *p = tr->sp; NOTE_SPAN(n, p[3] * 8 + p[6] * 6 + 0.02);
    double ph = 0; Bq cl = bq_bp(3500, 0.8); double v0 = n->vel;
    for (long i = 0; i < len; i++) {
        double t = (double)i / SR, f = p[0] + (p[1] - p[0]) * exp(-t / p[2]);
        ph += f / SR;
        double env = (t < 0.0015 ? t / 0.0015 : 1) * (exp(-t / p[3]) * (1 - p[7]) + p[7] * exp(-t / fmax(p[6], 1e-3)));
        double v = fsin(ph) * env;
        if (p[4] > 0) v += p[4] * bq(&cl, rn(&rg)) * exp(-t / 0.004) * 1.5;
        if (p[5] > 0) v = tanh(v * (1 + p[5])) / tanh(1 + p[5]);
        put(L, R, nf, s0 + i, v * v0, 0);
    }
}
// 808-style sub with glide: sp 0 amp tau (0 = sustain to gate), 1 drive, 2 overshoot semitones, 3 release s, 4 click
// n->p[0] glide target pitch (0 none), p[1] glide start (s after onset), p[2] glide time (s)
static void syn_808(Track *tr, const Note *n, float *L, float *R, long nf) {
    double *p = tr->sp; double rel = p[3] > 0 ? p[3] : 0.04; NOTE_SPAN(n, n->dur + rel * 6 + 0.01);
    double ph = 0, v0 = n->vel; Op lpf = {0};
    for (long i = 0; i < len; i++) {
        double t = (double)i / SR, m = n->pitch + p[2] * exp(-t / 0.012);
        if (n->p[0] > 0 && t > n->p[1]) { double x = smoothstep((t - n->p[1]) / fmax(n->p[2], 1e-3)); m += (n->p[0] - n->pitch) * x; }
        ph += mtof(m) / SR;
        double env = (t < 0.002 ? t / 0.002 : 1) * (p[0] > 0 ? exp(-t / p[0]) : 1) * (t > n->dur ? exp(-(t - n->dur) / rel) : 1);
        double v = fsin(ph);
        if (p[1] > 0) v = tanh(v * (1 + p[1])) / tanh(1 + p[1]);
        if (p[4] > 0) v += p[4] * op_lp(&lpf, rn(&rg), 2500) * exp(-t / 0.003) * 3;
        put(L, R, nf, s0 + i, v * env * v0, 0);
    }
}
// clap: several noise bursts then a tail; sp 0 centre Hz, 1 Q, 2 tail tau, 3 burst spacing ms, 4 n bursts, 5 hp
static void syn_clap(Track *tr, const Note *n, float *L, float *R, long nf) {
    double *p = tr->sp; NOTE_SPAN(n, p[2] * 8 + 0.06);
    Bq b1 = bq_bp(p[0], p[1]), b2 = bq_bp(p[0] * 1.6, p[1]), h = bq_hp(p[5] > 0 ? p[5] : 500, 0.7);
    int nb = p[4] > 0 ? (int)p[4] : 4; double sp = p[3] > 0 ? p[3] / 1000 : 0.009;
    double jit[8]; for (int k = 0; k < 8; k++) jit[k] = rr(&rg, -0.0015, 0.0015);
    for (long i = 0; i < len; i++) {
        double t = (double)i / SR, env = 0;
        for (int k = 0; k < nb; k++) { double tk = t - (k * sp + (k ? jit[k] : 0)); if (tk >= 0) { double e = k == nb - 1 ? exp(-tk / p[2]) : exp(-tk / 0.0035); if (e > env) env = e; } }
        double x = rn(&rg), v = bq(&h, bq(&b1, x) * 0.8 + bq(&b2, x) * 0.5) * env * 2.2;
        put(L, R, nf, s0 + i, v * n->vel, rr(&rg, -0.1, 0.1) * 0);
    }
}
// electronic snare: two tones + noise; sp 0 tone Hz, 1 tone tau, 2 noise tau, 3 noise level, 4 noise hp, 5 snap
static void syn_snare(Track *tr, const Note *n, float *L, float *R, long nf) {
    double *p = tr->sp; NOTE_SPAN(n, fmax(p[1], p[2]) * 7 + 0.01);
    Bq h = bq_hp(p[4] > 0 ? p[4] : 1500, 0.7), l = bq_lp(9000, 0.7); double ph1 = 0, ph2 = 0;
    for (long i = 0; i < len; i++) {
        double t = (double)i / SR; double f = p[0] * (1 + 0.6 * exp(-t / 0.006));
        ph1 += f / SR; ph2 += f * 1.83 / SR;
        double tone = (fsin(ph1) + 0.5 * fsin(ph2)) * exp(-t / p[1]);
        double nz = bq(&l, bq(&h, rn(&rg))) * exp(-t / p[2]) * p[3];
        double snap = p[5] * rn(&rg) * exp(-t / 0.002);
        put(L, R, nf, s0 + i, (tone * 0.7 + nz + snap) * n->vel * (t < 0.001 ? t / 0.001 : 1), 0);
    }
}
// hats: six detuned squares (the 808 ratios) plus noise, high-passed; sp 0 decay tau, 1 freq scale, 2 noise mix, 3 hp Hz, 4 tone bp Hz, 5 choke at gate end, 6 attack s
// n->p[0] > 0 overrides the decay
static void syn_hat(Track *tr, const Note *n, float *L, float *R, long nf) {
    double *p = tr->sp; double tau = n->p[0] > 0 ? n->p[0] : p[0]; NOTE_SPAN(n, tau * 7 + 0.005);
    static const double fr[6] = {205.3, 304.4, 369.6, 522.7, 540.0, 800.0};
    double ph[6] = {0}; for (int k = 0; k < 6; k++) ph[k] = r01(&rg);
    Bq b = bq_bp(p[4] > 0 ? p[4] : 10000, 1.2), h1 = bq_hp(p[3], 0.7), h2 = bq_hp(p[3], 0.7);
    for (long i = 0; i < len; i++) {
        double t = (double)i / SR, m = 0;
        for (int k = 0; k < 6; k++) { ph[k] += fr[k] * p[1] / SR; ph[k] -= floor(ph[k]); m += ph[k] < 0.5 ? 1 : -1; }
        double x = bq(&b, m / 6) * (1 - p[2]) + rn(&rg) * p[2];
        double env = (t < (p[6] > 0 ? p[6] : 0.0005) ? t / (p[6] > 0 ? p[6] : 0.0005) : 1) * exp(-t / tau);
        if (p[5] > 0 && t > n->dur) env *= exp(-(t - n->dur) / 0.006);   // choke: a closed hat cuts the open one
        put(L, R, nf, s0 + i, bq(&h2, bq(&h1, x)) * env * n->vel * 1.6, 0);
    }
}
// shaker: band noise with a soft rise; sp 0 centre, 1 q, 2 attack s, 3 decay tau
static void syn_shaker(Track *tr, const Note *n, float *L, float *R, long nf) {
    double *p = tr->sp; NOTE_SPAN(n, p[2] + p[3] * 7);
    Bq b = bq_bp(p[0], p[1]), h = bq_hp(3000, 0.7);
    double att = p[2] * rr(&rg, 0.8, 1.2);
    for (long i = 0; i < len; i++) {
        double t = (double)i / SR, env = t < att ? pow(t / att, 1.5) : exp(-(t - att) / p[3]);
        put(L, R, nf, s0 + i, bq(&h, bq(&b, rn(&rg))) * env * n->vel * 2.5, 0);
    }
}
// rim / stick click: sp 0 f1, 1 f2, 2 tau
static void syn_rim(Track *tr, const Note *n, float *L, float *R, long nf) {
    double *p = tr->sp; NOTE_SPAN(n, p[2] * 8);
    Bq b = bq_bp(p[0], 8), b2 = bq_bp(p[1], 5);
    for (long i = 0; i < len; i++) { double t = (double)i / SR, x = rn(&rg) * exp(-t / 0.0015);
        double v = bq(&b, x) * 4 + bq(&b2, x) * 3 + 0.6 * (fsin(p[0] * t) + 0.5 * fsin(p[1] * t)) * exp(-t / p[2]);
        put(L, R, nf, s0 + i, v * n->vel, 0); }
}

// ---------------- tuned percussion / plucks ----------------
// mallet: tuned-bar modes (1 : 3.93 : 9.2) plus an optional filtered-saw pluck layer
// sp 0 decay tau @ C4, 1 brightness, 2 pluck mix, 3 pluck env tau, 4 mallet noise, 5 stereo spread, 6 pluck cutoff mult
static void syn_mallet(Track *tr, const Note *n, float *L, float *R, long nf) {
    double *p = tr->sp; double f = mtof(n->pitch), tau = p[0] * pow(261.6 / f, 0.5);
    double gate_rel = 0.06; NOTE_SPAN(n, fmin(tau * 6, n->dur + gate_rel * 8) + 0.01);
    double r[3] = {1, 3.93, 9.2}, a[3] = {1, 0.35 * p[1], 0.12 * p[1]}, d[3] = {tau, tau / 4.5, tau / 11};
    double ph[3] = {0}, sph = 0, sph2 = 0.37; Svf sv = {0}; Bq nb = bq_bp(fmin(f * 5, 12000), 1.5);
    double pan = n->p[3];
    for (long i = 0; i < len; i++) {
        double t = (double)i / SR, v = 0;
        for (int k = 0; k < 3; k++) { if (f * r[k] > 18000) continue; ph[k] += f * r[k] / SR; v += a[k] * fsin(ph[k]) * exp(-t / d[k]); }
        if (p[2] > 0) { // pluck: two slightly detuned saws through a falling lowpass
            double dt = f / SR; sph += dt; if (sph >= 1) sph -= 1; sph2 += dt * 1.004; if (sph2 >= 1) sph2 -= 1;
            double s = osc_saw(sph, dt) + osc_saw(sph2, dt * 1.004);
            double fc = f * (1.2 + p[6] * exp(-t / p[3]));
            v += p[2] * svf(&sv, s * 0.5, fc, 0.9, 0, 0) * exp(-t / (tau * 0.8));
        }
        if (p[4] > 0) v += p[4] * bq(&nb, rn(&rg)) * exp(-t / 0.003) * 2;
        double g = t > n->dur ? exp(-(t - n->dur) / gate_rel) : 1;
        double env = (t < 0.0012 ? t / 0.0012 : 1) * g;
        put(L, R, nf, s0 + i, v * env * n->vel * 0.5, pan);
    }
}
// steel pan: partials 1, 2, 3 with the octave swelling just after the strike
// sp 0 decay tau, 1 octave level, 2 twelfth level, 3 stick noise
static void syn_steelpan(Track *tr, const Note *n, float *L, float *R, long nf) {
    double *p = tr->sp; double f = mtof(n->pitch); double rel = 0.08; NOTE_SPAN(n, fmin(p[0] * 6, n->dur + rel * 8));
    double ph[4] = {0}; Bq nb = bq_lp(2500, 0.7);
    for (long i = 0; i < len; i++) {
        double t = (double)i / SR;
        ph[0] += f / SR; ph[1] += f * 2.004 / SR; ph[2] += f * 3.01 / SR; ph[3] += f * 4.02 / SR;
        double swell = 1 - exp(-t / 0.025);
        double v = fsin(ph[0]) * exp(-t / p[0]) + p[1] * swell * fsin(ph[1]) * exp(-t / (p[0] * 0.7))
                 + p[2] * fsin(ph[2]) * exp(-t / (p[0] * 0.35)) + 0.08 * fsin(ph[3]) * exp(-t / (p[0] * 0.2));
        v += p[3] * bq(&nb, rn(&rg)) * exp(-t / 0.004) * 3;
        double g = t > n->dur ? exp(-(t - n->dur) / rel) : 1;
        put(L, R, nf, s0 + i, v * g * (t < 0.001 ? t / 0.001 : 1) * n->vel * 0.45, n->p[3]);
    }
}
// flute / pan pipe: few harmonics, tonal breath, delayed vibrato, a chiff at the start, optional glide from the last note
// sp 0 breath, 1 vibrato cents, 2 attack s, 3 h2, 4 h3, 5 glide s (n->p[2] = semitones to glide from), 6 chiff level (default 0.5), 7 h5
static void syn_flute(Track *tr, const Note *n, float *L, float *R, long nf) {
    double *p = tr->sp; double f0 = mtof(n->pitch); double rel = 0.09; NOTE_SPAN(n, n->dur + rel * 7);
    double gl = p[5] > 0 ? p[5] : 0.07, chl = p[6] > 0 ? p[6] : 0.5;
    double ph = r01(&rg); Bq b1 = bq_bp(f0, 12), b2 = bq_bp(f0 * 2, 12), bh = bq_hp(2500, 0.7), ch = bq_bp(f0 * 3, 2);
    double vr = rr(&rg, 4.6, 5.4), vph = r01(&rg);
    for (long i = 0; i < len; i++) {
        double t = (double)i / SR;
        double vib = p[1] / 1200 * smoothstep((t - 0.25) / 0.4) * sin(TAU * (vr * t + vph));
        double glide = n->p[2] != 0 ? n->p[2] / 12 * (1 - smoothstep(t / gl)) : 0;
        double f = f0 * pow(2, vib + glide); ph += f / SR;
        double tone = fsin(ph) + p[3] * fsin(2 * ph + 0.3) + p[4] * fsin(3 * ph + 0.7) + p[7] * fsin(5 * ph + 1.1);
        double env = adsr(t, n->dur, p[2], 0.3, 0.85, rel);
        double nz = rn(&rg);
        double breath = p[0] * (bq(&b1, nz) * 6 + bq(&b2, nz) * 3 + bq(&bh, nz) * 0.15);
        double chiff = chl * bq(&ch, nz) * exp(-t / 0.02) * (t < 0.06) * (n->p[2] != 0 ? 0.3 : 1);
        put(L, R, nf, s0 + i, (tone * env * (0.9 + 0.1 * fsin(3.1 * t)) + breath * env + chiff) * n->vel * 0.4, n->p[3]);
    }
}

// ---------------- pads, bass ----------------
// pad: detuned saw stack through a lowpass; sp 0 voices, 1 detune cents, 2 cutoff mult (x f0), 3 attack, 4 release, 5 q, 6 min cutoff Hz, 7 square mix
static void syn_pad(Track *tr, const Note *n, float *L, float *R, long nf) {
    double *p = tr->sp; int nv = p[0] > 0 ? (int)p[0] : 5; double f0 = mtof(n->pitch);
    NOTE_SPAN(n, n->dur + p[4] * 6);
    double ph[16], dt[16], pan[16]; Svf sl = {0}, sr2 = {0};
    for (int k = 0; k < nv; k++) { ph[k] = r01(&rg); double c = nv > 1 ? p[1] * (2.0 * k / (nv - 1) - 1) : 0; dt[k] = f0 * pow(2, c / 1200) / SR; pan[k] = nv > 1 ? (2.0 * k / (nv - 1) - 1) * 0.8 : 0; }
    double fc = fmax(p[6], f0 * p[2]);
    for (long i = 0; i < len; i++) {
        double t = (double)i / SR, l = 0, r = 0;
        for (int k = 0; k < nv; k++) { ph[k] += dt[k]; if (ph[k] >= 1) ph[k] -= 1; double s = osc_saw(ph[k], dt[k]); if (p[7] > 0) s = s * (1 - p[7]) + p[7] * osc_sq(ph[k], dt[k], 0.5);
            double a = (pan[k] + 1) * M_PI / 4; l += s * cos(a); r += s * sin(a); }
        double env = adsr(t, n->dur, p[3], 1.0, 1.0, p[4]);
        l = svf(&sl, l, fc, p[5] > 0 ? p[5] : 0.7, 0, 0); r = svf(&sr2, r, fc, p[5] > 0 ? p[5] : 0.7, 0, 0);
        double g = env * n->vel * 0.35 / sqrt(nv);
        put2(L, R, nf, s0 + i, l * g, r * g);
    }
}
// sub: sine with a little 2nd harmonic; sp 0 attack, 1 release, 2 h2, 3 drive, 4 amp tau (0 = flat)
static void syn_sub(Track *tr, const Note *n, float *L, float *R, long nf) {
    double *p = tr->sp; NOTE_SPAN(n, n->dur + p[1] * 7);
    double ph = 0, f = mtof(n->pitch);
    for (long i = 0; i < len; i++) {
        double t = (double)i / SR; ph += f / SR;
        double v = fsin(ph) + p[2] * fsin(2 * ph);
        if (p[3] > 0) v = tanh(v * (1 + p[3])) / tanh(1 + p[3]);
        double env = adsr(t, n->dur, p[0], 1, 1, p[1]) * (p[4] > 0 ? exp(-t / p[4]) : 1);
        put(L, R, nf, s0 + i, v * env * n->vel, 0);
    }
}

// ---------------- noise FX ----------------
// riser: band noise sweeping up, amplitude rising over the note; sp 0 f_lo, 1 f_hi, 2 q
// revcym: n->p[0] = 1 gives a reverse-cymbal shape (rising exp ending hard at the note end)
static void syn_riser(Track *tr, const Note *n, float *L, float *R, long nf) {
    double *p = tr->sp; NOTE_SPAN(n, n->dur + 0.3);
    Svf a = {0}, b = {0};
    for (long i = 0; i < len; i++) {
        double t = (double)i / SR, x = clampd(t / n->dur, 0, 1);
        double fc = p[0] * pow(p[1] / p[0], x * x);
        double env = n->p[0] > 0 ? (t < n->dur ? pow(x, 3) : exp(-(t - n->dur) / 0.01)) : (t < n->dur ? x * x : exp(-(t - n->dur) / 0.08));
        double bpl, bpr; svf(&a, rn(&rg), fc, p[2], &bpl, 0); svf(&b, rn(&rg), fc, p[2], &bpr, 0);
        put2(L, R, nf, s0 + i, bpl * env * n->vel, bpr * env * n->vel);
    }
}

// chirp: the "bed squeak" style pitched blip; sp 0 f start, 1 f end, 2 sweep s, 3 decay tau, 4 harmonic 2 level
static void syn_chirp(Track *tr, const Note *n, float *L, float *R, long nf) {
    double *p = tr->sp; NOTE_SPAN(n, p[3] * 7 + p[2]);
    double ph = 0, k = n->pitch > 0 ? mtof(n->pitch) / mtof(81) : 1; // pitch shifts the whole squeak
    for (long i = 0; i < len; i++) {
        double t = (double)i / SR, x = clampd(t / p[2], 0, 1), f = (p[0] + (p[1] - p[0]) * sin(x * M_PI / 2)) * k;
        ph += f / SR;
        double env = (t < 0.003 ? t / 0.003 : 1) * exp(-t / p[3]);
        put(L, R, nf, s0 + i, (fsin(ph) + p[4] * fsin(2 * ph)) * env * n->vel * 0.5, n->p[3]);
    }
}
// saw synth: polyBLEP saw/square stack through an enveloped lowpass (plucks, stabs, leads)
// sp 0 voices, 1 detune cents, 2 cutoff base mult, 3 env amount mult, 4 env tau, 5 q, 6 amp decay tau (0 = hold), 7 release, 8 square mix, 9 attack
static void syn_saw(Track *tr, const Note *n, float *L, float *R, long nf) {
    double *p = tr->sp; int nv = p[0] > 0 ? (int)p[0] : 2; double f0 = mtof(n->pitch), rel = p[7] > 0 ? p[7] : 0.05;
    NOTE_SPAN(n, n->dur + rel * 7);
    double ph[8], dt[8], pn[8]; Svf a = {0}, b = {0};
    for (int k = 0; k < nv; k++) { ph[k] = r01(&rg); double c = nv > 1 ? p[1] * (2.0 * k / (nv - 1) - 1) : 0; dt[k] = f0 * pow(2, c / 1200) / SR; pn[k] = nv > 1 ? (2.0 * k / (nv - 1) - 1) * 0.7 : 0; }
    for (long i = 0; i < len; i++) {
        double t = (double)i / SR, l = 0, r = 0;
        for (int k = 0; k < nv; k++) { ph[k] += dt[k]; if (ph[k] >= 1) ph[k] -= 1; double s = osc_saw(ph[k], dt[k]) * (1 - p[8]) + p[8] * osc_sq(ph[k], dt[k], 0.5);
            double ang = (pn[k] + 1) * M_PI / 4; l += s * cos(ang); r += s * sin(ang); }
        double fc = f0 * (p[2] + p[3] * exp(-t / p[4]));
        l = svf(&a, l, fc, p[5] > 0 ? p[5] : 0.8, 0, 0); r = svf(&b, r, fc, p[5] > 0 ? p[5] : 0.8, 0, 0);
        double att = p[9] > 0 ? p[9] : 0.002;
        double env = (t < att ? t / att : 1) * (p[6] > 0 ? exp(-t / p[6]) : 1) * (t > n->dur ? exp(-(t - n->dur) / rel) : 1);
        double g = env * n->vel * 0.4 / sqrt(nv);
        put2(L, R, nf, s0 + i, l * g, r * g);
    }
}

// supersaw: up to 16 detuned saws spread linearly in pitch and across the stereo field, random start phases,
// a sub-octave saw, an enveloped lowpass and saturation (research: 7-16 voices, 10-30 cents, 70-100% spread)
// sp 0 voices, 1 detune cents (total, edge to edge), 2 centre-voice boost, 3 cutoff mult (x f0), 4 env amount mult, 5 env tau,
//    6 attack s, 7 decay tau (0 = none), 8 sustain level, 9 release s, 10 sub level, 11 drive, 12 stereo spread 0..1, 13 q, 14 min cutoff Hz
static void syn_supersaw(Track *tr, const Note *n, float *L, float *R, long nf) {
    double *p = tr->sp; int nv = p[0] > 0 ? (int)fmin(16, p[0]) : 9; double f0 = mtof(n->pitch), rel = p[9] > 0 ? p[9] : 0.2;
    NOTE_SPAN(n, n->dur + rel * 6);
    double ph[16], dt[16], gl[16], gr[16], sph = 0;
    for (int k = 0; k < nv; k++) { double x = nv > 1 ? 2.0 * k / (nv - 1) - 1 : 0; ph[k] = r01(&rg); dt[k] = f0 * pow(2, x * p[1] / 2 / 1200) / SR;
        double pan = x * (p[12] > 0 ? p[12] : 0.9) * ((k & 1) ? 1 : -1) * (k & 2 ? 0.7 : 1); double a = (pan + 1) * M_PI / 4; double w = (fabs(x) < 1e-9 ? 1 + p[2] : 1); gl[k] = cos(a) * w; gr[k] = sin(a) * w; }
    Svf a1 = {0}, a2 = {0}, b1 = {0}, b2 = {0}; double aal = 0, aar = 0;
    double norm = 1 / sqrt(nv);
    for (long i = 0; i < len; i++) {
        double t = (double)i / SR, l = 0, r = 0;
        for (int k = 0; k < nv; k++) { ph[k] += dt[k]; if (ph[k] >= 1) ph[k] -= 1; double s = osc_saw(ph[k], dt[k]); l += s * gl[k]; r += s * gr[k]; }
        if (p[10] > 0) { double d = f0 * 0.5 / SR; sph += d; if (sph >= 1) sph -= 1; double s = osc_saw(sph, d) * p[10] * sqrt(nv); l += s; r += s; }
        l *= norm; r *= norm;
        double fc = fmax(p[14], f0 * (p[3] + p[4] * exp(-t / fmax(p[5], 1e-3))));
        double q = p[13] > 0 ? p[13] : 0.7;
        l = svf(&a2, svf(&a1, l, fc, q, 0, 0), fc, 0.6, 0, 0); r = svf(&b2, svf(&b1, r, fc, q, 0, 0), fc, 0.6, 0, 0);
        if (p[11] > 0) { double k = tanh(1 + p[11]); l = tanh_aa(&aal, l * (1 + p[11])) / k; r = tanh_aa(&aar, r * (1 + p[11])) / k; }
        double att = p[6] > 0 ? p[6] : 0.003;
        double env = (t < att ? t / att : 1) * (p[7] > 0 ? p[8] + (1 - p[8]) * exp(-(t - att) / p[7]) : 1) * (t > n->dur ? exp(-(t - n->dur) / rel) : 1);
        double g = env * n->vel * 0.45;
        put2(L, R, nf, s0 + i, l * g, r * g);
    }
}

// vinyl: hiss plus sparse crackles and pops for the length of the note (deterministic from the note's seed)
// sp 0 hiss level, 1 crackles per second, 2 pop level, 3 rumble level
static void syn_vinyl(Track *tr, const Note *n, float *L, float *R, long nf) {
    double *p = tr->sp; NOTE_SPAN(n, n->dur);
    Bq h = bq_bp(5000, 0.5), cr = bq_hp(1500, 0.7), cr2 = bq_hp(1500, 0.7), rb = bq_lp(40, 0.7); double pop = 0, popl = 0, popr = 0;
    for (long i = 0; i < len; i++) {
        double hiss = bq(&h, rn(&rg)) * p[0];
        if (r01(&rg) < p[1] / SR) { pop = rr(&rg, 0.2, 1) * p[2] * (r01(&rg) < 0.1 ? 3 : 1); popl = rr(&rg, 0.5, 1); popr = rr(&rg, 0.5, 1); }
        double c = pop * rn(&rg); pop *= 0.82;
        double rum = bq(&rb, rn(&rg)) * p[3] * 30;
        put2(L, R, nf, s0 + i, hiss + bq(&cr, c * popl) + rum, hiss * 0.9 + bq(&cr2, c * popr) + rum);
    }
}

// guitar: Karplus-Strong string with a pick-position comb, a damping loop filter and a small body resonator set
// sp 0 decay (loop gain 0.990-0.9995), 1 brightness (loop low-pass 0..1), 2 pick position (0..0.5), 3 body level, 4 mute (palm-mute 0..1)
static void syn_guitar(Track *tr, const Note *n, float *L, float *R, long nf) {
    double *p = tr->sp; double f = mtof(n->pitch), rel = 0.05; NOTE_SPAN(n, n->dur + 0.4);
    double period = SR / f; int D = (int)period; double frac = period - D; if (D < 4) return;
    float *buf = calloc(D + 4, sizeof(float));
    // excitation: noise burst shaped by velocity, pick-position comb applied by subtracting a delayed copy
    int pk = (int)(D * (p[2] > 0 ? p[2] : 0.18)); Bq ex = bq_lp(1500 + 6000 * n->vel * (p[1] > 0 ? p[1] : 0.6), 0.7);
    for (int i = 0; i < D; i++) buf[i] = (float)bq(&ex, rn(&rg));
    for (int i = D - 1; i >= pk; i--) buf[i] -= buf[i - pk];
    double g = p[0] > 0 ? p[0] : 0.996, br = p[1] > 0 ? p[1] : 0.6, ap = (1 - frac) / (1 + frac), apx = 0, apy = 0, last = 0;
    Bq b1 = bq_peak(110, 2, 6), b2 = bq_peak(220, 2, 4), b3 = bq_peak(420, 1.5, 3), hp = bq_hp(80, 0.7);
    int pos = 0;
    for (long i = 0; i < len; i++) {
        double t = (double)i / SR;
        double x = buf[pos], nx = buf[(pos + 1) % D];
        double lp = (1 - br) * 0.5 * (x + nx) + br * x;   // damping filter: blend of a 2-tap average and the raw sample
        double y = ap * (lp - apy) + apx; apx = lp; apy = y; // fractional delay allpass
        double damp = t > n->dur ? 0.96 : g - p[4] * 0.02;
        buf[pos] = (float)(y * damp); pos = (pos + 1) % D;
        double out = bq(&hp, x); out += p[3] * (bq(&b1, out) + bq(&b2, out) + bq(&b3, out)) * 0.3;
        last = out;
        double env = (t < 0.001 ? t / 0.001 : 1) * (t > n->dur ? exp(-(t - n->dur) / rel) : 1);
        put(L, R, nf, s0 + i, out * env * n->vel * 0.6, n->p[3]);
    }
    (void)last; free(buf);
}
