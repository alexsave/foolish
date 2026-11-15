// test_loop: the sampler's "long loop" (sp[10] = 2) and the unchanged modes 0 and 1. The test writes its own samples (float
// wavs at 48 kHz in a temp folder), calls syn_sampler directly and is deterministic.
//   F: 0.8 s tone (a short zone, X = 35% of it).   A: 1.2 s harmonic tone at A3, constant amplitude.   B: the same tone with a 4 dB bump (sigma 40 ms) at 0.7 s and a 5.5 Hz
//   vibrato of 15 cents.   C: 1.2 s of noise (the out-of-phase case).   D: 0.25 s tone (loop zone under 150 ms).
// Asserts: (a) mode 2, 8 s note on A: no 10 ms RMS frame after the attack is more than 1.0 dB from the median; (b) the largest
// sample-to-sample jump (relative to the peak) is at most 1.5 times that of the raw sample; (c) on B the envelope autocorrelation
// over lags 0.1 to 2 s peaks under 0.6 in mode 2 and over 0.8 in mode 1 (the old fault); (d) the same note twice is
// sample-identical, a note with another seed differs; (e) a 0.6 s note does not loop while held: sample-identical to mode 0 up to its end; (j) its release rings on through the loop; (f) mode 0 and
// mode 1 output checksums equal the hard-coded ones, which were produced by compiling this test against the sampler.h from
// before the mode 2 edit (copy kept in the scratch area) and printing them; (g) the 0.25 s sample is not looped, plays out and
// stops, and is flagged; (h) on C the 50 ms level around the joins stays within 1.0 dB (the normalised gains); (i) on F, a sample whose crossfade
// could run past its end, the level stays within 1.0 dB of the median and no sample exceeds 1.5 times the unlooped peak.
// Build and run from engine/: clang -O2 -w -o <tmp>/tl test_loop.c && <tmp>/tl <tmp>/dir     Exit 0 = pass.
// Mutation checked (each applied to a copy of sampler.h, the test run against it, the original untouched):
//   fixed jump target instead of the draw          -> (c) mode 2 envelope autocorrelation 0.890 (not under 0.6)
//   no correlation refinement (target = the draw)  -> (a) a 10 ms level frame is 6.200 dB from the median
//   plain linear gains, no normalisation           -> (h) the noise level dips 2.802 dB
//   loop not entered once the note has ended     -> (j) the release is at -125.7 dB instead of -26.1 dB 0.75 s after the note's end
//   zone-length rule off                           -> (g) the 0.25 s sample still sounds after 0.30 s
//   jump target range not kept clear of the end    -> (i) a 10 ms level frame is 2.458 dB from the median on the 0.8 s sample
//   mode 1 crossfade 60 ms changed to 50 ms        -> (f) mode 1 output changed
#define main engine_main   // render.h carries the renderer's main; the test has its own
#include "render.h"
#undef main
void city_setup(void) {}
double city_loop(void) { return 0; }
void city_full(void) {}
#define SRC 48000
#define CK0 0x528b260831d5ec63ULL
#define CK1 0x95dca783fca9adcbULL
static int fails;
static void check(int c, const char *msg, double v) { if (!c) { printf("FAIL: %s (%.3f)\n", msg, v); fails++; } }
static void wfloat(const char *path, const float *x, long n) {   // stereo float32 wav, both channels the same
    FILE *f = fopen(path, "wb"); uint32_t sz = (uint32_t)(n * 8), v; uint16_t u;
    fwrite("RIFF", 1, 4, f); v = 36 + sz; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    u = 3; fwrite(&u, 2, 1, f); u = 2; fwrite(&u, 2, 1, f); v = SRC; fwrite(&v, 4, 1, f); v = SRC * 8; fwrite(&v, 4, 1, f); u = 8; fwrite(&u, 2, 1, f); u = 32; fwrite(&u, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&sz, 4, 1, f);
    for (long i = 0; i < n; i++) { fwrite(&x[i], 4, 1, f); fwrite(&x[i], 4, 1, f); } fclose(f);
}
static float *mk(int kind, double secs, long *n) {
    *n = (long)(secs * SRC); float *x = malloc(*n * 4); uint64_t s = 88172645463325252ULL; double ph = 0;
    for (long i = 0; i < *n; i++) {
        double t = (double)i / SRC, v;
        if (kind == 2) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; v = ((double)(s >> 11) / 9007199254740992.0 - 0.5) * 0.7; }
        else {
            double cents = kind == 1 ? 15 * sin(2 * M_PI * 5.5 * t) : 0; ph += 2 * M_PI * 220 * pow(2, cents / 1200) / SRC;
            v = 0; for (int k = 1; k <= 8; k++) v += sin(k * ph) / k; v *= 0.25;
            if (kind == 1) v *= pow(10, 4 * exp(-pow((t - 0.7) / 0.04, 2) / 2) / 20);
        }
        double a = fmin(1, fmin(t / 0.003, (secs - t) / 0.040)); x[i] = (float)(v * a);
    }
    return x;
}
static int mkset(const char *dir, const char *name, int kind, double secs, int mode) {
    char wp[500], mp[500]; long n; float *x = mk(kind, secs, &n);
    snprintf(wp, sizeof wp, "%s/%s.wav", dir, name); snprintf(mp, sizeof mp, "%s/%s.txt", dir, name); wfloat(wp, x, n); free(x);
    FILE *f = fopen(mp, "w"); fprintf(f, "57 0 0 %s\n", wp); fclose(f);
    int t = smp_track(name, mp); g_tr[t].sp[10] = mode; g_tr[t].sp[0] = 0.25; return t;
}
static uint64_t fnv(const float *a, long n, uint64_t h) { const unsigned char *p = (const unsigned char *)a; for (long i = 0; i < n * 4; i++) { h ^= p[i]; h *= 1099511628211ULL; } return h; }
static long NF = 12 * SRC;
static void play(int tr, double dur, uint64_t seed, float **L, float **R) {
    *L = calloc(NF, 4); *R = calloc(NF, 4);
    Note nt = {.tr = tr, .beat = 0, .len = dur, .pitch = 57, .vel = 0.8, .t = 0.5, .dur = dur, .seed = seed};
    syn_sampler(&g_tr[tr], &nt, *L, *R, NF);
}
static double rmsdb(const float *x, long a, long n) { double e = 0; for (long i = 0; i < n; i++) e += (double)x[a + i] * x[a + i]; return 10 * log10(fmax(e / n, 1e-14)); }
static int cmpd(const void *a, const void *b) { double d = *(double *)a - *(double *)b; return d < 0 ? -1 : d > 0; }
static double env_ac(const float *x, double t0, double t1, double *lag) {   // peak of the normalised autocorrelation of the 10 ms level, lags 0.1 to 2 s
    long a = (long)(t0 * 100), b = (long)(t1 * 100), m = b - a; double *e = malloc(m * 8), mean = 0;
    for (long i = 0; i < m; i++) { e[i] = rmsdb(x, (a + i) * SRC / 100, SRC / 100); mean += e[i]; }
    mean /= m; for (long i = 0; i < m; i++) e[i] -= mean;
    double r0 = 0; for (long i = 0; i < m; i++) r0 += e[i] * e[i];
    double best = -1; for (long k = 10; k <= 200 && k < m; k++) { double r = 0; for (long i = 0; i + k < m; i++) r += e[i] * e[i + k]; r /= r0; if (r > best) { best = r; *lag = k * 0.01; } }
    free(e); return best;
}
int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "."; char cmd[600]; snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir); system(cmd);
    int A2 = mkset(dir, "a2", 0, 1.2, 2), B2 = mkset(dir, "b2", 1, 1.2, 2), B1 = mkset(dir, "b1", 1, 1.2, 1), B0 = mkset(dir, "b0", 1, 1.2, 0);
    int C2 = mkset(dir, "c2", 2, 1.2, 2), D2 = mkset(dir, "d2", 0, 0.25, 2), A0 = mkset(dir, "a0", 0, 1.2, 0), F2 = mkset(dir, "f2", 0, 0.8, 2), F0 = mkset(dir, "f0", 0, 0.8, 0);
    float *L, *R, *L2, *R2;
    // (f) modes 0 and 1 unchanged: 4 s notes on B
    play(B0, 4, 777, &L, &R); uint64_t c0 = fnv(R, NF, fnv(L, NF, 1469598103934665603ULL)); free(L); free(R);
    play(B1, 4, 777, &L, &R); uint64_t c1 = fnv(R, NF, fnv(L, NF, 1469598103934665603ULL)); free(L); free(R);
    printf("checksums mode0 0x%llxULL mode1 0x%llxULL\n", (unsigned long long)c0, (unsigned long long)c1);
    check(c0 == CK0, "mode 0 output changed", 0); check(c1 == CK1, "mode 1 output changed", 0);
    // (a) (b) on A
    play(A2, 8, 1234, &L, &R); long a0 = (long)(0.8 * SRC), nn = (long)(7.4 * SRC);
    int nfr = (int)(nn / (SRC / 100)); double *d = malloc(nfr * 8), *ds = malloc(nfr * 8);
    for (int i = 0; i < nfr; i++) ds[i] = d[i] = rmsdb(L, a0 + (long)i * SRC / 100, SRC / 100);
    qsort(ds, nfr, 8, cmpd); double med = ds[nfr / 2], worst = 0; for (int i = 0; i < nfr; i++) worst = fmax(worst, fabs(d[i] - med));
    check(worst <= 1.0, "(a) a 10 ms level frame deviates from the median by more than 1.0 dB on A", worst);
    double pk = 0, jr = 0; for (long i = a0; i < a0 + nn; i++) pk = fmax(pk, fabs(L[i])); for (long i = a0 + 1; i < a0 + nn; i++) jr = fmax(jr, fabs(L[i] - L[i - 1]));
    long rn; float *raw = mk(0, 1.2, &rn); double rp = 0, rj = 0; for (long i = 0; i < rn; i++) rp = fmax(rp, fabs(raw[i])); for (long i = 1; i < rn; i++) rj = fmax(rj, fabs(raw[i] - raw[i - 1])); free(raw);
    check(jr / pk <= 1.5 * rj / rp, "(b) largest relative jump exceeds 1.5 times the raw sample's", jr / pk / (rj / rp));
    // (d) same note twice identical, another seed differs
    play(A2, 8, 1234, &L2, &R2); check(!memcmp(L, L2, NF * 4) && !memcmp(R, R2, NF * 4), "(d) the same note rendered twice differs", 0); free(L2); free(R2);
    play(A2, 8, 4321, &L2, &R2); check(memcmp(L, L2, NF * 4) != 0, "(d) a note with another seed is identical", 0); free(L2); free(R2); free(L); free(R); free(d); free(ds);
    // (c) envelope autocorrelation on B
    double lag2, lag1; play(B2, 8, 1234, &L, &R); double ac2 = env_ac(L, 0.8, 8.4, &lag2); free(L); free(R);
    play(B1, 8, 1234, &L, &R); double ac1 = env_ac(L, 0.8, 8.4, &lag1); free(L); free(R);
    printf("envelope autocorrelation on B: mode 2 %.2f at %.2f s, mode 1 %.2f at %.2f s\n", ac2, lag2, ac1, lag1);
    check(ac2 < 0.6, "(c) mode 2 envelope autocorrelation peak is 0.6 or more", ac2); check(ac1 > 0.8, "(c) mode 1 envelope autocorrelation peak is not above 0.8 (the old fault is not shown)", ac1);
    // (e) a 0.6 s note does not loop while it is held: identical to mode 0 up to the note's end (the note starts at 0.5 s)
    play(A2, 0.6, 99, &L, &R); play(A0, 0.6, 99, &L2, &R2); long he = (long)(1.1 * SRC);
    check(!memcmp(L, L2, he * 4) && !memcmp(R, R2, he * 4), "(e) a note that ends before the zone differs from the unlooped render while it is held", 0);
    // (j) its release rings on through the loop: 0.75 s after the note's end (past the 1.2 s sample's own end) the level is the
    // release curve's (0.75 / 0.25 time constants = -26.1 dB from the held level), where the unlooped sample has stopped dead
    { double held = rmsdb(L, (long)(0.9 * SRC), SRC / 20), tail = rmsdb(L, (long)(1.85 * SRC) - SRC / 40, SRC / 20), tail0 = rmsdb(L2, (long)(1.85 * SRC) - SRC / 40, SRC / 20);
      check(fabs((tail - held) + 26.06) <= 2.0, "(j) the release of a short note is not on its curve 0.75 s after the note's end (dB from the held level)", tail - held);
      check(tail0 < held - 60, "(j) the unlooped sample still sounds past its own end (the test would prove nothing)", tail0 - held); }
    free(L); free(R); free(L2); free(R2);
    // (g) the short sample plays out and stops
    play(D2, 5, 5, &L, &R); long last = 0; for (long i = 0; i < NF; i++) if (fabs(L[i]) > 1e-7) last = i; double end_s = (double)last / SRC - 0.5;
    check(end_s < 0.30, "(g) the 0.25 s sample is still sounding after 0.30 s", end_s); free(L); free(R);
