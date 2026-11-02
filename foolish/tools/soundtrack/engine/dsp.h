// dsp.h: filters, oscillators, randomness. Shared by the synths, the mixer and the analyser.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR 48000
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define TAU (2 * M_PI)

// ---- deterministic randomness: every note owns a seed, so loop passes render identically ----
typedef struct { uint64_t s; } Rng;
static inline uint64_t mix64(uint64_t z) { z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL; z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL; return z ^ (z >> 31); }
static inline double r01(Rng *r) { r->s += 0x9e3779b97f4a7c15ULL; return (mix64(r->s) >> 11) * (1.0 / 9007199254740992.0); }
static inline double rr(Rng *r, double a, double b) { return a + (b - a) * r01(r); }
static inline double rn(Rng *r) { return 2 * r01(r) - 1; }
static inline double rgauss(Rng *r) { double u = r01(r) + 1e-12, v = r01(r); return sqrt(-2 * log(u)) * cos(TAU * v); }
static inline double db2a(double d) { return pow(10, d / 20); }
static inline double a2db(double a) { return 20 * log10(fabs(a) + 1e-20); }
static inline double mtof(double m) { return 440.0 * pow(2, (m - 69) / 12); }
static inline double clampd(double x, double a, double b) { return x < a ? a : x > b ? b : x; }

// ---- biquads (RBJ cookbook) ----
typedef struct { double b0, b1, b2, a1, a2, x1, x2, y1, y2; } Bq;
static Bq bq_norm(double b0, double b1, double b2, double a0, double a1, double a2) { Bq f = {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0, 0, 0, 0, 0}; return f; }
static Bq bq_lp(double fc, double q) { double w = TAU * fc / SR, al = sin(w) / (2 * q), c = cos(w); return bq_norm((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al); }
static Bq bq_hp(double fc, double q) { double w = TAU * fc / SR, al = sin(w) / (2 * q), c = cos(w); return bq_norm((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al); }
static Bq bq_bp(double fc, double q) { double w = TAU * fc / SR, al = sin(w) / (2 * q), c = cos(w); return bq_norm(al, 0, -al, 1 + al, -2 * c, 1 - al); }
static Bq bq_peak(double fc, double q, double gdb) { double A = pow(10, gdb / 40), w = TAU * fc / SR, al = sin(w) / (2 * q), c = cos(w);
    return bq_norm(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A); }
static Bq bq_lshelf(double fc, double gdb) { double A = pow(10, gdb / 40), w = TAU * fc / SR, c = cos(w), s = sin(w), al = s / 2 * sqrt(2), sa = 2 * sqrt(A) * al;
    return bq_norm(A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sa), (A + 1) + (A - 1) * c + sa, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa); }
static Bq bq_hshelf(double fc, double gdb) { double A = pow(10, gdb / 40), w = TAU * fc / SR, c = cos(w), s = sin(w), al = s / 2 * sqrt(2), sa = 2 * sqrt(A) * al;
    return bq_norm(A * ((A + 1) + (A - 1) * c + sa), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sa), (A + 1) - (A - 1) * c + sa, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sa); }
static inline double bq(Bq *f, double x) { double y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2; f->x2 = f->x1; f->x1 = x; f->y2 = f->y1; f->y1 = y; return y; }

// ---- state-variable filter (Chamberlin/Simper TPT), modulatable cutoff ----
typedef struct { double ic1, ic2; } Svf;
// returns lowpass; *bp and *hp optional
static inline double svf(Svf *s, double x, double fc, double q, double *bpo, double *hpo) {
    fc = clampd(fc, 10, SR * 0.45);
    double g = tan(M_PI * fc / SR), k = 1 / q, a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
    double v3 = x - s->ic2, v1 = a1 * s->ic1 + a2 * v3, v2 = s->ic2 + a2 * s->ic1 + a3 * v3;
    s->ic1 = 2 * v1 - s->ic1; s->ic2 = 2 * v2 - s->ic2;
    if (bpo) *bpo = v1;
    if (hpo) *hpo = x - k * v1 - v2;
    return v2;
}

// ---- one-pole ----
typedef struct { double y; } Op;
static inline double op_lp(Op *o, double x, double fc) { double a = 1 - exp(-TAU * fc / SR); o->y += a * (x - o->y); return o->y; }

// ---- antiderivative anti-aliased tanh (first order): the saturation's harmonics above Nyquist are smoothed away instead of
// folding back as inharmonic fizz; state is the previous input ----
static inline double lncosh(double x) { double a = fabs(x); return a > 18 ? a - 0.69314718056 : log(cosh(x)); }
static inline double tanh_aa(double *st, double x) {
    double x0 = *st, d = x - x0; *st = x;
    return fabs(d) < 1e-6 ? tanh(0.5 * (x + x0)) : (lncosh(x) - lncosh(x0)) / d;
}
// ---- band-limited oscillators (polyBLEP) ----
static inline double blep(double t, double dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1; }
    if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
    return 0;
}
static inline double osc_saw(double ph, double dt) { return 2 * ph - 1 - blep(ph, dt); }
static inline double osc_sq(double ph, double dt, double pw) {
    double v = ph < pw ? 1 : -1; v += blep(ph, dt); double p2 = ph - pw; if (p2 < 0) p2 += 1; v -= blep(p2, dt); return v; }

// ---- envelopes ----
// attack-decay-sustain-release with exponential segments; t since note on, dur = gate length
static inline double adsr(double t, double dur, double a, double d, double s, double r) {
    double v;
    double tt = t < dur ? t : dur;
    if (tt < a) v = tt / a; else v = s + (1 - s) * exp(-(tt - a) / fmax(d, 1e-4));
    if (t >= dur) v *= exp(-(t - dur) / fmax(r, 1e-4));
    return v;
}
static inline double smoothstep(double x) { x = clampd(x, 0, 1); return x * x * (3 - 2 * x); }
