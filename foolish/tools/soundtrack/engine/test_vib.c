// test_vib: the sampler vibrato switch in the score text and in the sampler.
// Parsing half: "VIB PART: off|on|depth <scale>|rate <hz>" sets p[2] (depth scale code: 0 track default, negative off, positive scale)
// and p[3] (rate override, 0 = the track's) of every note of that part inside that section, wherever the line stands in the section
// (before or after the part's own line), for bar notes and EVENTS notes alike; later words override earlier ones; a line whose
// words are not all VIB words is a part (a part named "VIB X" still reads as a part); untouched notes keep 0.
// Audio half: real sampler tracks (smp_track, loop mode 2) on a flat synthetic sample (a steady harmonic tone at A3, no vibrato of its own,
// written by the test), a note held 4 s, the f0 read from the phase of the first harmonic (a 5 period moving average, 5 ms steps).
// The engine gives every note a rate of the track's rate times (1 + 0.08 g) with g a standard normal draw (no clamp), so the rate of
// one note is only known to within 3 sigma = 24 percent; the rate assertions therefore use the median over 8 notes (8 seeds), whose
// bound is 10 percent. Asserts: on (the track at 5.35 Hz and 10.1 cents): median peak within 10 percent of 5.35 Hz, each note's depth
// within 15 percent of 10.1 cents; off (p[2] < 0): pitch sd under 1.5 cents; depth scale 0.5: half the depth within 15 percent; rate
// override 6.5: median peak within 10 percent of 6.5; sp[17] = 1.0: the first second flat (sd under 1.5 cents) where the default has
// vibrato by 0.6 s (sd over 3 cents); a track with sp[13] = 0 stays flat (sd under 1.5 cents) when a note asks for "on".
// Build and run from engine/: clang -O2 -w -o <tmp>/tv test_vib.c && <tmp>/tv <tmp>/score.txt <tmp>/dir   Exit 0 = pass.
// Mutation checked (each applied to a copy of scoretxt.h, the test run against it, the original untouched):
//   VIB applied to every section        -> A section one: off: the note at beat 0 has p2 2.00 p3 5.20 (a later section's values)
//   VIB applied to every part           -> B section one is untouched: the note at beat 0 has p2 -1.00
//   "off" parsed as scale 0             -> A section one: off: the note at beat 0 has p2 0.00
//   rate not stored                     -> A section two: the note at beat 8 has p3 0.00, expected 5.20
//   unknown words accepted              -> the part "VIB X" is read as a VIB line: 11 notes instead of 12
// Audio half, mutations of sampler.h (copy, original untouched):
//   off ignored                         -> off: pitch sd 7.07 cents
//   depth scale ignored                 -> depth scale 0.5: depth 9.79 cents, expected 5.05
//   rate override ignored               -> rate override 6.5: median peak 5.38 Hz
//   delay ignored                       -> sp[17] = 1.0: pitch sd 5.70 cents in the first second
//   "on" forces vibrato on a depth-0 track -> pitch sd 6.94 cents on the sp[13] = 0 track
//   sampler-only rule off               -> C is not a sampler part: the note at beat 16 has p2 3.00 p3 7.00
#define main engine_main   // render.h carries the renderer's main; the test has its own
#include "render.h"
#undef main
#include "scoretxt.h"
void city_setup(void) {}
double city_loop(void) { return 0; }
void city_full(void) {}
static int g_a, g_b, g_x, g_c;
static int map(const char *p, double *tp, double *vs, double *dp) {
    *tp = 0; *vs = 1;
    if (!strcmp(p, "A")) return g_a;
    if (!strcmp(p, "B")) return g_b;
    if (!strcmp(p, "VIB X")) return g_x;
    if (!strcmp(p, "C")) return g_c;
    return -1;
}
static int fails;
static void expect(int tr, double beat, double p2, double p3, const char *what) {
    for (int i = 0; i < g_nnote; i++) { Note *n = &g_note[i];
        if (n->tr == tr && fabs(n->beat - beat) < 1e-6) {
            if (fabs(n->p[2] - p2) > 1e-9 || fabs(n->p[3] - p3) > 1e-9) { printf("FAIL: %s: note at beat %.2f has p2 %.2f p3 %.2f, expected %.2f %.2f\n", what, beat, n->p[2], n->p[3], p2, p3); fails++; }
            return; } }
    printf("FAIL: %s: no note at beat %.2f of track %d\n", what, beat, tr); fails++;
}
#define SRC 48000
static int mkflat(const char *dir, const char *name, double depth_c, double rate, double delay) {
    char wp[500], mp[500]; long n = (long)(1.5 * SRC); float *x = malloc(n * 4); double ph = 0;
    for (long i = 0; i < n; i++) { double t = (double)i / SRC, v = 0; ph += 2 * M_PI * 220.0 / SRC; for (int k = 1; k <= 8; k++) v += sin(k * ph) / k;
        x[i] = (float)(0.25 * v * fmin(1, fmin(t / 0.003, (1.5 - t) / 0.040))); }
    snprintf(wp, sizeof wp, "%s/%s.wav", dir, name); snprintf(mp, sizeof mp, "%s/%s.txt", dir, name);
    FILE *f = fopen(wp, "wb"); uint32_t sz = (uint32_t)(n * 8), v; uint16_t u;
    fwrite("RIFF", 1, 4, f); v = 36 + sz; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    u = 3; fwrite(&u, 2, 1, f); u = 2; fwrite(&u, 2, 1, f); v = SRC; fwrite(&v, 4, 1, f); v = SRC * 8; fwrite(&v, 4, 1, f); u = 8; fwrite(&u, 2, 1, f); u = 32; fwrite(&u, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&sz, 4, 1, f); for (long i = 0; i < n; i++) { fwrite(&x[i], 4, 1, f); fwrite(&x[i], 4, 1, f); } fclose(f); free(x);
    f = fopen(mp, "w"); fprintf(f, "57 0 0 %s\n", wp); fclose(f);
    int t = smp_track(name, mp); g_tr[t].sp[10] = 2; g_tr[t].sp[0] = 0.25; g_tr[t].sp[13] = depth_c; g_tr[t].sp[14] = rate; g_tr[t].sp[17] = delay; return t;
}
// cents curve of a 4 s note started at 0.5 s: phase of the first harmonic against 220 Hz, 5 ms steps from t0 to t1 (s after the note start)
static int curve(int tr, double p2, double p3, uint64_t seed, double t0, double t1, double *c) {
    long NF = 6 * SRC; float *L = calloc(NF, 4), *R = calloc(NF, 4);
    Note nt = {.tr = tr, .beat = 0, .len = 4, .pitch = 57, .vel = 0.8, .t = 0.5, .dur = 4, .seed = seed}; nt.p[2] = p2; nt.p[3] = p3;
    syn_sampler(&g_tr[tr], &nt, L, R, NF);
    long W = (long)(5 * SRC / 220.0), n = 0; double prev = 0, acc = 0;
    for (double t = t0; t < t1; t += 0.005) {
        long a = (long)((0.5 + t) * SRC) - W / 2; double re = 0, im = 0;
        for (long k = 0; k < W; k++) { double ph = 2 * M_PI * 220.0 * (a + k) / SRC; re += L[a + k] * cos(ph); im -= L[a + k] * sin(ph); }
        double ang = atan2(im, re); if (n) { double d = ang - prev; while (d > M_PI) d -= 2 * M_PI; while (d < -M_PI) d += 2 * M_PI; acc += d; }
        prev = ang; c[n++] = acc; }
    // phase advance per step to cents: the frequency offset is the slope of the unwrapped phase; use the 15 ms central difference
    double *f = malloc(n * 8); for (int i = 0; i < n; i++) { int a = i > 1 ? i - 1 : 0, b = i < n - 2 ? i + 1 : n - 1; double dfreq = (c[b] - c[a]) / (2 * M_PI * (b - a) * 0.005); f[i] = 1200 * log2(1 + dfreq / 220.0); }
    memcpy(c, f, n * 8); free(f); free(L); free(R); return (int)n;
}
static double sdev(const double *c, int n) { double m = 0, v = 0; for (int i = 0; i < n; i++) m += c[i]; m /= n; for (int i = 0; i < n; i++) v += (c[i] - m) * (c[i] - m); return sqrt(v / n); }
static void peak(const double *c, int n, double *fr, double *amp) {   // the strongest sinusoid between 3 and 10 Hz (Hann window, a 0.02 Hz scan)
    double m = 0; for (int i = 0; i < n; i++) m += c[i]; m /= n; *amp = 0; *fr = 0;
    for (double f = 3; f <= 10; f += 0.02) { double re = 0, im = 0, w = 0;
        for (int i = 0; i < n; i++) { double h = 0.5 - 0.5 * cos(2 * M_PI * i / (n - 1)); re += (c[i] - m) * h * cos(2 * M_PI * f * i * 0.005); im += (c[i] - m) * h * sin(2 * M_PI * f * i * 0.005); w += h; }
        double a = 2 * sqrt(re * re + im * im) / w; if (a > *amp) { *amp = a; *fr = f; } }
}
static int cmpf(const void *a, const void *b) { double d = *(double *)a - *(double *)b; return d < 0 ? -1 : d > 0; }
static void audio_half(const char *dir) {
    char cmd[600]; snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir); system(cmd);
    int T = mkflat(dir, "vibA", 10.1, 5.35, 0), Z = mkflat(dir, "vibZ", 0, 5.35, 0), D = mkflat(dir, "vibD", 10.1, 5.35, 1.0);
    double c[1000], fr, am, fs[8], ds[8], fs2[8], fh[8], dh[8], dsd = 0;
    for (int s = 0; s < 8; s++) {
        int n = curve(T, 0, 0, 1000 + s * 77, 1.0, 3.5, c); peak(c, n, &fs[s], &ds[s]);
        n = curve(T, 0.5, 0, 1000 + s * 77, 1.0, 3.5, c); peak(c, n, &fh[s], &dh[s]);
        n = curve(T, 0, 6.5, 1000 + s * 77, 1.0, 3.5, c); peak(c, n, &fs2[s], &am);
        if (fabs(ds[s] - 10.1) > 1.5) { printf("FAIL: on: note %d has depth %.2f cents, expected 10.1 within 15 percent\n", s, ds[s]); fails++; }
        if (fabs(dh[s] - 5.05) > 0.76) { printf("FAIL: depth scale 0.5: note %d has depth %.2f cents, expected 5.05 within 15 percent\n", s, dh[s]); fails++; }
    }
    qsort(fs, 8, 8, cmpf); qsort(fs2, 8, 8, cmpf);
    double m1 = (fs[3] + fs[4]) / 2, m2 = (fs2[3] + fs2[4]) / 2;
    if (fabs(m1 / 5.35 - 1) > 0.10) { printf("FAIL: on: the median vibrato peak is %.2f Hz, expected 5.35 within 10 percent\n", m1); fails++; }
    if (fabs(m2 / 6.5 - 1) > 0.10) { printf("FAIL: rate override 6.5: the median vibrato peak is %.2f Hz, expected 6.5 within 10 percent\n", m2); fails++; }
    double worst_off = 0; for (int s = 0; s < 8; s++) { int n = curve(T, -1, 0, 1000 + s * 77, 1.0, 3.5, c); worst_off = fmax(worst_off, sdev(c, n)); }
    if (worst_off >= 1.5) { printf("FAIL: off: pitch sd %.2f cents, expected under 1.5\n", worst_off); fails++; }
    int n = curve(T, 0, 0, 1000, 0.55, 0.95, c); double sd_def = sdev(c, n);
    if (sd_def < 3.0) { printf("FAIL: default delay: pitch sd %.2f cents over 0.55 to 0.95 s, expected vibrato (over 3)\n", sd_def); fails++; }
    n = curve(D, 0, 0, 1000, 0.1, 0.95, c); double sd_del = sdev(c, n);
    if (sd_del >= 1.5) { printf("FAIL: sp[17] = 1.0: pitch sd %.2f cents in the first second, expected under 1.5\n", sd_del); fails++; }
    n = curve(Z, 1, 0, 1000, 1.0, 3.5, c); double sd_z = sdev(c, n);
    if (sd_z >= 1.5) { printf("FAIL: a track with sp[13] = 0 and a note asking for on: pitch sd %.2f cents, expected under 1.5\n", sd_z); fails++; }
    printf("audio: on median %.2f Hz, depth %.1f..%.1f c; scale 0.5 depth %.1f..%.1f c; rate 6.5 median %.2f Hz; off sd %.2f c; delay: default sd %.1f c, sp17 sd %.2f c; depth-0 track sd %.2f c\n",
           m1, ds[0], ds[7], dh[0], dh[7], m2, worst_off, sd_def, sd_del, sd_z);
}
int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "test_vib_score.txt";
    FILE *f = fopen(path, "w"); if (!f) { perror(path); return 2; }
    fputs("TEMPO 120\n"
          "SECTION ONE BARS 2 KEY C\n"
          "VIB A: off\n"                       // before the part's own line
          "A: C4:4 | D4:4\n"
          "B: E4:4 | F4:4\n"
          "EVENTS A: 1.5,G4,1,0.8\n"           // an EVENTS note is covered like a bar note
          "SECTION TWO BARS 2 KEY C\n"
          "A: C5:4 | D5:4\n"
          "B: E5:4 | F5:4\n"
          "VIB A: depth 0.5 rate 5.2\n"        // after the part's own lines
          "VIB B: on rate 6\n"
          "SECTION THREE BARS 1 KEY C\n"
          "A: C4:4\n"
          "VIB A: off depth 2\n"               // the later word wins
          "C: C4:4\n"
          "VIB C: depth 3 rate 7\n"            // C is not a sampler part: the line must not touch its notes
          "VIB X: C4:4\n", f);                  // a part named "VIB X", not a VIB line
    fclose(f);
    g_a = syn_track("a", syn_sampler); g_b = syn_track("b", syn_sampler); g_x = syn_track("x", syn_sampler); g_c = syn_track("c", 0);
    score_txt(path, map, 0); remove(path);
    if (g_nnote != 12) { printf("FAIL: expected 12 notes, got %d\n", g_nnote); puts("FAIL test_vib"); return 1; }
    expect(g_a, 0, -1, 0, "A section one: off"); expect(g_a, 4, -1, 0, "A section one: off"); expect(g_a, 1.5, -1, 0, "A section one: the EVENTS note is covered");
    expect(g_b, 0, 0, 0, "B section one is untouched"); expect(g_b, 4, 0, 0, "B section one is untouched");
    expect(g_a, 8, 0.5, 5.2, "A section two: depth 0.5 rate 5.2"); expect(g_a, 12, 0.5, 5.2, "A section two: depth 0.5 rate 5.2");
    expect(g_b, 8, 1, 6, "B section two: on rate 6"); expect(g_b, 12, 1, 6, "B section two: on rate 6");
    expect(g_a, 16, 2, 0, "A section three: depth 2 overrides off");
    expect(g_c, 16, 0, 0, "C is not a sampler part: its VIB line is ignored");
    expect(g_x, 16, 0, 0, "the part VIB X is a part, not a VIB line");
    audio_half(argc > 2 ? argv[2] : ".");
    puts(fails ? "FAIL test_vib" : "PASS test_vib"); return fails ? 1 : 0;
}
