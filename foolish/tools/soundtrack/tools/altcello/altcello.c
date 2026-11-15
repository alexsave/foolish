/* altcello command line.
 *   altcello probe STRING BETA VB FB SECONDS OUTPREFIX [--fs 96000] [--finger XF] [--pluck]
 *       [--noise STD] [--body off|on] [--strings bowed|all] [--torsion on|off] [--wav OUT.wav]
 *       [--hair KH] [--eta0 X] [--eta-exp X] [--fc HZ] [--bridge-inertia-scale X]
 * Diagnosis knobs (stage 5): [--noise-level via --noise] [--fb-env t:v,t:v,...] [--vb-env t:v,...] (replace the ramp-in of F_B / v_B by
 * breakpoints, seconds:value, linear between) [--lead-mass-scale X] [--vib DEPTH_C RATE_HZ] (finger vibrato from 0.3 s, needs --finger)
 * [--bridge-R-scale X] [--stat-mass-scale X] [--q2k STRING Q] [--mode NAME HZ] (E0-E3 diagnosis knobs)
 * [--walk SD BW_HZ] (slow random walks of F_B and v_B, relative sd and bandwidth).
 * The last five are the physical knobs of stage 2 iteration 3: bow-hair stiffness N/m (default off), body loss factor at 500 Hz
 * and its frequency exponent, plate coincidence frequency (a fitted value of radiate_plate_fits), bridge inertia times 2.1e-6 kg m^2.
 * Writes OUTPREFIX.f32 (interleaved float32 LE, channels in OUTPREFIX.txt) and
 * OUTPREFIX_snap.f32 (u displacement field every 4th sample for the last 2
 * periods, header OUTPREFIX_snap.txt). Prints the real-time factor. */
#include "sim.h"
#include "wav.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    long total, first;
    int stride, frames;
    float *buf;
    int npts;
} Snap;

static void snap_cb(const Sim *S, long i, void *user)
{
    Snap *sn = user;
    if (i >= sn->first && ((i - sn->first) % sn->stride) == 0) {
        float *dst = sn->buf + (size_t)sn->frames * sn->npts;
        for (int l = 0; l < sn->npts; l++) dst[l] = (float)S->s->y1[0][l];
        sn->frames++;
    }
}

static double cpu_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

/* "t:v,t:v,..." (seconds : value) into an envelope */
static void parse_env(Env *e, const char *s)
{
    e->n = 0;
    while (*s) {
        char *end;
        double t = strtod(s, &end);
        if (*end != ':') break;
        double v = strtod(end + 1, &end);
        env_add(e, t, v);
        s = *end == ',' ? end + 1 : end;
    }
}

