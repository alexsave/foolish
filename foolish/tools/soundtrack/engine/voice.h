// voice.h: an additive formant singer. Harmonics of a vibrating f0 are weighted by a
// fixed-in-Hz formant envelope (the vowel), so pitch moves while the vowel stays put, as
// a real voice does. An ensemble is several such singers with their own detune, vibrato
// and onset scatter.
#pragma once
#include "synth.h"

// Formant tables: [voice][vowel a e i o u][formant 0..4] = {freq, dB, bw}
// voice 0 bass, 1 tenor, 2 alto, 3 soprano (the classic Csound singing-formant table)
static const double VF[4][5][5][3] = {
 { // bass
  {{600,0,60},{1040,-7,70},{2250,-9,110},{2450,-9,120},{2750,-20,130}},
  {{400,0,40},{1620,-12,80},{2400,-9,100},{2800,-12,120},{3100,-18,120}},
  {{250,0,60},{1750,-30,90},{2600,-16,100},{3050,-22,120},{3340,-28,120}},
  {{400,0,40},{750,-11,80},{2400,-21,100},{2600,-20,120},{2900,-40,120}},
  {{350,0,40},{600,-20,80},{2400,-32,100},{2675,-28,120},{2950,-36,120}}},
 { // tenor
  {{650,0,80},{1080,-6,90},{2650,-7,120},{2900,-8,130},{3250,-22,140}},
  {{400,0,70},{1700,-14,80},{2600,-12,100},{3200,-14,120},{3580,-20,120}},
  {{290,0,40},{1870,-15,90},{2800,-18,100},{3250,-20,120},{3540,-30,120}},
  {{400,0,40},{800,-10,80},{2600,-12,100},{2800,-12,120},{3000,-26,120}},
  {{350,0,40},{600,-20,60},{2700,-17,100},{2900,-14,120},{3300,-26,120}}},
 { // alto
  {{800,0,80},{1150,-4,90},{2800,-20,120},{3500,-36,130},{4950,-60,140}},
  {{400,0,60},{1600,-24,80},{2700,-30,120},{3300,-35,150},{4950,-60,200}},
  {{350,0,50},{1700,-20,100},{2700,-30,120},{3700,-36,150},{4950,-60,200}},
  {{450,0,70},{800,-9,80},{2830,-16,100},{3500,-28,130},{4950,-55,135}},
  {{325,0,50},{700,-12,60},{2530,-30,170},{3500,-40,180},{4950,-64,200}}},
 { // soprano
  {{800,0,80},{1150,-6,90},{2900,-32,120},{3900,-20,130},{4950,-50,140}},
  {{350,0,60},{2000,-20,100},{2800,-15,120},{3600,-40,150},{4950,-56,200}},
  {{270,0,60},{2140,-12,90},{2950,-26,100},{3900,-26,120},{4950,-44,120}},
  {{450,0,40},{800,-11,80},{2830,-22,100},{3800,-22,120},{4950,-50,120}},
  {{325,0,50},{700,-16,60},{2700,-35,170},{3800,-40,180},{4950,-60,200}}},
};

static void vowel_at(int vt, double vw, double fshift, double F[5], double A[5], double B[5]) {
    vw = clampd(vw, 0, 4); int a = (int)vw; if (a > 3) a = 3; double x = vw - a;
    for (int k = 0; k < 5; k++) {
        F[k] = (VF[vt][a][k][0] * (1 - x) + VF[vt][a + 1][k][0] * x) * fshift;
        A[k] = db2a(VF[vt][a][k][1] * (1 - x) + VF[vt][a + 1][k][1] * x);
        B[k] = (VF[vt][a][k][2] * (1 - x) + VF[vt][a + 1][k][2] * x) * fmax(1, sqrt(fshift));
    }
}
static inline double fenv(double f, const double F[5], const double A[5], const double B[5], double floor_) {
    double e = floor_;
    for (int k = 0; k < 5; k++) { double d = (f * f - F[k] * F[k]) / (f * B[k] * 1.6); e += A[k] / sqrt(1 + d * d); }
    return e;
}

