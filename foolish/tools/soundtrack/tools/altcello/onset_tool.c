/* Onset and legato diagnostics (a diagnostic tool, not part of the shipped instrument; ONSET.md of the alt_cello folder).
 *
 *   onset_tool wav RAW.f32 FS F0 [--from T]
 *       the audio measures (analysis.h) of a mono float32 file: onset, time to periodic (r > 0.95 for 10 periods, after the onset)
 *   onset_tool grid STRING SEMIS ours|oracle OUT [--nf 12] [--widen 2] [--beta 0.1] [--vend 0.1] [--secs 1.1] [--seed N] [--accs a,b,..]
 *       Guettler diagram: for each of nf forces (log spaced over the playability band of the string at the SOUNDING beta and v_end,
 *       widened by `widen` on each side) and each constant bow acceleration a (v_B from 0 to vend in vend / a seconds), force applied
 *       0.02 s before the motion starts. SEMIS: finger position in semitones above the open string (0 = open). ours = the finite
 *       difference cello (body on, all strings); oracle = the MSW waveguide (open string only, bridge force as the sound).
 *       One row per cell: F a t_audio t_slip e28 steady_ok
 *         t_audio: time after the audio onset to the first run of 10 periods with r > 0.95 (-1: never within the run)
 *         t_slip: time after the bow starts moving to the first slip onset that begins 10 consecutive one-slip periods (-1: never)
 *         e28: share of the 2-8 kHz band in the first 0.2 s after the onset minus the same share in the last 0.4 s, dB
 *   onset_tool fing ...   (the finger-change experiments, see fing_main)
 */
#include "analysis.h"
#include "oracle_msw.h"
#include "player.h"
#include "sim.h"
#include "wav.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS96 96000.0


typedef struct {
    long n;
    double *vrel, *bridge; /* 96 kHz */
    double *aud;           /* the sound at audfs */
    long naud;
    double audfs;
} Rec;

static void rec_free(Rec *r)
{
    free(r->vrel);
    free(r->bridge);
    free(r->aud);
    memset(r, 0, sizeof *r);
}

/* run an initialised sim for secs, record vrel, bridge force and the radiated pressure at 48 kHz */
static void rec_sim(Sim *S, double secs, Rec *r)
{
    long n = (long)floor(secs * S->fs + 0.5);
    double *pr = malloc(sizeof(double) * (size_t)n);
    r->n = n;
    r->vrel = malloc(sizeof(double) * (size_t)n);
    r->bridge = malloc(sizeof(double) * (size_t)n);
    for (long i = 0; i < n; i++) {
        sim_step(S);
        pr[i] = S->out[7];
        r->vrel[i] = S->out[3];
        r->bridge[i] = S->out[0];
    }
    float *y;
    int fac = (int)(S->fs / 48000.0 + 0.5);
    r->naud = wav_decimate(pr, n, fac, &y);
    r->aud = malloc(sizeof(double) * (size_t)r->naud);
    for (long i = 0; i < r->naud; i++) r->aud[i] = y[i];
    r->audfs = S->fs / fac;
    free(y);
    free(pr);
}

/* ---------------------------------------------------------------- the grid */

static double finger_for(const Instrument *I, int si, int semis)
{
    const StringSpec *sp = &I->str[si];
    double fn = sp->f0 * pow(2.0, semis / 12.0), x = sp->L * (1.0 - sp->f0 / fn);
    if (semis < INSTR_MAXSEMI) x = sim_vib_finger(sp->L, x, I->stop_cents[si][semis]);
    return x;
}

typedef struct {
    int si, semis;
    double beta_rel, F, a, vend, secs, t_motion;
    int model_ours; /* else the oracle */
    uint64_t seed;
} Cell;

