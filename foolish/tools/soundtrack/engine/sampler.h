// sampler.h: plays recorded instrument samples (free libraries under samples/, read as data only).
// A sample set is described by a manifest of lines "root_midi layer rr path" (layer 0 = softest).
// Notes pick the nearest root (resampled with cubic interpolation), the velocity layer, and a round-robin
// from the note's seed; leading silence is trimmed at load; notes longer than the sample loop a crossfaded
// stretch of its sustain; note-off fades with the track's release.
#pragma once
#include "keys.h"
#include "mix.h"

typedef struct { int root, layer, rr; double sr; float *L, *R; long n; double att; int warned; char path[600]; } Smp;
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
        // the manifests carry the absolute path of the machine that built them; SAMPLES=<dir> re-roots everything after
        // ".../soundtrack/samples/" onto <dir>, so a rebuilt library can live anywhere
        { const char *root = getenv("SAMPLES"), *cut = strstr(path, "/soundtrack/samples/");
          if (root && cut) snprintf(x.path, sizeof x.path, "%s/%s", root, cut + 20); else snprintf(x.path, sizeof x.path, "%s", path); }
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
//        17 vibrato delay in seconds (> 0; 0 = the built-in 0.18 s), the ramp to full depth stays 0.32 s after it. The note's own switch:
//        n->p[2] depth scale (0 = the track's sp[13], negative = off for this note, positive s = sp[13] times s) and n->p[3] rate
//        override in Hz (0 = sp[14]); the score text sets them with "VIB PART: on|off|depth s|rate hz". A track with sp[13] = 0 has no
//        vibrato whatever the note asks for.
//        10 = 1: loop the middle 50-90% of the sample with a fixed 60 ms linear crossfade (the same fragment every pass).
//        10 = 2 ("long loop"): play the sample forward once; when a note outlives the zone [A, B] (A = the later of the attack point
//        plus 50 ms and 20% of the length, B = 60 ms before the end) each pass jumps, at B - X, to a point drawn from the note's own
//        seed in the first half of the zone (kept at least 2X + one period from B so the new stream has room for the next
//        crossfade), refined within one root period to the best normalised cross-correlation, and crossfades
//        over X = 35% of the zone (40 to 250 ms) with constant-power gains normalised for that correlation; a zone under 150 ms
//        is not looped (the sample plays out, one stderr line per sample); a note that ended before the zone never starts a loop, one
//        already looping keeps looping through its release. -DSAMPLER_TRACE prints note and join lines to stderr.
//     16 linear release ms (> 0): after note-off the gain falls linearly to zero over exactly that many ms and rendering ends there
//        (instead of the exponential of sp[0]); with sp[2] equal to it, two adjacent notes crossfade with gains that sum to 1
//        (not used by loop mode 2); 0 = off (default)
//     15 exact (> 0): no per-note random detune (+-2 cents) and no +-0.6 dB gain variation, so a note at velocity v plays its sample
//        at gain (v + 0.05)^sp[1] times sp[8] and at the sample's own pitch (sp[6] detune still applies if set); 0 = off (default)
// mode 2 ("long loop") of sp[10]; see the sp comment. Positions are in source frames; the loop zone is [A, B].
static double ll_corr(const Smp *x, long a, long c, long W) {   // normalised cross-correlation of the mono signal at a and c over W frames
    double sab = 0, saa = 0, sbb = 0;
    for (long k = 0; k < W; k++) { double u = x->L[a + k] + x->R[a + k], v = x->L[c + k] + x->R[c + k]; sab += u * v; saa += u * u; sbb += v * v; }
    return saa > 1e-12 && sbb > 1e-12 ? sab / sqrt(saa * sbb) : 0;
}
static void sampler_longloop(Smp *x, Rng *rg, const Note *n, const double *p, double ratio, double start, double vdep, double vrate, double vph, double vdelay,
                             double g, double gl, double gr, double dur, double rel, float *L, float *R, long nf, long s0, long len) {
    long N = x->n; double sr = x->sr, att = x->att;
    double A = fmax(att * sr + 0.050 * sr, 0.2 * N), B = N - 0.060 * sr, Z = B - A;
    int loop = Z >= 0.150 * sr;
    if (!loop && !x->warned) { x->warned = 1; fprintf(stderr, "sampler: %s: loop zone %.0f ms is under 150 ms, mode 2 plays it out without looping\n", x->path, Z / sr * 1000); }
    double X = clampd(0.35 * Z, 0.040 * sr, 0.250 * sr);
    double period = sr / (440.0 * pow(2, (x->root - 69) / 12.0));
    long W = (long)fmax(2 * period, 0.015 * sr);
#ifdef SAMPLER_TRACE
    fprintf(stderr, "TRACE note t=%.4f dur=%.4f layer=%d file=%s n=%ld sr=%.0f root=%d ratio=%.6f att=%.3f\n", n->t, n->dur, x->layer, x->path, N, sr, x->root, ratio, att);
#endif
    double pos = start, pos2 = 0, w = 0, rc = 0; int xf = 0, looped = 0;
    for (long i = 0; i < len; i++) {
        double t = (double)i / SR, a = 1;
        if (p[2] > 0 && t < p[2] / 1000) a = t / (p[2] / 1000);
        if (t > dur) a *= exp(-(t - dur) / rel);
        if (loop && !xf && pos >= B - X) {   // also inside the release: a tail that reaches the sample's end would stop dead there
            double hi = fmin(A + 0.5 * Z, B - 2 * X - period); if (hi < A) hi = A;   // the new stream keeps room for the next crossfade
            double s = A + r01(rg) * (hi - A);
            long cp = (long)pos, best = (long)s; double br = -2;
            for (long c = (long)(s - period); c <= (long)(s + period); c++) {
                if (c < 1 || c + W >= N || cp + W >= N) continue;
                double r = ll_corr(x, cp, c, W); if (r > br) { br = r; best = c; }
            }
            pos2 = best + (pos - cp); rc = clampd(br, 0, 1); w = 0; xf = 1; looped = 1;
#ifdef SAMPLER_TRACE
            fprintf(stderr, "TRACE jump t=%.4f from=%.0f to=%.0f corr=%.3f\n", n->t + t, pos, pos2, br);
#endif
        }
        double l, r;
        if (xf) {
            double ca = cos(w * M_PI / 2), cb = sin(w * M_PI / 2), nrm = sqrt(ca * ca + cb * cb + 2 * ca * cb * rc);
            l = (cubic(x->L, N, pos) * ca + cubic(x->L, N, pos2) * cb) / nrm; r = (cubic(x->R, N, pos) * ca + cubic(x->R, N, pos2) * cb) / nrm;
        } else { if (pos >= N - 1) break; l = cubic(x->L, N, pos); r = cubic(x->R, N, pos); }
        long j = s0 + i; if (j >= 0 && j < nf) { L[j] += (float)(l * a * g * gl); R[j] += (float)(r * a * g * gr); }
        double inc = vdep > 0 ? ratio * pow(2, vdep * clampd((t - vdelay) / 0.32, 0, 1) * sin(2 * M_PI * vrate * t + vph)) : ratio;
        pos += inc;
        if (xf) { pos2 += inc; w += inc / X;
            if (w >= 1) { pos = pos2; xf = 0;
#ifdef SAMPLER_TRACE
                fprintf(stderr, "TRACE join t=%.4f xf_s=%.4f A=%.0f B=%.0f\n", n->t + t, X / ratio / SR, A, B);
#endif
            } }
        if (a < 1e-5 && t > dur) break;
    }
}
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
    // sp[15] exact > 0 turns both off (a replica reproduces a stored sample at the level its velocity says, at its own pitch)
    double gvar = 1; if (p[15] <= 0) { det += rgauss(&rg) * 2.0 / 1200; gvar = db2a(rgauss(&rg) * 0.6); }
    double ratio = pow(2, (n->pitch - x->root) / 12.0 + det) * x->sr / SR;
    double rel = p[0] > 0 ? p[0] : 0.25, start = p[4] > 0 ? p[4] / 1000 * x->sr : 0;
    if (p[12] > 0 && x->att > 0.03) { double f = clampd((n->dur - 0.5) / 3.5, 0, 1), sk = (x->att - 0.025) * (1 - f) + x->att * 0.4 * f; start = fmax(start, sk * x->sr); }
    // the note's own vibrato switch (n->p[2] depth scale: 0 = the track's, negative = off, positive = times that; n->p[3] rate override, 0 = the
    // track's) and the track's delay sp[17] (0 = 0.18 s); the random draws are the same whatever the switch says, so a note keeps its phase
    double vsc = n->p[2] < 0 ? 0 : (n->p[2] > 0 ? n->p[2] : 1), vdelay = p[17] > 0 ? p[17] : 0.18;
    double vdep = p[13] / 1200 * vsc, vrate = (n->p[3] > 0 ? n->p[3] : (p[14] > 0 ? p[14] : 5.5)) * (1 + 0.08 * rgauss(&rg)), vph = r01(&rg) * 2 * M_PI;
    double g = db2a(p[8]) * pow(v + 0.05, p[1] > 0 ? p[1] : 1.0) * ss->gain[x->layer] * gvar;
    double pan = p[5] > 0 ? clampd((n->pitch - 64) / 30.0 * p[5], -1, 1) : 0; double ca = (pan + 1) * M_PI / 4, gl = cos(ca) * 1.41421356, gr = sin(ca) * 1.41421356; if (gl > 1) gl = 1; if (gr > 1) gr = 1;
    double dur = n->dur; if (p[9] > 0) { int ti = (int)(tr - g_tr); dur = fmax(dur, damper_time(ti, n->t + n->dur) - n->t); }
    long s0 = lround(n->t * SR), len = (long)((dur + rel * 6) * SR);
    if (p[10] >= 1.5) { sampler_longloop(x, &rg, n, p, ratio, start, vdep, vrate, vph, vdelay, g, gl, gr, dur, rel, L, R, nf, s0, len); return; }
    double rlin = p[16] > 0 ? p[16] / 1000 : 0;    // sp[16]: linear release, the gain falls to zero over exactly this long after note-off
    if (rlin > 0) len = (long)((dur + rlin) * SR);
    // loop region for long notes: the middle 50%-90% of the sample, crossfaded over 60 ms
    long la = (long)(x->n * 0.5), lb = (long)(x->n * 0.9), xf = (long)(0.06 * x->sr); if (lb - la < 2 * xf || p[10] <= 0) { la = 0; lb = 0; }
    double pos = start;
    for (long i = 0; i < len; i++) {
        double t = (double)i / SR, a = 1;
        if (p[2] > 0 && t < p[2] / 1000) a = t / (p[2] / 1000);
        if (t > dur) a *= rlin > 0 ? fmax(0.0, 1 - (t - dur) / rlin) : exp(-(t - dur) / rel);
        double l, r;
        if (lb > 0 && pos >= lb - xf) { // crossfaded loop
            double over = pos - (lb - xf); if (over >= xf) { pos = la + (over - xf); over = -1; }
            if (over >= 0) { double w = over / xf, p2 = la + over; l = cubic(x->L, x->n, pos) * (1 - w) + cubic(x->L, x->n, p2) * w; r = cubic(x->R, x->n, pos) * (1 - w) + cubic(x->R, x->n, p2) * w; }
            else { l = cubic(x->L, x->n, pos); r = cubic(x->R, x->n, pos); }
        } else { if (pos >= x->n - 1) break; l = cubic(x->L, x->n, pos); r = cubic(x->R, x->n, pos); }
        long j = s0 + i; if (j >= 0 && j < nf) { L[j] += (float)(l * a * g * gl); R[j] += (float)(r * a * g * gr); }
        if (vdep > 0) { double ve = clampd((t - vdelay) / 0.32, 0, 1); pos += ratio * pow(2, vdep * ve * sin(2 * M_PI * vrate * t + vph)); } else
        pos += ratio;
        if (a < 1e-5 && t > dur) break;
    }
}
static int smp_track(const char *name, const char *manifest) { int i = syn_track(name, syn_sampler); g_tr[i].user = ss_load(manifest); return i; }