static int probe(int argc, char **argv)
{
    if (argc < 8) {
        fprintf(stderr, "usage: altcello probe STRING BETA VB FB SECONDS OUTPREFIX [--fs 96000] [--finger XF] [--pluck] [--noise STD] [--body off|on] [--strings bowed|all] [--torsion on|off] [--wav OUT.wav] [--hair KH] [--eta0 X] [--eta-exp X] [--fc HZ] [--bridge-inertia-scale X]\n");
        return 2;
    }
    Instrument ic = *instrument_cello(); /* a private copy: the string loss options below edit it */
    const StringSpec *sp = instrument_string(&ic, argv[2]);
    if (!sp) {
        fprintf(stderr, "unknown string %s\n", argv[2]);
        return 2;
    }
    double beta = atof(argv[3]), vB = atof(argv[4]), FB = atof(argv[5]), secs = atof(argv[6]);
    const char *prefix = argv[7];
    double fs = 96000.0, xF = -1.0;
    int pluck = 0;
    double noise = 0.03;
    int body = 0, all = 0, torsion = 1;
    const char *wavpath = NULL;
    double hair = 0.0;
    BodyOpts bo = body_opts_default(&ic);
    const char *fbenv = NULL, *vbenv = NULL;
    double vibc = 0.0, vibr = 0.0, walk_sd = 0.0, walk_bw = 0.0;
    for (int i = 8; i < argc; i++) {
        if (!strcmp(argv[i], "--fs") && i + 1 < argc) fs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--finger") && i + 1 < argc) xF = atof(argv[++i]);
        else if (!strcmp(argv[i], "--pluck")) pluck = 1;
        else if (!strcmp(argv[i], "--noise") && i + 1 < argc) noise = atof(argv[++i]);
        else if (!strcmp(argv[i], "--body") && i + 1 < argc) body = !strcmp(argv[++i], "on");
        else if (!strcmp(argv[i], "--strings") && i + 1 < argc) all = !strcmp(argv[++i], "all");
        else if (!strcmp(argv[i], "--torsion") && i + 1 < argc) torsion = !strcmp(argv[++i], "on");
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wavpath = argv[++i];
        else if (!strcmp(argv[i], "--hair") && i + 1 < argc) hair = atof(argv[++i]);
        else if (!strcmp(argv[i], "--eta0") && i + 1 < argc) bo.eta0 = atof(argv[++i]);
        else if (!strcmp(argv[i], "--eta-exp") && i + 1 < argc) bo.eta_exp = atof(argv[++i]);
        else if (!strcmp(argv[i], "--fc") && i + 1 < argc) bo.fc = atof(argv[++i]);
        else if (!strcmp(argv[i], "--fb-env") && i + 1 < argc) fbenv = argv[++i];
        else if (!strcmp(argv[i], "--vb-env") && i + 1 < argc) vbenv = argv[++i];
        else if (!strcmp(argv[i], "--lead-mass-scale") && i + 1 < argc) bo.lead_mass_scale = atof(argv[++i]);
        else if (!strcmp(argv[i], "--vib") && i + 2 < argc) { vibc = atof(argv[i + 1]); vibr = atof(argv[i + 2]); i += 2; }
        else if (!strcmp(argv[i], "--walk") && i + 2 < argc) { walk_sd = atof(argv[i + 1]); walk_bw = atof(argv[i + 2]); i += 2; }
        else if (!strcmp(argv[i], "--bridge-R-scale") && i + 1 < argc) bo.bridge_R *= atof(argv[++i]);
        else if (!strcmp(argv[i], "--stat-mass-scale") && i + 1 < argc) bo.stat_mass_scale = atof(argv[++i]);
        else if (!strcmp(argv[i], "--q2k") && i + 2 < argc) { /* STRING Q: sigma1 so that a partial at 2 kHz has quality factor Q */
            int si = instrument_string_index(&ic, argv[i + 1]);
            if (si < 0) { fprintf(stderr, "unknown string %s\n", argv[i + 1]); return 2; }
            StringSpec *q = &ic.str[si];
            double c = 2.0 * q->L * q->f0, w = 2.0 * M_PI * 2000.0, rate = w / (2.0 * atof(argv[i + 2]));
            q->sigma1 = (rate - q->sigma0) / ((w / c) * (w / c));
            i += 2;
        }
        else if (!strcmp(argv[i], "--mode") && i + 2 < argc) { /* NAME FREQ: move a named table mode */
            int found = 0;
            for (int m = 0; m < ic.body.nlead; m++)
                if (!strcmp(ic.body.lead[m].name, argv[i + 1])) { ic.body.lead[m].f = atof(argv[i + 2]); found = 1; }
            if (!found) { fprintf(stderr, "unknown mode %s\n", argv[i + 1]); return 2; }
            i += 2;
        }
        else if (!strcmp(argv[i], "--bridge-inertia-scale") && i + 1 < argc) bo.bridge_inertia *= atof(argv[++i]);
    }
    Sim S;
    if (body || all) {
        SimOpts so = {body, all, 0, &bo};
        sim_init_instrument(&S, &ic, fs, &so, sp->name);
    } else sim_init(&S, &ic, sp, fs);
    if (hair > 0.0) hair_init(&S.hair, hair, S.k);
    S.bow_noise_level = noise;
    S.torsion_on = torsion;
    sim_set_bow(&S, beta, vB, FB, 0.03);
    if (fbenv) parse_env(&S.FB, fbenv);
    if (vbenv) parse_env(&S.vB, vbenv);
    if (vibc > 0.0) { S.vib_depth_c = vibc; S.vib_rate = vibr; S.vib_t0 = 0.3; }
    if (walk_sd > 0.0) sim_set_walk(&S, walk_sd, walk_bw, secs);
    if (pluck) {
        S.bow_on = 0;
    }
    if (xF >= 0.0) {
        env_const(&S.xF, xF);
        env_const(&S.aF, 1.0);
    }
    sim_start(&S);
    if (pluck) sim_pluck(&S, 0.1 * sp->L, 0.02 * sp->L, 1e-3);

    Snap sn = {0};
    sn.stride = 4;
    sn.npts = S.s->N + 1;
    long total = (long)floor(secs * fs + 0.5);
    long span = (long)floor(2.0 * fs / sp->f0 + 0.5);
    sn.first = total > span ? total - span : 0;
    sn.buf = malloc(sizeof(float) * (size_t)sn.npts * (size_t)(span / sn.stride + 2));

    double c0 = cpu_seconds();
    Trace *tr = sim_run(&S, secs, snap_cb, &sn);
    double c1 = cpu_seconds();

    char path[1024];
    snprintf(path, sizeof path, "%s.f32", prefix);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    for (long i = 0; i < tr->n; i++)
        for (int c = 0; c < SIM_NCH; c++) {
            float v = (float)tr->ch[c][i];
            fwrite(&v, sizeof v, 1, f);
        }
    fclose(f);
    snprintf(path, sizeof path, "%s.txt", prefix);
    f = fopen(path, "w");
    fprintf(f, "fs=%.0f channels=%d names=", fs, SIM_NCH);
    for (int c = 0; c < SIM_NCH; c++) fprintf(f, "%s%s", c ? "," : "", sim_channel_names[c]);
    fprintf(f, " string=%s beta=%g vb=%g fb=%g N=%d h=%.6g\n", sp->name, beta, vB, FB, S.s->N, S.s->h);
    fclose(f);
    snprintf(path, sizeof path, "%s_snap.f32", prefix);
    f = fopen(path, "wb");
    fwrite(sn.buf, sizeof(float), (size_t)sn.frames * sn.npts, f);
    fclose(f);
    snprintf(path, sizeof path, "%s_snap.txt", prefix);
    f = fopen(path, "w");
    fprintf(f, "fs=%.0f points=%d stride=%d frames=%d h=%.9g L=%g\n", fs, sn.npts, sn.stride, sn.frames, S.s->h, sp->L);
    fclose(f);

    if (wavpath) {
        if (!body) {
            fprintf(stderr, "--wav needs --body on (it writes the body pressure)\n");
            return 2;
        }
        int fac = (int)(fs / 48000.0 + 0.5);
        float *y;
        long m = wav_decimate(tr->ch[7], tr->n, fac, &y);
        float pk = 0;
        for (long i = 0; i < m; i++)
            if (fabsf(y[i]) > pk) pk = fabsf(y[i]);
        float g = pk > 0 ? (float)pow(10.0, -1.0 / 20.0) / pk : 1.0f;
        for (long i = 0; i < m; i++) y[i] *= g;
        if (wav_write_f32(wavpath, y, m, (int)(fs / fac))) {
            perror(wavpath);
            return 1;
        }
        printf("wrote %s: %ld samples at %d Hz, body pressure normalised to -1 dBFS peak\n", wavpath, m, (int)(fs / fac));
        free(y);
    }
    double cpu = c1 - c0;
    printf("string=%s fs=%.0f N=%d Nt=%d h=%.4g mm B=%.3e  simulated %.3f s in %.3f s cpu  real-time factor %.1f\n",
           sp->name, fs, S.s->N, S.s->tor->N, S.s->h * 1e3, string_B(sp), secs, cpu, cpu > 0 ? secs / cpu : 0.0);
    trace_free(tr);
    free(sn.buf);
    sim_free(&S);
    return 0;
}