static void cell_run(const Instrument *I, const Cell *c, Rec *rec)
{
    const StringSpec *sp = &I->str[c->si];
    double ratio = c->semis > 0 ? pow(2.0, c->semis / 12.0) : 1.0;
    double beta = c->beta_rel / ratio;
    double T = c->vend / c->a;
    if (c->model_ours) {
        Sim S;
        SimOpts so = {1, 1, 0, NULL};
        sim_init_instrument(&S, I, FS96, &so, sp->name);
        env_const(&S.beta, beta);
        S.FB.n = 0;
        env_add(&S.FB, 0.0, 0.0);
        env_add(&S.FB, 0.005, c->F);
        S.vB.n = 0;
        env_add(&S.vB, 0.0, 0.0);
        env_add(&S.vB, c->t_motion, 0.0);
        env_add(&S.vB, c->t_motion + T, c->vend);
        if (c->semis > 0) {
            env_const(&S.xF, finger_for(I, c->si, c->semis));
            env_const(&S.aF, 1.0);
        }
        sim_start(&S);
        if (c->seed) rng_seed(&S.rng, ALTCELLO_SEED ^ (c->seed * 0x9e3779b97f4a7c15ULL));
        rec_sim(&S, c->secs, rec);
        sim_free(&S);
    } else {
        OracleSpec os;
        oracle_spec_from_string(I, sp, FS96, c->beta_rel, 4000.0, &os, NULL, NULL);
        Oracle *o = oracle_new(&os);
        long n = (long)floor(c->secs * FS96 + 0.5);
        rec->n = n;
        rec->vrel = malloc(sizeof(double) * (size_t)n);
        rec->bridge = malloc(sizeof(double) * (size_t)n);
        for (long i = 0; i < n; i++) {
            double t = (double)i / FS96;
            double v = t < c->t_motion ? 0.0 : t < c->t_motion + T ? c->vend * (t - c->t_motion) / T : c->vend;
            oracle_step(o, v, t < 0.005 ? c->F * t / 0.005 : c->F, 0.0, &rec->vrel[i], &rec->bridge[i]);
        }
        oracle_free(o);
        rec->aud = malloc(sizeof(double) * (size_t)n);
        memcpy(rec->aud, rec->bridge, sizeof(double) * (size_t)n); /* the bridge force is the oracle's sound */
        rec->naud = n;
        rec->audfs = FS96;
    }
}

typedef struct {
    double t_audio, t_slip, e28, onset;
    int steady; /* the last 0.4 s is Helmholtz motion by both classifiers (analysis.h) */
} CellRes;

static CellRes cell_measure(const Cell *c, const Rec *r, double f_sound)
{
    CellRes o = {-1.0, -1.0, 0.0, -1.0, 0};
    o.onset = audio_onset_from(r->aud, r->naud, r->audfs, c->t_motion);
    double tp = o.onset >= 0 ? audio_periodic_time(r->aud, r->naud, r->audfs, f_sound, o.onset, 0.95, 10) : -1.0;
    if (tp >= 0) o.t_audio = tp - o.onset;
    double ts = settle_time(r->vrel, r->n, FS96, FS96 / f_sound);
    if (ts >= 0) o.t_slip = ts - c->t_motion;
    if (o.onset >= 0) {
        long a0 = (long)(o.onset * r->audfs), a1 = a0 + (long)(0.2 * r->audfs), b1 = r->naud, b0 = b1 - (long)(0.4 * r->audfs);
        if (a1 < b0) o.e28 = band_share_db(r->aud, a0, a1, r->audfs, 2000.0, 8000.0) - band_share_db(r->aud, b0, b1, r->audfs, 2000.0, 8000.0);
    }
    {
        double settle = (double)r->n / FS96 - 0.4;
        Helm h = analyse_helmholtz(r->bridge, r->vrel, r->n, FS96, f_sound, settle);
        SimpleHelm s = analyse_simple(r->vrel, r->n, FS96, h.f_est, settle, 0.2);
        o.steady = h.ok_one_per_period && s.ok;
    }
    return o;
}

