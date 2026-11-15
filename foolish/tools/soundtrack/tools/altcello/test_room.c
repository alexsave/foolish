/* stage 3a room and microphone: image arrival times, RT60 against the target, linear Schroeder decay, cardioid rear rejection,
 * source directivity, microphone self-noise level, energy continuity at the image-FDN crossover, determinism. */
#include "fft.h"
#include "room.h"
#include "test_common.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static double dist3(const double *a, const double *b)
{
    return sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}
static int gcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }

/* energy of ir in +-half seconds around t */
static double win_energy(const float *ir, long n, double fs, double t, double half)
{
    long a = (long)floor((t - half) * fs), b = (long)ceil((t + half) * fs);
    double e = 0.0;
    for (long i = a < 0 ? 0 : a; i <= b && i < n; i++) e += (double)ir[i] * ir[i];
    return e;
}

static void t_arrivals(void)
{
    RoomSpec s;
    room_spec_default(&s, 1.5);
    s.pattern = ROOM_OMNI;
    float *ir; long n;
    CHECK(room_ir_build(&s, &ir, &n) == 0, "ir build");
    double c = room_speed(&s), r[7], p[7];
    r[0] = dist3(s.src, s.mic);
    for (int a = 0; a < 3; a++) for (int w = 0; w < 2; w++) {
        double im[3] = {s.src[0], s.src[1], s.src[2]};
        im[a] = w == 0 ? -s.src[a] : 2.0 * s.dim[a] - s.src[a];
        r[1 + 2 * a + w] = dist3(im, s.mic);
    }
    for (int k = 0; k < 7; k++) p[k] = r[k] / c * s.fs;
    for (int k = 0; k < 7; k++) for (int j = k + 1; j < 7; j++)
        CHECK(fabs(p[k] - p[j]) > 8.0, "paths %d and %d only %.1f samples apart: the test window is ambiguous", k, j, fabs(p[k] - p[j]));
    static const char *nm[7] = {"direct", "wall x=0", "wall x=Lx", "wall y=0", "wall y=Ly", "floor", "ceiling"};
    for (int k = 0; k < 7; k++) {
        long c0 = (long)floor(p[k] + 0.5), best = c0;
        for (long i = c0 - 3; i <= c0 + 3; i++) if (fabsf(ir[i]) > fabsf(ir[best])) best = i;
        printf("room arrival %-9s distance %.4f m, expected sample %.2f, IR peak at %ld (|%.4f|), error %+.2f\n", nm[k], r[k], p[k], best,
               fabsf(ir[best]), (double)best - p[k]);
        CHECK(fabs((double)best - p[k]) <= 1.0, "%s arrives at sample %ld, geometric %.2f", nm[k], best, p[k]);
        CHECK(fabsf(ir[best]) > 0.3 * (1.0 / r[k]) * pow(0.88, k > 0), "%s peak %.4f far below 1/r", nm[k], fabsf(ir[best]));
    }
    free(ir);
}

