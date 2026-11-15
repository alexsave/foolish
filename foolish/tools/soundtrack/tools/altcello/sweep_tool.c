/* Schelleng sweeps with two classifiers, for our finite difference model and for the MSW oracle (oracle_msw.h): a diagnostic
 * tool, not part of the shipped instrument.
 *
 *   sweep_tool oracle STRING BETA FMIN FMAX NF SECS OUT [--vb X] [--q2k-free]
 *   sweep_tool ours   STRING BETA FMIN FMAX NF SECS OUT [--vb X] [--body] [--no-noise] [--no-torsion] [--pt-bow] [--stiff X]
 *   sweep_tool onset  STRING BETA F SECS [--model oracle|ours] [--vb X]          (B4: time to steady Helmholtz, force-first start)
 *   sweep_tool wave   STRING BETA F SECS OUT [--model oracle|ours] [--vb X] [--body] ...   (the last 3 periods: t, v at the bow, v_rel, bridge force)
 *
 * F log spaced FMIN..FMAX, NF points, each run SECS long; v_B and F_B ramp in over 30 ms (like test_schelleng). The classifiers
 * (both on the last 1.5 s, 20 periods): "ours" = analyse_helmholtz (every one of 20 periods holds exactly one slip interval of
 * at least 0.15 ms), "simple" = analyse_simple (every period holds one dominant slip, slips under 20 percent of the main slip's
 * displacement ignored). OUT gets one row per force: F ours simple counts_min counts_max slip_frac corr f_est cents bad_periods.
 * The summary line gives, per classifier, F_min, F_max (first and last Helmholtz force), the Helmholtz count and the longest
 * contiguous run (its F_min, F_max). */
#include "analysis.h"
#include "oracle_msw.h"
#include "sim.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXNF 64

typedef struct {
    double F[MAXNF];
    int ours[MAXNF], simple[MAXNF];
    int n;
} Sweep;

static void summary(const char *tag, const double *F, const int *ok, int n)
{
    int first = -1, last = -1, cnt = 0, bs = -1, bl = 0, rs = -1, rl = 0;
    for (int i = 0; i < n; i++) {
        if (ok[i]) {
            if (first < 0) first = i;
            last = i;
            cnt++;
            if (rs < 0) rs = i;
            rl++;
            if (rl > bl) { bl = rl; bs = rs; }
        } else { rs = -1; rl = 0; }
    }
    if (first < 0) { printf("SUMMARY %s npts 0\n", tag); return; }
    printf("SUMMARY %s npts %d Fmin %.4f Fmax %.4f ratio %.2f | contiguous %d pts Fmin %.4f Fmax %.4f ratio %.2f\n", tag, cnt, F[first], F[last],
           F[last] / F[first], bl, F[bs], F[bs + bl - 1], F[bs + bl - 1] / F[bs]);
}

static void classify_and_write(FILE *out, double F, const double *bridge, const double *vrel, long n, double fs, double f0, double settle,
                               int *ok_ours, int *ok_simple)
{
    Helm h = analyse_helmholtz(bridge, vrel, n, fs, f0, settle);
    SimpleHelm s = analyse_simple(vrel, n, fs, h.f_est, settle, 0.2);
    *ok_ours = h.ok_one_per_period;
    *ok_simple = s.ok;
    fprintf(out, "%.5f %d %d %d %d %.4f %.4f %.3f %.2f %d\n", F, h.ok_one_per_period, s.ok, h.counts_min, h.counts_max, h.slip_frac, h.corr, h.f_est,
            h.cents_flat, s.bad_periods);
    printf("F %8.4f ours %d simple %d counts %d..%d slip %.3f corr %.3f %+.1f c\n", F, h.ok_one_per_period, s.ok, h.counts_min, h.counts_max, h.slip_frac, h.corr,
           h.cents_flat);
    fflush(stdout);
}

static double rampv(double t, double ramp, double v) { return t >= ramp ? v : v * t / ramp; }