static int grid_main(int argc, char **argv)
{
    if (argc < 6) { fprintf(stderr, "usage: onset_tool grid STRING SEMIS ours|oracle OUT [options]\n"); return 2; }
    const Instrument *I = instrument_cello();
    int si = instrument_string_index(I, argv[2]);
    if (si < 0) { fprintf(stderr, "unknown string\n"); return 2; }
    Cell c = {si, atoi(argv[3]), 0.1, 0, 0, 0.1, 1.1, 0.02, !strcmp(argv[4], "ours"), 0};
    int nf = 12;
    double widen = 2.0;
    double accs[16] = {0.25, 0.5, 1, 2, 4, 8};
    int na = 6;
    for (int i = 6; i < argc; i++) {
        if (!strcmp(argv[i], "--nf") && i + 1 < argc) nf = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--widen") && i + 1 < argc) widen = atof(argv[++i]);
        else if (!strcmp(argv[i], "--beta") && i + 1 < argc) c.beta_rel = atof(argv[++i]);
        else if (!strcmp(argv[i], "--vend") && i + 1 < argc) c.vend = atof(argv[++i]);
        else if (!strcmp(argv[i], "--secs") && i + 1 < argc) c.secs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) c.seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--accs") && i + 1 < argc) {
            na = 0;
            for (char *s = strtok(argv[++i], ","); s && na < 16; s = strtok(NULL, ",")) accs[na++] = atof(s);
        }
    }
    FILE *out = fopen(argv[5], "w");
    if (!out) { perror(argv[5]); return 1; }
    double fmin, fmax;
    if (!player_band(I, si, c.beta_rel, c.vend, &fmin, &fmax)) { fmin = 0.5 * I->helmholtz_force[si]; fmax = 2.0 * I->helmholtz_force[si]; }
    double f_sound = I->str[si].f0 * pow(2.0, c.semis / 12.0);
    fprintf(out, "# string %s semis %d model %s beta_rel %.3f vend %.3f band %.4f %.4f widen %.2f f_sound %.3f\n# F a t_audio t_slip e28 onset steady\n", argv[2], c.semis,
            argv[4], c.beta_rel, c.vend, fmin, fmax, widen, f_sound);
    for (int i = 0; i < nf; i++) {
        c.F = fmin / widen * pow(fmax * widen / (fmin / widen), nf > 1 ? (double)i / (nf - 1) : 0.5);
        for (int j = 0; j < na; j++) {
            c.a = accs[j];
            Rec r;
            cell_run(I, &c, &r);
            CellRes m = cell_measure(&c, &r, f_sound);
            fprintf(out, "%.5f %.3f %.4f %.4f %.2f %.4f %d\n", c.F, c.a, m.t_audio, m.t_slip, m.e28, m.onset, m.steady);
            fflush(out);
            rec_free(&r);
        }
    }
    fclose(out);
    return 0;
}

static int wav_main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: onset_tool wav RAW.f32 FS F0 [--from T]\n"); return 2; }
    FILE *f = fopen(argv[2], "rb");
    if (!f) { perror(argv[2]); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f) / 4;
    fseek(f, 0, SEEK_SET);
    float *xf = malloc(sizeof(float) * (size_t)n);
    if (fread(xf, 4, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);
    double *x = malloc(sizeof(double) * (size_t)n);
    for (long i = 0; i < n; i++) x[i] = xf[i];
    double fs = atof(argv[3]), f0 = atof(argv[4]), from = -1.0;
    for (int i = 5; i < argc; i++) if (!strcmp(argv[i], "--from") && i + 1 < argc) from = atof(argv[++i]);
    double on = from >= 0 ? from : audio_onset(x, n, fs);
    double tp = on >= 0 ? audio_periodic_time(x, n, fs, f0, on, 0.95, 10) : -1.0;
    printf("onset %.4f t_periodic %.4f\n", on, tp >= 0 ? tp - on : -1.0);
    return 0;
}

int fing_main(int argc, char **argv);

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: onset_tool wav|grid|fing ...\n"); return 2; }
    if (!strcmp(argv[1], "wav")) return wav_main(argc, argv);
    if (!strcmp(argv[1], "grid")) return grid_main(argc, argv);
    if (!strcmp(argv[1], "fing")) return fing_main(argc, argv);
    fprintf(stderr, "unknown mode %s\n", argv[1]);
    return 2;
}

/* ---------------------------------------------------------------- finger and bow-change experiments
 *   onset_tool fing slur|lift|bow STR [--ramp S] [--K X] [--R X] [--width M] [--dip D] [--seed N] [--tsw S] [--F N] [--trace OUT]
 * One continuous bow (constant v_B 0.1 m/s and the force in the middle of the playability band, no direction change) except in `bow`.
 *   slur: STR = G2 (the default), finger from 7 semitones (D3) to 9 (E3) at t_sw: the finger lifts and lands as in the player: the press dips
 *         by `dip` (0.8) over `ramp` (0.010 s) with a sin shape and the position swaps in the middle.
 *   lift: STR = D3, finger at 2 semitones (E3) lifts to the open string (D3) over `ramp`, press falling with a raised cosine.
 *   bow:  STR = G2, finger at 7 semitones (D3), the bow reverses through zero speed over 0.04 s with the force dipping to 0.75.
 * K, R, width are multipliers of the instrument's pad values (K 2e7, R 200, width 0.010) except width, which is metres.
 * Prints one line: t_pitch (s from the start of the finger event until the pitch is within 10 cents of the final pitch for 5 periods),
 * t_pitch_ramp (the same from the end of the ramp), t_reper (audio: the first time after the event start of 10 periods with r > 0.95 at
 * the new pitch, minus the event start), dip_c (lowest cents re the final pitch within 0.2 s), lev_dip_db (lowest 5 ms rms re the level before). */