/* altcello admittance OUTPREFIX: the body's bridge admittance from the mode table
 * (exact modal sum), OUTPREFIX_adm.txt columns: f Hz, |Y_uu| dB re 1 s/kg, |Y_ww| dB re 1 s/kg;
 * OUTPREFIX_modes.txt: name f Q m cu cw */
static int admittance(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: altcello admittance OUTPREFIX\n");
        return 2;
    }
    BodyModes *bm = body_modes_build(instrument_cello());
    char path[1024];
    snprintf(path, sizeof path, "%s_adm.txt", argv[2]);
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return 1; }
    fprintf(f, "# f_Hz Yuu_dB Yww_dB (re 1 s/kg)\n");
    for (int i = 0; i <= 600; i++) {
        double fr = 50.0 * pow(200.0, i / 600.0), w = 2.0 * M_PI * fr;
        double ur = 0, ui = 0, wr = 0, wi = 0;
        for (int k = 0; k < bm->n; k++) {
            double wk = 2.0 * M_PI * bm->f[k];
            /* Y = j w / (m (wk^2 - w^2 + j w wk / Q)) */
            double dr = wk * wk - w * w, di = w * wk / bm->Q[k], den = bm->m[k] * (dr * dr + di * di);
            double yr = (w * di) / den, yi = (w * dr) / den;
            ur += bm->cu[k] * bm->cu[k] * yr; ui += bm->cu[k] * bm->cu[k] * yi;
            wr += bm->cw[k] * bm->cw[k] * yr; wi += bm->cw[k] * bm->cw[k] * yi;
        }
        fprintf(f, "%.4f %.3f %.3f\n", fr, 20.0 * log10(hypot(ur, ui)), 20.0 * log10(hypot(wr, wi)));
    }
    fclose(f);
    snprintf(path, sizeof path, "%s_modes.txt", argv[2]);
    f = fopen(path, "w");
    for (int k = 0; k < bm->n; k++) fprintf(f, "%s %.3f %.2f %.4f %.3f %.3f\n", body_mode_name(instrument_cello(), k), bm->f[k], bm->Q[k], bm->m[k], bm->cu[k], bm->cw[k]);
    fclose(f);
    body_modes_free(bm);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "admittance")) return admittance(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "probe")) return probe(argc, argv);
    fprintf(stderr, "usage: altcello probe ...\n");
    return 2;
}
