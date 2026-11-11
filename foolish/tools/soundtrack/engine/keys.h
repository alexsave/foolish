// keys.h: additive piano, FM electric piano, percussive organ.
#pragma once
#include "voice.h"

// ---------------- sustain pedal ----------------
// a track's pedal is a list of [down, up) intervals in seconds, filled by the renderer from cc 64
typedef struct { double t0, t1; } Ped;
static Ped *g_ped[MAXTR]; static int g_nped[MAXTR];
// time the damper actually falls for a key released at t (s)
static double damper_time(int tr, double t) {
    for (int i = 0; i < g_nped[tr]; i++) if (t >= g_ped[tr][i].t0 && t < g_ped[tr][i].t1) return g_ped[tr][i].t1;
    return t;
}

// ---------------- piano ----------------
// Each string's partials are stretched by inharmonicity B, weighted by a hammer spectrum (hardness grows with
// velocity) and a strike-position comb, and decay in two stages (prompt + aftersound from the coupled unison
// strings, which also beat against each other). Dampers fall at key-up or pedal-up; the top notes have none.
// sp: 0 brightness (hammer, 0..2), 1 decay scale, 2 unison detune cents, 3 hammer noise, 4 thump, 5 stereo width,
//     6 release tau s, 7 aftersound weight, 8 max partial Hz, 9 strike position (fraction, ~0.12), 10 velocity curve exp
static void syn_piano(Track *tr, const Note *n, float *L, float *R, long nf) {
    double *p = tr->sp; double m = n->pitch, f0 = mtof(m), v = n->vel;
    int ti = (int)(tr - g_tr);
    double off = n->t + n->dur, damp = m >= 89 ? 1e9 : damper_time(ti, off);
    double rel = (p[6] > 0 ? p[6] : 0.12) * (m < 48 ? 1.6 : 1);
    double tau1 = (p[1] > 0 ? p[1] : 1) * 2.8 * pow(261.6 / f0, 0.65); if (tau1 > 14) tau1 = 14;
    double natural = tau1 * 4 * 4.6; // aftersound to -80 dB-ish
    double end = fmin(natural, damp - n->t + rel * 7); if (end < 0.05) end = 0.05;
    NOTE_SPAN(n, end + 0.01);
    double B = 0.00015 * exp(0.047 * (m - 60)); if (B < 2e-5) B = 2e-5;
    int nstr = m < 32 ? 1 : m < 44 ? 2 : 3;
    // the felt is a nonlinear low-pass: its corner rises with velocity (~1.7 kHz at pp to ~7 kHz+ at ff) and its slope
    // is steeper when soft (research: KTH hammer pages; corner values are a starting point)
    double hard = (p[0] > 0 ? p[0] : 1);
    double fch = (900 + 4800 * v * v) * hard + 2.0 * f0, slope = 2.2 + 2.0 * (1 - v);
    Rng nr = {(uint64_t)(m * 7919) ^ 0x5eed}; double dv = rr(&nr, 0.7, 1.35);   // per-key decay character, the same every strike
    double x0 = p[9] > 0 ? p[9] : 0.122;
    double maxf = p[8] > 0 ? p[8] : 16000;
    double vel = pow(v, p[10] > 0 ? p[10] : 1.7);
    double pan = clampd((m - 64) / 40.0 * (p[5] > 0 ? p[5] : 0.6), -0.9, 0.9);
    double ca = (pan + 1) * M_PI / 4, gl = cos(ca) * 1.41421356, gr = sin(ca) * 1.41421356;
    enum { KMAX = 72 };
    double fk[KMAX][3], ak[KMAX], t1[KMAX], t2[KMAX], awk[KMAX], ph[KMAX][3]; int K = 0;
    double aw = p[7] > 0 ? p[7] : 0.22;
    for (int k = 1; k < KMAX; k++) {
        double f = k * f0 * sqrt(1 + B * k * k); if (f > maxf || f > SR * 0.45) break;
        double a = fabs(sin(M_PI * k * x0)) / pow(k, 1.0) / (1 + pow(f / fch, slope));
        a *= 1 / (1 + pow(70 / f, 4)); // the soundboard radiates little below ~70 Hz
        ak[K] = a; t1[K] = dv * tau1 / (1 + pow(f / 1800, 1.3)) / (1 + 0.15 * (k - 1) / 10.0); t2[K] = t1[K] * 4.5 / (1 + f / 3000); awk[K] = (p[7] > 0 ? p[7] : 0.22) / (1 + f / 1000);   // the slow aftersound lives in the low partials
        for (int s = 0; s < nstr; s++) { double c = nstr == 1 ? 0 : (s - (nstr - 1) / 2.0) * (p[2] > 0 ? p[2] : 0.5) * rr(&nr, 0.4, 1.6); fk[K][s] = f * pow(2, c / 1200); ph[K][s] = r01(&rg); }
        K++;
    }
    double norm = 0; for (int k = 0; k < K; k++) norm += ak[k] * ak[k]; norm = 0.5 / sqrt(norm + 1e-12);
    Bq hn = bq_bp(fmin(2500 + 2000 * hard, 9000), 0.8), th = bq_lp(160, 0.7);
    double ex[KMAX]; // running envelopes, updated every 16 samples
    for (long i = 0; i < len; i++) {
        double t = (double)i / SR, s = 0;
        double dg = (n->t + t) > damp ? exp(-((n->t + t) - damp) / rel) : 1;
        if ((i & 15) == 0) for (int k = 0; k < K; k++) ex[k] = ak[k] * ((1 - awk[k]) * exp(-t / t1[k]) + awk[k] * exp(-t / t2[k]));
        for (int k = 0; k < K; k++) {
            if (ex[k] < 1e-5) continue;
            double o = 0; for (int st = 0; st < nstr; st++) { ph[k][st] += fk[k][st] / SR; if (ph[k][st] >= 1) ph[k][st] -= 1; o += fsin(ph[k][st]); }
            s += ex[k] * o / nstr;
        }
        s *= norm;
        double nz = rn(&rg);
        s += p[3] * bq(&hn, nz) * exp(-t / 0.006) * v * 0.3;           // felt-on-string noise
        s += p[4] * bq(&th, nz) * exp(-t / 0.03) * v * 1.2;            // key/soundboard thump
        double a = (t < 0.0015 ? t / 0.0015 : 1) * dg * vel;
        put2(L, R, nf, s0 + i, s * a * gl, s * a * gr);
    }
}