static void t_rt60(void)
{
    static const double T[2] = {0.6, 1.0};
    for (int pat = 0; pat < 2; pat++)
    for (int k = 0; k < 2; k++) {
        RoomSpec s;
        room_spec_default(&s, 1.5);
        s.rt60 = T[k];
        s.pattern = pat == 0 ? ROOM_OMNI : ROOM_CARDIOID;
        float *ir; long n;
        room_ir_build(&s, &ir, &n);
        double *cv = malloc(sizeof(double) * (size_t)n);
        for (int bi = 0; bi < 2; bi++) {
            double fc = bi ? 1000.0 : 500.0;
            double rt = room_rt60(ir, n, s.fs, fc, cv);
            printf("room RT60 %s target %.2f s: %.0f Hz measured %.3f s (%+.1f %%)\n", pat ? "cardioid" : "omni", T[k], fc, rt, 100.0 * (rt / T[k] - 1.0));
            CHECK(fabs(rt / T[k] - 1.0) < 0.10, "RT60 %.0f Hz target %.2f measured %.3f", fc, T[k], rt);
            /* Schroeder curve: least-squares line over -5..-35 dB, max deviation. Checked for the omni mic: the cardioid pointed at the
               source weights the direct sound and the sparse first reflections more, which bends the curve at its start (reported only) */
            double sx = 0, sy = 0, sxx = 0, sxy = 0, m = 0;
            for (long i = 0; i < n; i++) if (cv[i] <= -5.0 && cv[i] >= -35.0) { double t = i / s.fs; sx += t; sy += cv[i]; sxx += t * t; sxy += t * cv[i]; m += 1; }
            double sl = (m * sxy - sx * sy) / (m * sxx - sx * sx), ic = (sy - sl * sx) / m, dev = 0.0;
            for (long i = 0; i < n; i++) if (cv[i] <= -5.0 && cv[i] >= -35.0) { double d = fabs(cv[i] - (ic + sl * i / s.fs)); if (d > dev) dev = d; }
            printf("room Schroeder curve %s %.2f s %.0f Hz: slope %.1f dB/s (RT60 %.3f s), max deviation from the line -5..-35 dB %.2f dB\n",
                   pat ? "cardioid" : "omni", T[k], fc, sl, -60.0 / sl, dev);
            if (pat == 0) CHECK(dev < 1.5, "Schroeder curve %.0f Hz target %.2f deviates %.2f dB from a line", fc, T[k], dev);
        }
        free(cv); free(ir);
    }
}

static double direct_energy(const RoomSpec *s)
{
    float *ir; long n;
    room_ir_build(s, &ir, &n);
    double e = win_energy(ir, n, s->fs, room_direct_time(s), 0.0015);
    free(ir);
    return e;
}

static void t_cardioid(void)
{
    RoomSpec s;
    room_spec_default(&s, 1.5);
    RoomSpec rear = s, omni = s, omni_rear;
    for (int a = 0; a < 3; a++) rear.mic_axis[a] = -s.mic_axis[a];
    omni.pattern = ROOM_OMNI;
    omni_rear = omni;
    for (int a = 0; a < 3; a++) omni_rear.mic_axis[a] = -s.mic_axis[a];
    double front = 10.0 * log10(direct_energy(&s)), back = 10.0 * log10(direct_energy(&rear));
    double of = 10.0 * log10(direct_energy(&omni)), ob = 10.0 * log10(direct_energy(&omni_rear));
    printf("room cardioid: direct sound front %.2f dB, source behind the mic %.2f dB, rejection %.1f dB; omni %.2f / %.2f dB\n", front, back,
           front - back, of, ob);
    CHECK(front - back >= 15.0, "cardioid rear rejection %.1f dB", front - back);
    CHECK(fabs(of - ob) < 0.1, "omni changes by %.2f dB when turned around", of - ob);
    CHECK(fabs(front - of) < 0.1, "cardioid on axis differs from omni by %.2f dB", front - of);
}

static double band_level(const float *ir, long n, double fs, double t, double f0, double f1)
{
    int N = 2048;
    long c = (long)floor(t * fs + 0.5);
    double *re = calloc(N, sizeof(double)), *im = calloc(N, sizeof(double));
    for (int i = 0; i < 1024; i++) { long j = c - 512 + i; if (j >= 0 && j < n) re[i] = ir[j]; }
    fft(re, im, N);
    double e = 0.0;
    for (int k = 1; k < N / 2; k++) { double f = k * fs / N; if (f >= f0 && f < f1) e += re[k] * re[k] + im[k] * im[k]; }
    free(re); free(im);
    return 10.0 * log10(e);
}

