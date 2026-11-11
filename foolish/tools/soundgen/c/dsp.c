// dsp.c - see dsp.h.
#include "dsp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rng.h"

#define PI_D 3.14159265358979323846

// ---- random draws ------------------------------------------------------------
void rng_init(Rng *r, uint64_t seed) { r->s = rng_seed64(seed); r->has = 0; r->spare = 0; }
static double unit(Rng *r) { return rng_unit(rng_splitmix64(&r->s)); }
double uni(Rng *r, double a, double b) { return a + (b - a) * unit(r); }
double normal(Rng *r) {
    if (r->has) { r->has = 0; return r->spare; }
    double u = 0;
    while (u <= 1e-300) u = unit(r);
    const double v = unit(r), m = sqrt(-2 * log(u));
    r->spare = m * sin(2 * PI_D * v); r->has = 1;
    return m * cos(2 * PI_D * v);
}
int irange(Rng *r, int lo, int hi) { int k = lo + (int)(unit(r) * (hi - lo)); return k >= hi ? hi - 1 : k; }
int poisson(Rng *r, double lam) {
    const double L = exp(-lam); double p = 1; int k = 0;
    do { k++; p *= unit(r); } while (p > L);
    return k - 1;
}

// ---- signals -----------------------------------------------------------------
Sig sig_new(int n) { Sig s = { calloc((size_t)(n > 0 ? n : 1), sizeof(double)), n }; return s; }
void sig_free(Sig *s) { free(s->x); s->x = 0; s->n = 0; }
Sig noise(Rng *r, int n) { Sig s = sig_new(n); for (int i = 0; i < n; i++) s.x[i] = normal(r); return s; }
double sig_rms(const double *x, int n) { double q = 0; for (int i = 0; i < n; i++) q += x[i] * x[i]; return n ? sqrt(q / n) : 0; }
double sig_peak(const double *x, int n) { double p = 0; for (int i = 0; i < n; i++) if (fabs(x[i]) > p) p = fabs(x[i]); return p; }
int sig_argmax_abs(const Sig *s) { int k = 0; for (int i = 1; i < s->n; i++) if (fabs(s->x[i]) > fabs(s->x[k])) k = i; return k; }
void sig_scale(Sig *s, double k) { for (int i = 0; i < s->n; i++) s->x[i] *= k; }
void sig_add(Sig *d, const Sig *s, double k) { for (int i = 0; i < d->n && i < s->n; i++) d->x[i] += k * s->x[i]; }

// ---- the FFT: iterative radix 2, complex, in place --------------------------
static int pow2ge(int n) { int p = 1; while (p < n) p <<= 1; return p; }
static void fft(double *re, double *im, int n, int inverse) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double a = (inverse ? 2 : -2) * PI_D / len;
        const double wr = cos(a), wi = sin(a);
        for (int i = 0; i < n; i += len) {
            double cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                const int p = i + k, q = p + len / 2;
                const double tr = re[q] * cr - im[q] * ci, ti = re[q] * ci + im[q] * cr;
                re[q] = re[p] - tr; im[q] = im[p] - ti;
                re[p] += tr; im[p] += ti;
                const double nr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = nr;
            }
        }
    }
    if (inverse) for (int i = 0; i < n; i++) { re[i] /= n; im[i] /= n; }
}

// The FFT of x (n samples) padded to N, its bins multiplied by gain(f), back.
typedef double (*GainFn)(double f, const void *ctx);
static void filter(double *x, int n, int N, GainFn gain, const void *ctx) {
    double *re = calloc((size_t)N, sizeof(double)), *im = calloc((size_t)N, sizeof(double));
    memcpy(re, x, sizeof(double) * (size_t)n);
    fft(re, im, N, 0);
    for (int k = 0; k <= N / 2; k++) {
        const double g = gain((double)k * SR / N, ctx);
        re[k] *= g; im[k] *= g;
        if (k > 0 && k < N / 2) { re[N - k] *= g; im[N - k] *= g; }
    }
    fft(re, im, N, 1);
    memcpy(x, re, sizeof(double) * (size_t)n);
    free(re); free(im);
}
// Room for a zero-phase response to ring either side of the signal without
// wrapping back into it: 170 ms.
#define PAD 8192

