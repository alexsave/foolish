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
 *   onset_tool pitch RAW.f32 FS FLO FHI WIN T0 T1 HOP
 *       the pitch track (analysis.h pitch_track) of a mono float32 file: lines t_s f_Hz
 *   onset_tool fing ...   (the finger-change experiments, legato.h)
 */
#include "analysis.h"
#include "knobs.h"
#include "oracle_msw.h"
#include "player.h"
#include "sim.h"
#include "wav.h"
#include "legato.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>



/* ---------------------------------------------------------------- the grid */

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
    double on = audio_onset_from(x, n, fs, from < 0.0 ? 0.0 : from); /* --from: look for the onset from T on (the landing click of a bow is not the note) */
    double tp = on >= 0 ? audio_periodic_time(x, n, fs, f0, on, 0.95, 10) : -1.0;
    printf("onset %.4f t_periodic %.4f\n", on, tp >= 0 ? tp - on : -1.0);
    return 0;
}

int fing_main(int argc, char **argv);
static int pitch_main(int argc, char **argv);

int main(int argc, char **argv)
{
    knobs_install();
    if (argc < 2) { fprintf(stderr, "usage: onset_tool wav|grid|fing ...\n"); return 2; }
    if (!strcmp(argv[1], "wav")) return wav_main(argc, argv);
    if (!strcmp(argv[1], "grid")) return grid_main(argc, argv);
    if (!strcmp(argv[1], "fing")) return fing_main(argc, argv);
    if (!strcmp(argv[1], "pitch")) return pitch_main(argc, argv);
    fprintf(stderr, "unknown mode %s\n", argv[1]);
    return 2;
}


int fing_main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: onset_tool fing slur|lift|bow STR [options]\n"); return 2; }
    LegatoArgs a;
    if (legato_default(&a, argv[2], argv[3])) { fprintf(stderr, "unknown string or scenario\n"); return 2; }
    const char *trace = NULL;
    int o0 = -1, o1 = -1;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--ramp") && i + 1 < argc) a.ramp = atof(argv[++i]);
        else if (!strcmp(argv[i], "--K") && i + 1 < argc) a.ic.finger.K *= atof(argv[++i]);
        else if (!strcmp(argv[i], "--R") && i + 1 < argc) a.ic.finger.R *= atof(argv[++i]);
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) a.ic.finger.width = atof(argv[++i]);
        else if (!strcmp(argv[i], "--dip") && i + 1 < argc) a.dip = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) a.seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--tsw") && i + 1 < argc) a.tsw = atof(argv[++i]);
        else if (!strcmp(argv[i], "--F") && i + 1 < argc) a.F = atof(argv[++i]);
        else if (!strcmp(argv[i], "--bw") && i + 1 < argc) a.bw = atof(argv[++i]);
        else if (!strcmp(argv[i], "--bdip") && i + 1 < argc) a.bdip = atof(argv[++i]);
        else if (!strcmp(argv[i], "--only")) a.only_bowed = 1;
        else if (!strcmp(argv[i], "--semis") && i + 2 < argc) { o0 = atoi(argv[i + 1]); o1 = atoi(argv[i + 2]); i += 2; }
        else if (!strcmp(argv[i], "--fdip") && i + 1 < argc) a.fdip = atof(argv[++i]);
        else if (!strcmp(argv[i], "--fw") && i + 1 < argc) a.fw = atof(argv[++i]);
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace = argv[++i];
    }
    if (o0 >= 0) { a.s0 = o0; a.s1 = o1; }
    LegatoOut o;
    if (legato_run(&a, &o)) return 1;
    printf("%s %s ramp %.4f K %.3g R %.3g width %.4f seed %llu pre_ok %d f_fin %.2f (%+.1f c re nominal) t_pitch %.4f t_pitch_ramp %.4f t_reper %.4f dip_c %.1f lev_dip_db %.1f\n",
           a.scen, a.ic.str[a.si].name, a.ramp, a.ic.finger.K / 2e7, a.ic.finger.R / 200.0, a.ic.finger.width, (unsigned long long)a.seed, o.pre_ok, o.f_fin,
           o.cents_nominal, o.t_pitch, o.t_pitch_ramp, o.t_reper, o.dip_c, o.lev_dip_db);
    if (trace) {
        FILE *f = fopen(trace, "w");
        fprintf(f, "# t_s pitch_hz\n");
        for (long k = 0; k < o.ntrack; k++) fprintf(f, "%.5f %.3f\n", o.tc[k] - a.tsw, o.fq[k]);
        fclose(f);
    }
    return 0;
}

static int pitch_main(int argc, char **argv)
{
    if (argc < 10) { fprintf(stderr, "usage: onset_tool pitch RAW.f32 FS FLO FHI WIN T0 T1 HOP\n"); return 2; }
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
    double fs = atof(argv[3]), win = atof(argv[6]), t0 = atof(argv[7]), t1 = atof(argv[8]), hop = atof(argv[9]);
    long kmax = (long)((t1 - t0) / hop) + 10;
    double *tc = malloc(sizeof(double) * (size_t)kmax), *fq = malloc(sizeof(double) * (size_t)kmax);
    long m = pitch_track(x, n, fs, atof(argv[4]), atof(argv[5]), win, t0, t1, hop, tc, fq, kmax);
    for (long k = 0; k < m; k++) printf("%.5f %.3f\n", tc[k], fq[k]);
    return 0;
}