static void t_directivity(void)
{
    RoomSpec s;
    room_spec_default(&s, 1.5);
    s.pattern = ROOM_OMNI;
    /* the same cello and mic in the middle of a 60 x 60 x 40 m box so that no reflection lands in the 20 ms analysis window */
    s.dim[0] = 60.0; s.dim[1] = 60.0; s.dim[2] = 40.0;
    for (int a = 0; a < 3; a++) { double off = a == 2 ? 15.0 : 25.0; s.src[a] += off; s.mic[a] += off; }
    RoomSpec back = s; /* the cello turned around: the mic is now behind it */
    back.front[1] = 1.0;
    float *a, *b; long na, nb;
    room_ir_build(&s, &a, &na);
    room_ir_build(&back, &b, &nb);
    double lf = band_level(a, na, s.fs, room_direct_time(&s), 4000, 8000), lb = band_level(b, nb, s.fs, room_direct_time(&back), 4000, 8000);
    double lf2 = band_level(a, na, s.fs, room_direct_time(&s), 60, 250), lb2 = band_level(b, nb, s.fs, room_direct_time(&back), 60, 250);
    printf("room source directivity: 4-8 kHz front %.2f dB, behind %.2f dB, difference %.2f dB; 60-250 Hz difference %.2f dB\n", lf, lb, lf - lb, lf2 - lb2);
    CHECK(fabs((lf - lb) - 3.0) < 0.4, "front-back at 4-8 kHz is %.2f dB, want 3", lf - lb);
    CHECK(fabs(lf2 - lb2) < 0.3, "front-back below 250 Hz is %.2f dB, want 0", lf2 - lb2);
    free(a); free(b);
}

static void t_noise(void)
{
    RoomSpec s;
    room_spec_default(&s, 1.5);
    float *ir; long n;
    room_ir_build(&s, &ir, &n);
    long nin = 480000;
    float *in = calloc((size_t)nin, sizeof(float)), *out;
    long nout = room_apply(&s, ir, n, in, nin, &out);
    CHECK(nout == nin + n - 1, "output length %ld", nout);
    int N = 262144;
    double *re = calloc(N, sizeof(double)), *im = calloc(N, sizeof(double));
    for (int i = 0; i < N; i++) re[i] = out[100000 + i];
    double raw = 0.0;
    for (int i = 0; i < N; i++) raw += re[i] * re[i];
    fft(re, im, N);
    double ea = 0.0;
    for (int k = 0; k < N; k++) {
        int kk = k <= N / 2 ? k : N - k;
        double f = kk * s.fs / N, a = f < 20.0 ? 0.0 : pow(10.0, room_aweight_db(f) / 20.0);
        ea += (re[k] * re[k] + im[k] * im[k]) * a * a;
    }
    double dba = 10.0 * log10(ea / ((double)N * N)) + s.ref_db_spl; /* mean square after weighting, re rms 1 = 74 dB SPL */
    printf("room noise: unweighted %.1f dB re rms 1 (= %.1f dB SPL), A-weighted %.2f dB(A) SPL, spec %.1f dB(A)\n", 10.0 * log10(raw / N),
           10.0 * log10(raw / N) + s.ref_db_spl, dba, s.self_noise_dba);
    CHECK(fabs(dba - s.self_noise_dba) < 2.0, "noise floor %.2f dB(A), spec %.1f", dba, s.self_noise_dba);
    CHECK(fabs(room_aweight_db(1000.0)) < 0.05, "A(1 kHz) = %.3f dB", room_aweight_db(1000.0));
    CHECK(fabs(room_aweight_db(100.0) + 19.1) < 0.2, "A(100 Hz) = %.2f dB, table -19.1", room_aweight_db(100.0));
    CHECK(fabs(room_aweight_db(10000.0) + 2.5) < 0.3, "A(10 kHz) = %.2f dB, table -2.5", room_aweight_db(10000.0));
    /* with a signal, the sounding part is at rms 1 */
    for (long i = 0; i < 96000; i++) in[i] = (float)(0.3 * sin(2.0 * M_PI * 220.0 * i / 48000.0));
    free(out);
    nout = room_apply(&s, ir, n, in, nin, &out);
    double p = 0.0;
    for (long i = 20000; i < 90000; i++) p += (double)out[i] * out[i];
    printf("room level: sounding rms %.3f (want 1.0)\n", sqrt(p / 70000.0));
    CHECK(fabs(sqrt(p / 70000.0) - 1.0) < 0.1, "sounding rms %.3f", sqrt(p / 70000.0));
    free(re); free(im); free(in); free(out); free(ir);
}

