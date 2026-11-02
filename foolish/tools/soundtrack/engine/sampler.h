// sampler.h: plays recorded instrument samples (free libraries under samples/, read as data only).
// A sample set is described by a manifest of lines "root_midi layer rr path" (layer 0 = softest).
// Notes pick the nearest root (resampled with cubic interpolation), the velocity layer, and a round-robin
// from the note's seed; leading silence is trimmed at load; notes longer than the sample loop a crossfaded
// stretch of its sustain; note-off fades with the track's release.
#pragma once
#include "keys.h"
#include "mix.h"

typedef struct { int root, layer, rr; double sr; float *L, *R; long n; double att; char path[600]; } Smp;
typedef struct { Smp *s; int n, nl; double gain[16]; char name[64]; } SampleSet;

static void smp_trim(Smp *x) {   // drop leading silence (keep 1.5 ms before the first sample over -50 dBFS of the peak)
    double pk = 1e-9; for (long i = 0; i < x->n; i++) pk = fmax(pk, fmax(fabsf(x->L[i]), fabsf(x->R[i])));
    long a = 0; while (a < x->n && fabsf(x->L[a]) < pk * 0.003 && fabsf(x->R[a]) < pk * 0.003) a++;
    a -= (long)(0.0015 * x->sr); if (a < 0) a = 0;
    if (a > 0) { memmove(x->L, x->L + a, (x->n - a) * sizeof(float)); memmove(x->R, x->R + a, (x->n - a) * sizeof(float)); x->n -= a; }
    // the bow's own swell: time for the 10 ms RMS to come within 3 dB of the sustain level (the median of the 30-80% stretch)
    long w = (long)(0.01 * x->sr), nw = x->n / w; x->att = 0; if (nw < 10) return;
    double *rm = malloc(nw * sizeof(double)), *sorted = malloc(nw * sizeof(double));
    for (long k = 0; k < nw; k++) { double e = 0; for (long i = k * w; i < (k + 1) * w; i++) e += x->L[i] * x->L[i] + x->R[i] * x->R[i]; rm[k] = sqrt(e / (2 * w)); }
    long k0 = nw * 3 / 10, k1 = nw * 8 / 10, m = k1 - k0; memcpy(sorted, rm + k0, m * sizeof(double));
    for (long i = 1; i < m; i++) for (long j = i; j > 0 && sorted[j - 1] > sorted[j]; j--) { double t = sorted[j]; sorted[j] = sorted[j - 1]; sorted[j - 1] = t; }
    double sus = sorted[m / 2]; long k = 0; while (k < nw && rm[k] < sus * 0.708) k++;
    x->att = k < nw ? (double)k * w / x->sr : 0; free(rm); free(sorted);
}
static SampleSet *ss_load(const char *manifest) {
    FILE *f = fopen(manifest, "r"); if (!f) { perror(manifest); exit(1); }
    SampleSet *ss = calloc(1, sizeof *ss); ss->s = calloc(4096, sizeof(Smp)); snprintf(ss->name, 64, "%s", manifest);
    char line[800]; g_wav_quiet = 1;
    while (fgets(line, sizeof line, f)) {
        Smp x = {0}; char path[600]; if (line[0] == '#' || sscanf(line, "%d %d %d %599[^\n]", &x.root, &x.layer, &x.rr, path) != 4) continue;
        snprintf(x.path, sizeof x.path, "%s", path);
        x.n = 0; x.L = x.R = 0;   // loaded on first use
        if (x.layer + 1 > ss->nl) ss->nl = x.layer + 1;
        ss->s[ss->n++] = x;
    }
    fclose(f); g_wav_quiet = 0;
    // normalise layers so the loudest layer's median peak is 1 and softer layers keep their natural level
    for (int l = 0; l < ss->nl; l++) ss->gain[l] = 1;
    fprintf(stderr, "sample set %s: %d samples, %d layers\n", manifest, ss->n, ss->nl);
    return ss;
}
static inline double cubic(const float *x, long n, double pos) {
    long i = (long)pos; double t = pos - i;
    double y0 = i > 0 ? x[i - 1] : x[0], y1 = i < n ? x[i] : 0, y2 = i + 1 < n ? x[i + 1] : 0, y3 = i + 2 < n ? x[i + 2] : 0;
    double a = -0.5 * y0 + 1.5 * y1 - 1.5 * y2 + 0.5 * y3, b = y0 - 2.5 * y1 + 2 * y2 - 0.5 * y3, c = -0.5 * y0 + 0.5 * y2;
    return ((a * t + b) * t + c) * t + y1;
}
// sp: 0 release s, 1 velocity->gain exponent (0 = 1.0), 2 attack fade ms (0 = none), 3 max pitch distance to stretch (st, 0 = 12),
//     4 start offset ms (skip the sample's own attack, for legato), 5 pan spread by pitch, 6 detune cents (per note random, for ensembles),
//     7 layer bias (added to velocity before choosing a layer), 8 gain dB, 9 sustain pedal aware (cc 64), 10 loop sustained samples, 11 layer crossfade,
//     12 bow: skip the sample's slow swell by note length (1 = on): a short note starts 25 ms before the measured attack point,
//        a long one (4 s and up) keeps the first 40% of the swell; 13 added vibrato depth cents, 14 its rate Hz (onset after
//        0.18 s, full by 0.5 s, rate and phase per note) for samples recorded without vibrato or to spread an ensemble
static void syn_sampler(Track *tr, const Note *n, float *L, float *R, long nf) {
    SampleSet *ss = (SampleSet *)tr->user; double *p = tr->sp; if (!ss || !ss->n) return;
    Rng rg = {n->seed};
    double v = clampd(n->vel + p[7], 0, 0.999); int layer = (int)(v * ss->nl); if (layer >= ss->nl) layer = ss->nl - 1;
    // candidates in this layer (fall back to any layer) nearest in pitch
    int best = -1; double bd = 1e9; int cand[64], nc = 0;
    for (int pass = 0; pass < 2 && best < 0; pass++)
        for (int i = 0; i < ss->n; i++) { Smp *x = &ss->s[i]; if (pass == 0 && x->layer != layer) continue;
            double d = fabs(x->root - n->pitch) + (x->root < n->pitch ? 0.01 : 0); if (d < bd - 1e-9) { bd = d; best = i; nc = 0; } if (fabs(d - bd) < 1e-9 && nc < 64) cand[nc++] = i; }
    if (best < 0) return;
    Smp *x = &ss->s[nc > 1 ? cand[(int)(r01(&rg) * nc) % nc] : best];
    if (!x->L) { g_wav_quiet = 1; x->n = wav_read(x->path, &x->L, &x->R); x->sr = g_wav_sr; g_wav_quiet = 0; smp_trim(x); }
    double det = p[6] > 0 ? rgauss(&rg) * p[6] / 1200 : 0;
    // no two hits identical even without round-robin samples: +-2 cents and +-0.6 dB per note (from its seed)
    det += rgauss(&rg) * 2.0 / 1200; double gvar = db2a(rgauss(&rg) * 0.6);
    double ratio = pow(2, (n->pitch - x->root) / 12.0 + det) * x->sr / SR;
    double rel = p[0] > 0 ? p[0] : 0.25, start = p[4] > 0 ? p[4] / 1000 * x->sr : 0;
    if (p[12] > 0 && x->att > 0.03) { double f = clampd((n->dur - 0.5) / 3.5, 0, 1), sk = (x->att - 0.025) * (1 - f) + x->att * 0.4 * f; start = fmax(start, sk * x->sr); }
    double vdep = p[13] / 1200, vrate = (p[14] > 0 ? p[14] : 5.5) * (1 + 0.08 * rgauss(&rg)), vph = r01(&rg) * 2 * M_PI;
    double g = db2a(p[8]) * pow(v + 0.05, p[1] > 0 ? p[1] : 1.0) * ss->gain[x->layer] * gvar;
    double pan = p[5] > 0 ? clampd((n->pitch - 64) / 30.0 * p[5], -1, 1) : 0; double ca = (pan + 1) * M_PI / 4, gl = cos(ca) * 1.41421356, gr = sin(ca) * 1.41421356; if (gl > 1) gl = 1; if (gr > 1) gr = 1;
    double dur = n->dur; if (p[9] > 0) { int ti = (int)(tr - g_tr); dur = fmax(dur, damper_time(ti, n->t + n->dur) - n->t); }
    long s0 = lround(n->t * SR), len = (long)((dur + rel * 6) * SR);
    // loop region for long notes: the middle 50%-90% of the sample, crossfaded over 60 ms
    long la = (long)(x->n * 0.5), lb = (long)(x->n * 0.9), xf = (long)(0.06 * x->sr); if (lb - la < 2 * xf || p[10] <= 0) { la = 0; lb = 0; }
    double pos = start;
    for (long i = 0; i < len; i++) {
        double t = (double)i / SR, a = 1;
        if (p[2] > 0 && t < p[2] / 1000) a = t / (p[2] / 1000);
        if (t > dur) a *= exp(-(t - dur) / rel);
        double l, r;
        if (lb > 0 && pos >= lb - xf) { // crossfaded loop
            double over = pos - (lb - xf); if (over >= xf) { pos = la + (over - xf); over = -1; }
            if (over >= 0) { double w = over / xf, p2 = la + over; l = cubic(x->L, x->n, pos) * (1 - w) + cubic(x->L, x->n, p2) * w; r = cubic(x->R, x->n, pos) * (1 - w) + cubic(x->R, x->n, p2) * w; }
            else { l = cubic(x->L, x->n, pos); r = cubic(x->R, x->n, pos); }
        } else { if (pos >= x->n - 1) break; l = cubic(x->L, x->n, pos); r = cubic(x->R, x->n, pos); }
        long j = s0 + i; if (j >= 0 && j < nf) { L[j] += (float)(l * a * g * gl); R[j] += (float)(r * a * g * gr); }
        if (vdep > 0) { double ve = clampd((t - 0.18) / 0.32, 0, 1); pos += ratio * pow(2, vdep * ve * sin(2 * M_PI * vrate * t + vph)); } else
        pos += ratio;
        if (a < 1e-5 && t > dur) break;
    }
}
static int smp_track(const char *name, const char *manifest) { int i = syn_track(name, syn_sampler); g_tr[i].user = ss_load(manifest); return i; }