// sp: 0 voice type, 1 formant shift, 2 ensemble size, 3 detune cents, 4 vib rate, 5 vib cents, 6 breath,
//     7 attack s, 8 release s, 9 tilt dB/oct, 10 scoop cents, 11 scoop s, 12 vib delay s, 13 glide s,
//     14 onset scatter s, 15 brightness boost dB at 2.5-4 kHz (singer's formant), 16 max harmonic Hz
// n->p: 0 vowel start, 1 vowel end (<0 same), 2 glide-from semitones (0 none), 3 pan
static void syn_voice(Track *tr, const Note *n, float *L, float *R, long nf) {
    double *p = tr->sp; sin_init();
    int vt = (int)p[0]; double fsh = p[1] > 0 ? p[1] : 1; int nv = p[2] > 0 ? (int)p[2] : 1;
    double rel = p[8] > 0 ? p[8] : 0.12, att = p[7] > 0 ? p[7] : 0.05;
    NOTE_SPAN(n, n->dur + rel * 6 + (p[14] > 0 ? p[14] : 0));
    double v0 = n->p[0], v1 = n->p[1] < 0 ? v0 : n->p[1];
    double hmax = p[16] > 0 ? p[16] : 9000;
    for (int vi = 0; vi < nv; vi++) {
        double det = nv > 1 ? rgauss(&rg) * p[3] / 1200 : 0;
        double vr = p[4] * rr(&rg, 0.9, 1.1), vph = r01(&rg), vd = p[5] * rr(&rg, 0.8, 1.2);
        double off = nv > 1 ? r01(&rg) * p[14] : 0;
        double pan = nv > 1 ? clampd(n->p[3] + rr(&rg, -0.6, 0.6), -1, 1) : n->p[3];
        double ca = (pan + 1) * M_PI / 4, gl = cos(ca), grr = sin(ca);
        double ph = r01(&rg), jit = 0; Op jl = {0}, sl = {0};
        long o = (long)(off * SR);
        Bq nb1, nb2, nb3; double Fv[5], Av[5], Bv[5]; vowel_at(vt, v0, fsh, Fv, Av, Bv);
        nb1 = bq_bp(Fv[0], 3); nb2 = bq_bp(Fv[1], 4); nb3 = bq_hp(2500, 0.7);
        double amp[128], amp_prev[128]; int nh = 0; for (int k = 0; k < 128; k++) amp[k] = amp_prev[k] = 0;
        double norm = 1;
        for (long i = 0; i < len; i++) {
            double t = (double)i / SR;
            double m = n->pitch + det * 12;
            if (n->p[2] != 0) m += n->p[2] * (1 - smoothstep(t / (p[13] > 0 ? p[13] : 0.08)));
            if (p[10] != 0) m -= p[10] / 100 * (1 - smoothstep(t / (p[11] > 0 ? p[11] : 0.06)));
            jit = op_lp(&jl, rgauss(&rg), 18) ; double shim = op_lp(&sl, rgauss(&rg), 9);
            double vib = vd / 100 * smoothstep((t - p[12]) / 0.3) * sin(TAU * (vr * t + vph));
            m += vib + jit * 0.08;
            double f0 = mtof(m);
            if ((i & 31) == 0) { // refresh the harmonic weights from the formant envelope
                double x = n->dur > 0 ? clampd(t / n->dur, 0, 1) : 0;
                vowel_at(vt, v0 + (v1 - v0) * smoothstep(x), fsh, Fv, Av, Bv);
                nh = (int)fmin(127, hmax / f0);
                double tot = 0;
                for (int k = 1; k <= nh; k++) {
                    double f = k * f0, a = fenv(f, Fv, Av, Bv, 0.003) * pow(k, -0.6) * pow(2, -p[9] / 6 * log2(k));
                    if (p[15] != 0 && f > 2300 && f < 4200) a *= db2a(p[15]);
                    if (f > hmax * 0.8) a *= clampd((hmax - f) / (hmax * 0.2), 0, 1);
                    amp_prev[k] = amp[k]; amp[k] = a; tot += a * a;
                }
                if (i == 0) { norm = 1 / sqrt(tot + 1e-9); for (int k = 1; k <= nh; k++) amp_prev[k] = amp[k]; }
            }
            ph += f0 / SR; ph -= floor(ph);
            double fr = (i & 31) / 32.0, v = 0;
            for (int k = 1; k <= nh; k++) { double x = k * ph; v += (amp_prev[k] + (amp[k] - amp_prev[k]) * fr) * fsin(x); }
            v *= norm * (1 + shim * 0.04);
            double nz = rn(&rg);
            double br = p[6] * (bq(&nb1, nz) * 0.6 + bq(&nb2, nz) * 0.5 + bq(&nb3, nz) * 0.08);
            double env = adsr(t, n->dur, att, 1, 1, rel);
            double s = (v + br) * env * n->vel * 0.5 / sqrt(nv);
            long j = s0 + o + i; if (j >= 0 && j < nf) { L[j] += (float)(s * gl); R[j] += (float)(s * grr); }
        }
    }
}