static void t_determinism(void)
{
    RoomSpec s;
    room_spec_default(&s, 1.5);
    float *a, *b, *c; long na, nb, nc;
    room_ir_build(&s, &a, &na);
    room_ir_build(&s, &b, &nb);
    CHECK(na == nb && !memcmp(a, b, sizeof(float) * (size_t)na), "same seed gives a different IR");
    RoomSpec s2 = s;
    s2.seed = s.seed + 1;
    room_ir_build(&s2, &c, &nc);
    CHECK(memcmp(a, c, sizeof(float) * (size_t)na), "a different seed gives the same IR: the FDN signs do not come from rng");
    float in[4800];
    for (int i = 0; i < 4800; i++) in[i] = (float)sin(0.05 * i);
    float *o1, *o2, *o3;
    long m1 = room_apply(&s, a, na, in, 4800, &o1), m2 = room_apply(&s, a, na, in, 4800, &o2), m3 = room_apply(&s2, a, na, in, 4800, &o3);
    CHECK(m1 == m2 && !memcmp(o1, o2, sizeof(float) * (size_t)m1), "same seed gives a different wet signal");
    CHECK(m3 == m1 && memcmp(o1, o3, sizeof(float) * (size_t)m1), "a different seed gives the same noise");
    free(a); free(b); free(c); free(o1); free(o2); free(o3);
}

static void t_misc(void)
{
    RoomSpec s;
    room_spec_default(&s, 1.5);
    int nl;
    const int *L = room_fdn_lengths(&nl);
    for (int i = 0; i < nl; i++) for (int j = i + 1; j < nl; j++) CHECK(gcd(L[i], L[j]) == 1, "FDN lengths %d and %d share a factor", L[i], L[j]);
    double V = s.dim[0] * s.dim[1] * s.dim[2], S = 2.0 * (s.dim[0] * s.dim[1] + s.dim[0] * s.dim[2] + s.dim[1] * s.dim[2]);
    double al = room_wall_alpha(&s, 0.8);
    CHECK(fabs(0.161 * V / (-S * log(1.0 - al)) / 0.8 - 1.0) < 0.01, "Eyring: 0.161 V / (-S ln(1-alpha)) = %.3f s", 0.161 * V / (-S * log(1.0 - al)));
    CHECK(al > 0.05 && al < 0.5, "alpha %.3f", al);
    double a1 = room_air_db_per_m(1000.0, 20.0, 50.0) * 1000.0, a4 = room_air_db_per_m(4000.0, 20.0, 50.0) * 1000.0,
           a8 = room_air_db_per_m(8000.0, 20.0, 50.0) * 1000.0;
    printf("room air absorption (ISO 9613-1 formula, 20 C 50 %% RH): 1 kHz %.2f, 4 kHz %.1f, 8 kHz %.1f dB/km (REFERENCES recalled 4.7, 37, 117)\n", a1, a4, a8);
    CHECK(a1 > 3.0 && a1 < 7.0 && a4 > 25.0 && a4 < 50.0 && a8 > 80.0 && a8 < 160.0, "air absorption out of the textbook range");
    /* energy continuity across the image/FDN crossover: 10 ms windows just before and after */
    float *ir; long n;
    room_ir_build(&s, &ir, &n);
    double tx = room_crossover_time(&s);
    double eb = win_energy(ir, n, s.fs, tx - 0.015, 0.005), ea = win_energy(ir, n, s.fs, tx + 0.015, 0.005);
    double db = 10.0 * log10(ea / eb), expect = -60.0 * 0.030 / s.rt60; /* the decay over 30 ms at the mid band target */
    printf("room crossover at %.1f ms; energy 15 ms after / 15 ms before: %.2f dB (the decay alone gives about %.1f dB)\n", tx * 1000.0, db, expect);
    CHECK(fabs(db - expect) < 3.0, "energy step across the crossover %.2f dB, decay alone %.2f", db, expect);
    CHECK(tx > room_direct_time(&s) + 0.02, "crossover %.1f ms", tx * 1000.0);
    free(ir);
}

int main(void)
{
    t_arrivals();
    t_rt60();
    t_cardioid();
    t_directivity();
    t_noise();
    t_determinism();
    t_misc();
    TEST_DONE("test_room");
}