#ifndef OLD_SAMPLER   // the pre-edit sampler.h has no warned flag; it is only compiled that way to print the checksums
    SampleSet *ss = (SampleSet *)g_tr[D2].user; check(ss->s[0].warned == 1, "(g) the short sample was not flagged", 0);
#endif
    // (h) noise: the joins keep the level (constant-power normalised gains)
    play(C2, 8, 1234, &L, &R); double wc = 0; double *e5 = malloc(150 * 8), *s5 = malloc(150 * 8); int k5 = 0;
    for (long a = (long)(0.8 * SRC); a + SRC / 20 < (long)(8.2 * SRC) && k5 < 150; a += SRC / 20) e5[k5++] = rmsdb(L, a, SRC / 20);
    memcpy(s5, e5, k5 * 8); qsort(s5, k5, 8, cmpd); for (int i = 0; i < k5; i++) wc = fmax(wc, fabs(e5[i] - s5[k5 / 2]));
    check(wc <= 1.0, "(h) a 50 ms level on noise deviates from the median by more than 1.0 dB", wc); free(L); free(R);
    // (i) the 0.8 s sample
    play(F2, 8, 31, &L, &R); play(F0, 0.5, 31, &L2, &R2); double fp2 = 0, fp0 = 0; for (long i = 0; i < NF; i++) { if (!(fabs(L[i]) <= 10)) fp2 = 1e9; fp2 = fmax(fp2, fabs(L[i])); fp0 = fmax(fp0, fabs(L2[i])); }
    check(fp2 <= 1.5 * fp0, "(i) a sample exceeds 1.5 times the unlooped peak (or is not finite)", fp2 / fp0);
    { int nf2 = (int)(7.4 * 100); double *q = malloc(nf2 * 8), *q2 = malloc(nf2 * 8); for (int i = 0; i < nf2; i++) q2[i] = q[i] = rmsdb(L, (long)(0.8 * SRC) + (long)i * SRC / 100, SRC / 100);
      qsort(q2, nf2, 8, cmpd); double w2 = 0; for (int i = 0; i < nf2; i++) w2 = fmax(w2, fabs(q[i] - q2[nf2 / 2]));
      check(w2 <= 1.0, "(i) a 10 ms level frame deviates from the median by more than 1.0 dB on the 0.8 s sample", w2); free(q); free(q2); }
    free(L); free(R); free(L2); free(R2);
    puts(fails ? "FAIL test_loop" : "PASS test_loop"); return fails ? 1 : 0;
}