// scratch: a sung "ahh"/horn-ish source read through a playback-rate curve, the way a DJ's record hand and fader move it
// n->p[0]: type 0 baby (forward-back), 1 chirp (fader cuts at the turnarounds), 2 transformer (forward, fader chops in 32nds)
// sp 0 source vowel (0..4), 1 source brightness, 2 gain
static void syn_scratch(Track *tr, const Note *n, float *L, float *R, long nf) {
    double *p = tr->sp; NOTE_SPAN(n, n->dur + 0.02);
    // build 0.8 s of source: a vowel at the note's pitch with breathy noise, plus a short attack transient
    long sn = (long)(0.8 * SR); float *src = calloc(sn, sizeof(float));
    double F[5], A[5], B[5]; vowel_at(1, p[0], 1.0, F, A, B); double f0 = mtof(n->pitch), ph = 0;
    int nh = (int)fmin(60, 8000 / f0); double amp[64]; for (int k = 1; k <= nh; k++) amp[k] = fenv(k * f0, F, A, B, 0.003) * pow(k, -0.4 + 0.3 * p[1]);
    Bq nb = bq_bp(F[1], 2);
    for (long i = 0; i < sn; i++) { double t = (double)i / SR; ph += f0 * (1 - 0.06 * t) / SR; double v = 0; for (int k = 1; k <= nh; k++) v += amp[k] * fsin(k * ph); v = v * 0.25 + bq(&nb, rn(&rg)) * 0.3;
        src[i] = (float)(v * (t < 0.01 ? t / 0.01 : 1) * exp(-t / 0.6)); }
    int type = (int)n->p[0]; double pos = 0.02 * SR, T = n->dur;
    for (long i = 0; i < len; i++) {
        double t = (double)i / SR, x = t / T, rate, g = 1;
        if (type == 2) { rate = 1.0; g = fmod(t * 8 / (60.0 / bpm_at(n->beat)), 1.0) < 0.5 ? 1 : 0; }   // fader chops in 32nds
        else { rate = 2.2 * sin(TAU * x); if (type == 1) g = fabs(sin(TAU * x)) > 0.35 ? 1 : 0; }      // one forward-back stroke per note
        pos += rate; if (pos < 0) pos = 0; if (pos >= sn - 2) pos = sn - 2;
        long a = (long)pos; double fr = pos - a, v = src[a] * (1 - fr) + src[a + 1] * fr;
        double env = (t < 0.003 ? t / 0.003 : 1) * (t > T ? exp(-(t - T) / 0.005) : 1);
        double s = v * g * env * n->vel * (p[2] > 0 ? p[2] : 1) * (0.5 + 0.5 * fabs(rate) / 2.2);
        put(L, R, nf, s0 + i, s, 0);
    }
    free(src);
}