static double rms_at(const double *x, long n, double fs, double t, double w)
{
    long a = (long)(t * fs), b = a + (long)(w * fs);
    if (a < 0) a = 0;
    if (b > n) b = n;
    double s = 0.0;
    for (long i = a; i < b; i++) s += x[i] * x[i];
    return sqrt(s / (double)(b - a + 1));
}

int fing_main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: onset_tool fing slur|lift|bow STR [options]\n"); return 2; }
    const char *scen = argv[2];
    Instrument ic = *instrument_cello();
    int si = instrument_string_index(&ic, argv[3]);
    if (si < 0) { fprintf(stderr, "unknown string\n"); return 2; }
    double ramp = 0.010, dip = 0.8, tsw = 1.0, F = -1.0, vB = 0.1;
    uint64_t seed = 0;
    const char *trace = NULL;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--ramp") && i + 1 < argc) ramp = atof(argv[++i]);
        else if (!strcmp(argv[i], "--K") && i + 1 < argc) ic.finger.K *= atof(argv[++i]);
        else if (!strcmp(argv[i], "--R") && i + 1 < argc) ic.finger.R *= atof(argv[++i]);
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) ic.finger.width = atof(argv[++i]);
        else if (!strcmp(argv[i], "--dip") && i + 1 < argc) dip = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--tsw") && i + 1 < argc) tsw = atof(argv[++i]);
        else if (!strcmp(argv[i], "--F") && i + 1 < argc) F = atof(argv[++i]);
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace = argv[++i];
    }
    int s0 = !strcmp(scen, "lift") ? 2 : 7, s1 = !strcmp(scen, "slur") ? 9 : !strcmp(scen, "lift") ? 0 : 7;
    const StringSpec *sp = &ic.str[si];
    double beta_rel = 0.1, beta = beta_rel / pow(2.0, s0 / 12.0);
    double lo, hi;
    if (F < 0.0) { if (!player_band(&ic, si, beta_rel, vB, &lo, &hi)) { lo = 0.5 * ic.helmholtz_force[si]; hi = 2.0 * ic.helmholtz_force[si]; } F = sqrt(lo * hi); }
    double x0 = finger_for(&ic, si, s0), x1 = s1 > 0 ? finger_for(&ic, si, s1) : x0;
    double f_old = sp->f0 * pow(2.0, s0 / 12.0), f_new = sp->f0 * pow(2.0, s1 / 12.0);
    Sim S;
    SimOpts so = {1, 1, 0, NULL};
    sim_init_instrument(&S, &ic, FS96, &so, sp->name);
    env_const(&S.beta, beta);
    S.FB.n = 0;
    env_add(&S.FB, 0.0, 0.0);
    env_add(&S.FB, 0.005, F);
    S.vB.n = 0;
    env_add(&S.vB, 0.0, 0.0);
    env_add(&S.vB, 0.02, 0.0);
    env_add(&S.vB, 0.02 + vB / 2.0, vB); /* a = 2 m/s^2 */
    S.xF.n = 0;
    S.aF.n = 0;
    double t_end_ev = tsw + ramp;
    if (!strcmp(scen, "slur")) {
        env_add(&S.xF, 0.0, x0);
        env_add(&S.xF, tsw + 0.5 * ramp - 1e-5, x0);
        env_add(&S.xF, tsw + 0.5 * ramp, x1);
        env_add(&S.aF, 0.0, 1.0);
        for (int k = 0; k <= 8; k++) env_add(&S.aF, tsw + ramp * k / 8.0, 1.0 - dip * sin(M_PI * k / 8.0));
    } else if (!strcmp(scen, "lift")) {
        env_add(&S.xF, 0.0, x0);
        env_add(&S.aF, 0.0, 1.0);
        for (int k = 0; k <= 8; k++) env_add(&S.aF, tsw + ramp * k / 8.0, 0.5 * (1.0 + cos(M_PI * k / 8.0)));
    } else { /* bow change */
        env_const(&S.xF, x0);
        env_const(&S.aF, 1.0);
        S.vB.n = 0;
        env_add(&S.vB, 0.0, 0.0);
        env_add(&S.vB, 0.02, 0.0);
        env_add(&S.vB, 0.02 + vB / 2.0, vB);
        env_add(&S.vB, tsw - 0.02, vB);
        env_add(&S.vB, tsw + 0.02, -vB);
        S.FB.n = 0;
        env_add(&S.FB, 0.0, 0.0);
        env_add(&S.FB, 0.005, F);
        env_add(&S.FB, tsw - 0.02, F);
        env_add(&S.FB, tsw, 0.75 * F);
        env_add(&S.FB, tsw + 0.02, F);
        t_end_ev = tsw + 0.02;
    }
    sim_start(&S);
    if (seed) rng_seed(&S.rng, ALTCELLO_SEED ^ (seed * 0x9e3779b97f4a7c15ULL));
    Rec r;
    rec_sim(&S, tsw + 0.7, &r);
    sim_free(&S);
    double fs = r.audfs;
    /* the state before: periodic at the old pitch? */
    double pre = audio_periodic_time(r.aud, r.naud, fs, f_old, tsw - 0.3, 0.95, 10);
    int pre_ok = pre >= 0 && pre < tsw - 0.2;
    double t_ev = !strcmp(scen, "bow") ? tsw - 0.02 : tsw;
    /* pitch track */
    double tc[4000], fq[4000];
    double W = 2.0 / f_new;
    long m = pitch_track(r.aud, r.naud, fs, 0.8 * (f_old < f_new ? f_old : f_new), 1.2 * (f_old > f_new ? f_old : f_new), W, tsw - 0.05, tsw + 0.6, 0.0005, tc, fq, 4000);
    double fin[64];
    int nf = 0;
    for (long k = 0; k < m && nf < 64; k++) if (tc[k] > tsw + 0.45 && tc[k] < tsw + 0.55 && fq[k] > 0) fin[nf++] = fq[k];
    for (int a = 0; a < nf; a++) for (int b = a + 1; b < nf; b++) if (fin[b] < fin[a]) { double q = fin[a]; fin[a] = fin[b]; fin[b] = q; }
    double f_fin = nf ? fin[nf / 2] : f_new;
    double t_p = -1.0, dipc = 0.0;
    long need = (long)ceil(5.0 / f_fin / 0.0005);
    long run = 0;
    for (long k = 0; k < m; k++) {
        double c = tc[k] >= t_ev && fq[k] > 0 ? cents(fq[k], f_fin) : 1e9;
        if (tc[k] >= t_ev && tc[k] < t_ev + 0.2 && fq[k] > 0 && c < dipc) dipc = c;
        if (tc[k] < t_ev) continue;
        run = fabs(c) < 10.0 ? run + 1 : 0;
        if (t_p < 0 && run >= need) t_p = tc[k - need + 1] - t_ev;
    }
    double rep = audio_periodic_time(r.aud, r.naud, fs, f_fin, t_ev, 0.95, 10);
    double lev0 = rms_at(r.aud, r.naud, fs, tsw - 0.1, 0.05), lmin = lev0;
    for (double t = t_ev; t < t_ev + 0.3; t += 0.005) { double v = rms_at(r.aud, r.naud, fs, t, 0.005); if (v < lmin) lmin = v; }
    printf("%s %s ramp %.4f K %.3g R %.3g width %.4f seed %llu pre_ok %d f_fin %.2f (%+.1f c re nominal) t_pitch %.4f t_pitch_ramp %.4f t_reper %.4f dip_c %.1f lev_dip_db %.1f\n", scen,
           sp->name, ramp, ic.finger.K / 2e7, ic.finger.R / 200.0, ic.finger.width, (unsigned long long)seed, pre_ok, f_fin, cents(f_fin, f_new),
           t_p, t_p >= 0 ? t_p - (t_end_ev - t_ev) : -1.0, rep >= 0 ? rep - t_ev : -1.0, dipc, 20.0 * log10(lmin / lev0 + 1e-12));
    if (trace) {
        FILE *f = fopen(trace, "w");
        fprintf(f, "# t_s pitch_hz\n");
        for (long k = 0; k < m; k++) fprintf(f, "%.5f %.3f\n", tc[k] - tsw, fq[k]);
        fclose(f);
    }
    rec_free(&r);
    return 0;
}