// ---------------- FM electric piano (tine-and-tonebar sketch) ----------------
// sp: 0 index at strike, 1 index decay tau, 2 tine level, 3 decay tau @C4, 4 release, 5 tremolo depth, 6 tremolo hz, 7 bark (velocity -> index)
static void syn_epiano(Track *tr, const Note *n, float *L, float *R, long nf) {
    // the tremolo runs on absolute time (both tines of the stereo pan share it); in loop mode its rate is rounded to a whole
    // number of cycles per loop, or the two passes disagree at the wrap
    double tr_hz = tr->sp[6]; if (g_period_s > 0) tr_hz = fmax(1, lround(tr_hz * g_period_s)) / g_period_s;
    double *p = tr->sp; double f = mtof(n->pitch), v = n->vel, rel = p[4] > 0 ? p[4] : 0.15;
    double tau = p[3] * pow(261.6 / f, 0.5);
    NOTE_SPAN(n, fmin(n->dur + rel * 7, tau * 6));
    double pc = r01(&rg), pm = 0, pt = 0, I0 = p[0] * (1 + p[7] * v);
    for (long i = 0; i < len; i++) {
        double t = (double)i / SR;
        double I = I0 * exp(-t / p[1]) + 0.25;
        pm += f / SR; pt += f * 14.1 / SR; pc += f / SR;
        double mod = I * fsin(pm), car = fsin(pc + mod / TAU);
        double tine = p[2] * fsin(pt) * exp(-t / 0.02) * v;
        double env = (t < 0.002 ? t / 0.002 : 1) * exp(-t / tau) * (t > n->dur ? exp(-(t - n->dur) / rel) : 1);
        double trem = 1 - p[5] * (0.5 + 0.5 * sin(TAU * tr_hz * (n->t + t)));
        double trem2 = 1 - p[5] * (0.5 - 0.5 * sin(TAU * tr_hz * (n->t + t)));
        double s = (car + tine) * env * (0.4 + 0.6 * v) * 0.4;
        put2(L, R, nf, s0 + i, s * trem, s * trem2);
    }
}

// ---------------- percussive organ (M1-style bass/stab organ sketch) ----------------
// drawbar-like harmonics, a fast decay to a held level, key click, a little percussion on the 3rd harmonic
// sp: 0 decay tau, 1 sustain level, 2 click, 3 brightness (upper harmonic scale), 4 release, 5 drive,
//     6 square a fifth up (level), 7 24 dB low-pass Hz (0 = off), 8 pitch-envelope semitones (fast blip down)
static void syn_organ(Track *tr, const Note *n, float *L, float *R, long nf) {
    double *p = tr->sp; double f = mtof(n->pitch), rel = p[4] > 0 ? p[4] : 0.03;
    NOTE_SPAN(n, n->dur + rel * 7);
    double sqph = 0; Svf l1 = {0}, l2 = {0};
    static const double hr[7] = {1, 2, 3, 4, 5, 6, 8}, hl[7] = {1.0, 0.75, 0.55, 0.32, 0.12, 0.22, 0.15};
    double ph[7]; for (int k = 0; k < 7; k++) ph[k] = r01(&rg);
    Bq ck = bq_bp(2800, 1.2);
    for (long i = 0; i < len; i++) {
        double t = (double)i / SR, s = 0;
        double fe = f * pow(2, p[8] / 12 * exp(-t / 0.006));
        for (int k = 0; k < 7; k++) { if (fe * hr[k] > 16000) continue; ph[k] += fe * hr[k] / SR; double a = hl[k] * (k ? p[3] : 1); if (k == 2) a *= 1 + 1.5 * exp(-t / 0.08); s += a * fsin(ph[k]); }
        if (p[6] > 0) { double dt = fe * 1.4983 / SR; sqph += dt; if (sqph >= 1) sqph -= 1; s += p[6] * osc_sq(sqph, dt, 0.5); }
        if (p[7] > 0) s = svf(&l2, svf(&l1, s, p[7], 0.54, 0, 0), p[7], 1.31, 0, 0);
        double env = (t < 0.002 ? t / 0.002 : 1) * (p[1] + (1 - p[1]) * exp(-t / p[0])) * (t > n->dur ? exp(-(t - n->dur) / rel) : 1);
        s = s * env * 0.35;
        if (p[5] > 0) s = tanh(s * (1 + p[5])) / (1 + p[5] * 0.3);
        s += p[2] * bq(&ck, rn(&rg)) * exp(-t / 0.0025) * n->vel;
        put(L, R, nf, s0 + i, s * n->vel, 0);
    }
}