// ---- spectra -----------------------------------------------------------------
double interp(double x, const double *xp, const double *fp, int n, double left, double right) {
    if (x < xp[0]) return left;
    if (x > xp[n - 1]) return right;
    int lo = 0, hi = n - 1;
    while (hi - lo > 1) { const int m = (lo + hi) / 2; if (xp[m] <= x) lo = m; else hi = m; }
    if (xp[hi] == xp[lo]) return fp[hi];
    return fp[lo] + (fp[hi] - fp[lo]) * (x - xp[lo]) / (xp[hi] - xp[lo]);
}
double tmpl_db(double f, const double *cs, const double *tmpl, int n) {
    const double lf = log(f < 1 ? 1 : f);
    if (lf <= log(cs[0])) return tmpl[0];
    if (lf >= log(cs[n - 1])) return tmpl[n - 1];
    int lo = 0, hi = n - 1;
    while (hi - lo > 1) { const int m = (lo + hi) / 2; if (log(cs[m]) <= lf) lo = m; else hi = m; }
    const double a = log(cs[lo]), b = log(cs[hi]);
    return tmpl[lo] + (tmpl[hi] - tmpl[lo]) * (lf - a) / (b - a);
}
typedef struct { const double *cs, *tmpl; int n; double lo, hi; } EqCtx;
static double eq_gain(double f, const void *p) {
    const EqCtx *c = p;
    double g = tmpl_db(f, c->cs, c->tmpl, c->n);
    if (f < c->lo || (c->hi > 0 && f > c->hi)) g = -60;
    return pow(10, g / 20);
}
void eq(Sig *y, const double *cs, const double *tmpl, int n, double lo, double hi) {
    const EqCtx c = { cs, tmpl, n, lo, hi };
    filter(y->x, y->n, pow2ge(y->n + PAD), eq_gain, &c);
}
Sig colored_noise(Rng *r, int n, const double *cs, const double *tmpl, int nt, double lo, double hi) {
    // Circular, as numpy's: noise the length of the FFT, filtered round it.
    const int N = pow2ge(n);
    Sig z = noise(r, N);
    const EqCtx c = { cs, tmpl, nt, lo, hi };
    filter(z.x, N, N, eq_gain, &c);
    z.n = n;
    return z;
}
typedef struct { double lo, hi; } BpCtx;
static double bp_gain(double f, const void *p) { const BpCtx *c = p; return (f < c->lo || f > c->hi) ? 0 : 1; }
void bp(Sig *y, double lo, double hi) { const BpCtx c = { lo, hi }; filter(y->x, y->n, pow2ge(y->n + PAD), bp_gain, &c); }
Sig bp_copy(const Sig *y, double lo, double hi) {
    Sig z = sig_new(y->n); memcpy(z.x, y->x, sizeof(double) * (size_t)y->n); bp(&z, lo, hi); return z;
}

// ---- envelopes and windows ---------------------------------------------------
void moving_rms(const double *x, int n, int w, double *out) {
    // out[i] = sqrt(mean(x[i - w/2 .. i + (w-1)/2]^2)), zeros past the ends:
    // numpy's convolve(..., "same") with a box of w.
    double *c = malloc(sizeof(double) * (size_t)(n + 1));
    c[0] = 0;
    for (int i = 0; i < n; i++) c[i + 1] = c[i] + x[i] * x[i];
    for (int i = 0; i < n; i++) {
        int a = i - w / 2, b = i + (w - 1) / 2 + 1;
        if (a < 0) a = 0;
        if (b > n) b = n;
        const double v = (c[b] - c[a]) / w;
        out[i] = v > 0 ? sqrt(v) : 0;
    }
    free(c);
}
double max_moving_rms(const Sig *y, int w, int from, int to) {
    double *e = malloc(sizeof(double) * (size_t)y->n), m = 0;
    moving_rms(y->x, y->n, w, e);
    if (from < 0) from = 0;
    if (to > y->n) to = y->n;
    for (int i = from; i < to; i++) if (e[i] > m) m = e[i];
    free(e);
    return m;
}

void burst_add(Rng *r, Sig *y, double at_s, double db, double width, double rise, double mult) {
    const int k = (int)(at_s * SR), L = (int)(width * mult * SR) + 8;
    const double tau = width / 2.3, g = pow(10, db / 20);
    for (int i = 0; i < L; i++) {
        const double t = (double)i / SR, v = normal(r) * (t / rise < 1 ? t / rise : 1) * exp(-t / tau) * g;
        if (k + i >= 0 && k + i < y->n) y->x[k + i] += v;
    }
}

Sig reson(const Sig *x, double hz, const double *freqs, double bw) {
    Sig y = sig_new(x->n);
    const double r = exp(-PI_D * bw / SR);
    double y1 = 0, y2 = 0, c = 2 * r * cos(2 * PI_D * hz / SR);
    for (int i = 0; i < x->n; i++) {
        if (freqs) c = 2 * r * cos(2 * PI_D * freqs[i] / SR);
        const double v = (1 - r) * x->x[i] + c * y1 - r * r * y2;
        y.x[i] = v; y2 = y1; y1 = v;
    }
    return y;
}

// ---- rates ---------------------------------------------------------------------
Sig resample_3_2(const Sig *x) {
    const int m = (int)((long)x->n * 2 / 3), half = 48;   // taps either side, at 48 kHz
    const double fc = 15600.0 / SR;                       // cycles per input sample
    Sig y = sig_new(m);
    for (int j = 0; j < m; j++) {
        const double p = j * 1.5;
        const int k0 = (int)floor(p) - half, k1 = (int)floor(p) + half;
        double acc = 0;
        for (int k = k0; k <= k1; k++) {
            if (k < 0 || k >= x->n) continue;
            const double d = p - k;
            const double s = d == 0 ? 2 * fc : sin(2 * PI_D * fc * d) / (PI_D * d);
            const double w = 0.42 + 0.5 * cos(PI_D * d / (half + 1)) + 0.08 * cos(2 * PI_D * d / (half + 1));   // Blackman
            acc += x->x[k] * s * w;
        }
        y.x[j] = acc;
    }
    return y;
}