static void oracle_run(Oracle *o, double vB, double F, double ramp, double fs, long n, double *bridge, double *vrel)
{
    for (long i = 0; i < n; i++) {
        double t = (double)i / fs;
        oracle_step(o, rampv(t, ramp, vB), rampv(t, ramp, F), 0.0, &vrel[i], &bridge[i]);
    }
}

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: see sweep_tool.c\n"); return 2; }
    const char *mode = argv[1];
    const Instrument *I = instrument_cello();
    const StringSpec *sp = instrument_string(I, argv[2]);
    if (!sp) { fprintf(stderr, "unknown string\n"); return 2; }
    int si = instrument_string_index(I, argv[2]);
    double beta = atof(argv[3]), fs = 96000.0, vB = 0.1;
    int body = 0, noise = 1, torsion = 1, ptbow = 0, use_ours_model = 0;
    double stiff = 1.0;
    int a0 = !strcmp(mode, "onset") ? 6 : !strcmp(mode, "wave") ? 7 : 9;
    for (int i = a0; i < argc; i++) {
        if (!strcmp(argv[i], "--vb") && i + 1 < argc) vB = atof(argv[++i]);
        else if (!strcmp(argv[i], "--body")) body = 1;
        else if (!strcmp(argv[i], "--no-noise")) noise = 0;
        else if (!strcmp(argv[i], "--no-torsion")) torsion = 0;
        else if (!strcmp(argv[i], "--pt-bow")) ptbow = 1;
        else if (!strcmp(argv[i], "--stiff") && i + 1 < argc) stiff = atof(argv[++i]);
        else if (!strcmp(argv[i], "--model") && i + 1 < argc) use_ours_model = !strcmp(argv[++i], "ours");
    }
    Instrument ic = *I;
    ic.str[si].EI *= stiff;

    if (!strcmp(mode, "wave")) {
        double F = atof(argv[4]), secs = atof(argv[5]);
        long n = (long)(secs * fs);
        double *bridge = calloc((size_t)n, sizeof(double)), *vrel = calloc((size_t)n, sizeof(double)), *vbow = calloc((size_t)n, sizeof(double));
        if (use_ours_model) {
            Sim S;
            if (body) {
                SimOpts so = {1, 1, 0, NULL};
                sim_init_instrument(&S, &ic, fs, &so, argv[2]);
            } else sim_init(&S, &ic, &ic.str[si], fs);
            S.torsion_on = torsion;
            S.bow_noise_level = noise ? ic.bow.noise_level : 0.0;
            if (ptbow) S.bow_width = 1e-7;
            sim_set_bow(&S, beta, vB, F, 0.03);
            sim_start(&S);
            Trace *tr = sim_run(&S, secs, NULL, NULL);
            memcpy(bridge, tr->ch[0], sizeof(double) * (size_t)n);
            memcpy(vbow, tr->ch[2], sizeof(double) * (size_t)n);
            memcpy(vrel, tr->ch[3], sizeof(double) * (size_t)n);
            trace_free(tr);
            sim_free(&S);
        } else {
            OracleSpec os;
            oracle_spec_from_string(&ic, &ic.str[si], fs, beta, 4000.0, &os, NULL, NULL);
            Oracle *o = oracle_new(&os);
            oracle_run(o, vB, F, 0.03, fs, n, bridge, vrel);
            for (long i = 0; i < n; i++) vbow[i] = vrel[i] + rampv((double)i / fs, 0.03, vB);
            oracle_free(o);
        }
        FILE *wf = fopen(argv[6], "w");
        long P = (long)(fs / sp->f0 + 0.5), i0 = n - (long)(3 * P) - 1;
        fprintf(wf, "# t_ms v_bow vrel bridge\n");
        for (long i = i0; i < n; i++) fprintf(wf, "%.4f %.6f %.6f %.6f\n", 1000.0 * (double)(i - i0) / fs, vbow[i], vrel[i], bridge[i]);
        fclose(wf);
        return 0;
    }
    if (!strcmp(mode, "onset")) { /* B4 */
        double F = atof(argv[4]), secs = atof(argv[5]);
        long n = (long)(secs * fs);
        double *bridge = calloc((size_t)n, sizeof(double)), *vrel = calloc((size_t)n, sizeof(double));
        double ramp = 0.05;
        if (use_ours_model) {
            Sim S;
            SimOpts so = {1, 1, 0, NULL};
            sim_init_instrument(&S, &ic, fs, &so, argv[2]);
            S.torsion_on = torsion;
            S.bow_noise_level = noise ? ic.bow.noise_level : 0.0;
            env_const(&S.beta, beta);
            env_const(&S.FB, F); /* force first: at full value from t = 0 */
            S.vB.n = 0;
            env_add(&S.vB, 0.0, 0.0);
            env_add(&S.vB, ramp, vB);
            sim_start(&S);
            Trace *tr = sim_run(&S, secs, NULL, NULL);
            memcpy(bridge, tr->ch[0], sizeof(double) * (size_t)n);
            memcpy(vrel, tr->ch[3], sizeof(double) * (size_t)n);
            trace_free(tr);
            sim_free(&S);
        } else {
            OracleSpec os;
            double err = oracle_spec_from_string(&ic, &ic.str[si], fs, beta, 4000.0, &os, NULL, NULL);
            (void)err;
            Oracle *o = oracle_new(&os);
            for (long i = 0; i < n; i++) {
                double t = (double)i / fs;
                oracle_step(o, t >= ramp ? vB : vB * t / ramp, F, 0.0, &vrel[i], &bridge[i]);
            }
            oracle_free(o);
        }
        double P = fs / sp->f0;
        double ts = settle_time(vrel, n, fs, P);
        Helm h = analyse_helmholtz(bridge, vrel, n, fs, sp->f0, secs - 1.0);
        printf("ONSET model %s F %.4f settle_time %.4f s  final: one-per-period %d slip_frac %.3f\n", use_ours_model ? "ours" : "oracle", F, ts, h.ok_one_per_period,
               h.slip_frac);
        free(bridge);
        free(vrel);
        return 0;
    }

    double fmin = atof(argv[4]), fmax = atof(argv[5]);
    int nf = atoi(argv[6]);
    double secs = atof(argv[7]);
    FILE *out = fopen(argv[8], "w");
    if (!out || nf > MAXNF) { fprintf(stderr, "bad output or nf\n"); return 2; }
    fprintf(out, "# F ours simple counts_min counts_max slip_frac corr f_est cents bad_periods\n");
    long n = (long)(secs * fs);
    double settle = secs - 1.5, ramp = 0.03;
    Sweep sw = {{0}, {0}, {0}, nf};
    OracleSpec os;
    if (!strcmp(mode, "oracle")) {
        double err = oracle_spec_from_string(&ic, &ic.str[si], fs, beta, 4000.0, &os, NULL, NULL);
        printf("oracle %s: Z0 %.4f g %.5f p %.4f (loss fit rms relative error %.3f)\n", argv[2], os.Z0, os.g, os.p, err);
    }
    for (int i = 0; i < nf; i++) {
        double F = nf > 1 ? fmin * pow(fmax / fmin, (double)i / (nf - 1)) : fmin;
        double *bridge, *vrel;
        if (!strcmp(mode, "oracle")) {
            bridge = calloc((size_t)n, sizeof(double));
            vrel = calloc((size_t)n, sizeof(double));
            Oracle *o = oracle_new(&os);
            oracle_run(o, vB, F, ramp, fs, n, bridge, vrel);
            oracle_free(o);
        } else {
            Sim S;
            if (body) {
                SimOpts so = {1, 1, 0, NULL};
                sim_init_instrument(&S, &ic, fs, &so, argv[2]);
            } else sim_init(&S, &ic, &ic.str[si], fs);
            S.torsion_on = torsion;
            S.bow_noise_level = noise ? ic.bow.noise_level : 0.0;
            if (ptbow) S.bow_width = 1e-7; /* narrower than the grid: one grid point */
            sim_set_bow(&S, beta, vB, F, ramp);
            sim_start(&S);
            Trace *tr = sim_run(&S, secs, NULL, NULL);
            bridge = tr->ch[0];
            vrel = tr->ch[3];
            tr->ch[0] = tr->ch[3] = NULL;
            trace_free(tr);
            sim_free(&S);
        }
        classify_and_write(out, F, bridge, vrel, n, fs, sp->f0, settle, &sw.ours[i], &sw.simple[i]);
        sw.F[i] = F;
        free(bridge);
        free(vrel);
    }
    fclose(out);
    char tag[64];
    snprintf(tag, sizeof tag, "%s %s beta %.2f ours-classifier", mode, argv[2], beta);
    summary(tag, sw.F, sw.ours, nf);
    snprintf(tag, sizeof tag, "%s %s beta %.2f simple-classifier", mode, argv[2], beta);
    summary(tag, sw.F, sw.simple, nf);
    return 0;
}
