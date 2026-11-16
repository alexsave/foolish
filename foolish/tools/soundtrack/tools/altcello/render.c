#include "render.h"
#include "fe_body.h"
#include "pickup.h"
#include "room.h"
#include "sim.h"
#include "wav.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double cpu_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

void render_opts_default(RenderOpts *o)
{
    memset(o, 0, sizeof *o);
    o->style = STYLE_EXPERT;
    o->seed = 0;
    o->rt60 = 0.0;
    o->dist = 1.5;
    o->fs = 96000.0;
    o->damp = -1.0;
    o->damp_delay = -1.0;
    o->predamp = -1;
    o->rock = -1.0;
    o->dip = -1.0;
    o->swap_dip = -1.0;
    o->att = -1;
    o->att_hold = -1.0;
}

int render_notes(const Note *notes, int n, const RenderOpts *o, float **pcm48, long *n48, Played *played, double *rtf)
{
    const Instrument *I = instrument_cello();
    Player pl;
    player_init(&pl, I, o->style, o->seed);
    pl.vib_all_off = o->vib_off;
    if (o->damp >= 0.0) pl.damp_press = o->damp;
    if (o->damp_ease > 0.0) pl.damp_ease = o->damp_ease;
    if (o->damp_delay >= 0.0) pl.damp_delay = o->damp_delay;
    if (o->predamp >= 0) pl.predamp = o->predamp;
    if (o->fr > 0.0) pl.f_ramp = o->fr;
    if (o->land[0] > 0.0) { pl.land_lead = o->land[0]; pl.land_ramp = o->land[1]; }
    if (o->bc[1] > 0.0) { pl.bc_lo = o->bc[0]; pl.bc_hi = o->bc[1]; }
    pl.slur = o->slur;
    if (o->dip >= 0.0) pl.dip = o->dip;
    if (o->swap_dip >= 0.0) pl.swap_dip = o->swap_dip;
    if (o->rock >= 0.0) pl.rock = o->rock;
    if (o->acc[1] > 0.0) { pl.acc_lo = o->acc[0]; pl.acc_hi = o->acc[1]; }
    if (o->beta0 > 0.0) pl.beta_base = o->beta0;
    if (o->vb[1] > 0.0) { pl.vb_min = o->vb[0]; pl.vb_span = o->vb[1]; }
    if (o->frac[1] > 0.0) { pl.frac_min = o->frac[0]; pl.frac_span = o->frac[1]; }
    if (o->att >= 0) pl.att_on = o->att;
    if (o->att_acc > 0.0) pl.att_acc = o->att_acc;
    if (o->att_k > 0.0) pl.att_k = o->att_k;
    if (o->att_hold >= 0.0) pl.att_hold = o->att_hold;
    if (o->rel[1] > 0.0) { pl.rel_lo = o->rel[0]; pl.rel_hi = o->rel[1]; }
    if (o->att_v > 0.0) pl.att_v_frac = o->att_v;
    if (o->att_frac > 0.0) pl.att_frac = o->att_frac;
    if (o->sw[1] > 0.0) { pl.sw_lo = o->sw[0]; pl.sw_hi = o->sw[1]; }
    if (player_play(&pl, notes, n, played)) return 1;
    Sim S;
    SimOpts so = {1, o->solo ? 0 : 1, 0, NULL};
    sim_init_instrument(&S, I, o->fs, &so, "D3");
    S.play = &played->trk;
    sim_start(&S);
    long total = (long)floor(played->dur * o->fs + 0.5);
    double *pr = malloc(sizeof(double) * (size_t)total);
    /* contact pickup capture (output only): point velocities 0 bridge_u, 1 bridge_w, 2 top centre, 3 back centre (the last two need the FE body, fe=1|2) */
    enum { NPK = 4 };
    static const char *const pk_name[NPK] = {"bridge_u", "bridge_w", "top", "back"};
    double *pk[NPK] = {0}, *tcw = NULL, *bcw = NULL;
    int pk_nfe = 0;
    if (o->pickup) {
        for (int c = 0; c < 2; c++) pk[c] = malloc(sizeof(double) * (size_t)total);
        if (S.body && S.body->nfe > 0) {
            if (fe_point_weights(I, &tcw, &bcw, &pk_nfe) == 0 && pk_nfe == S.body->nfe) {
                pk[2] = malloc(sizeof(double) * (size_t)total);
                pk[3] = malloc(sizeof(double) * (size_t)total);
            } else fprintf(stderr, "pickup: the FE file has no top_centre_n / back_centre_n for the modes in use, top and back skipped\n");
        }
    }
    double c0 = cpu_seconds();
    for (long i = 0; i < total; i++) {
        sim_step(&S);
        pr[i] = S.out[7];
        if (pk[0]) { pk[0][i] = S.out[5]; pk[1][i] = S.out[6]; }
        if (pk[2]) {
            double a = 0.0, b = 0.0;
            for (int j = 0; j < pk_nfe; j++) { a += tcw[j] * S.body->tdq[j]; b += bcw[j] * S.body->tdq[j]; }
            pk[2][i] = a / (2.0 * S.body->k);
            pk[3][i] = b / (2.0 * S.body->k);
        }
        if (o->step_cb) o->step_cb(&S, i, o->step_user);
    }
    double c1 = cpu_seconds();
    if (rtf) *rtf = c1 > c0 ? played->dur / (c1 - c0) : 0.0;
    int fac = (int)(o->fs / 48000.0 + 0.5);
    for (int c = 0; c < NPK; c++) {
        if (!pk[c]) continue;
        double *a = malloc(sizeof(double) * (size_t)total);
        for (int kind = 0; kind < 4; kind++) { /* 0 velocity (P2), 1 acceleration + 16 Hz high-pass (P1, 10 MOhm), 2 the same at 160 Hz (1 MOhm), 3 acceleration, no high-pass */
            const double *x = pk[c];
            const char *kn = "vel";
            if (kind == 1) { pickup_accel_hp(pk[c], total, o->fs, 16.0, a); x = a; kn = "acc_fc16"; }
            if (kind == 2) { pickup_accel_hp(pk[c], total, o->fs, 160.0, a); x = a; kn = "acc_fc160"; }
            if (kind == 3) { pickup_accel_hp(pk[c], total, o->fs, 1e-3, a); x = a; kn = "acc_raw"; }
            float *y;
            long m = wav_decimate(x, total, fac, &y);
            char path[1024];
            snprintf(path, sizeof path, "%s_%s_%s.wav", o->pickup, pk_name[c], kn);
            if (wav_write_f32(path, y, m, 48000)) perror(path);
            free(y);
        }
        free(a);
        free(pk[c]);
    }
    free(tcw);
    free(bcw);
    *n48 = wav_decimate(pr, total, fac, pcm48);
    free(pr);
    sim_free(&S);
    return 0;
}

int render_cli(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: altcello render NOTES.txt OUT.wav [--style expert|novice] [--seed N] [--vib on|off] [--room off|RT60] [--dist M] [--trace PREFIX] [--fs HZ] [--damp PRESS] [--acc LO HI] [--vb MIN SPAN] [--frac MIN SPAN]\n");
        return 2;
    }
    RenderOpts o;
    render_opts_default(&o);
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--style") && i + 1 < argc) o.style = !strcmp(argv[++i], "novice") ? STYLE_NOVICE : STYLE_EXPERT;
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) o.seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--vib") && i + 1 < argc) o.vib_off = !strcmp(argv[++i], "off");
        else if (!strcmp(argv[i], "--room") && i + 1 < argc) { const char *v = argv[++i]; o.rt60 = !strcmp(v, "off") ? 0.0 : atof(v); }
        else if (!strcmp(argv[i], "--dist") && i + 1 < argc) o.dist = atof(argv[++i]);
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) o.trace = argv[++i];
        else if (!strcmp(argv[i], "--pickup") && i + 1 < argc) o.pickup = argv[++i];
        else if (!strcmp(argv[i], "--fs") && i + 1 < argc) o.fs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--damp") && i + 1 < argc) o.damp = atof(argv[++i]);
        else if (!strcmp(argv[i], "--ddelay") && i + 1 < argc) o.damp_delay = atof(argv[++i]);
        else if (!strcmp(argv[i], "--dease") && i + 1 < argc) o.damp_ease = atof(argv[++i]);
        else if (!strcmp(argv[i], "--framp") && i + 1 < argc) o.fr = atof(argv[++i]);
        else if (!strcmp(argv[i], "--land") && i + 2 < argc) { o.land[0] = atof(argv[++i]); o.land[1] = atof(argv[++i]); }
        else if (!strcmp(argv[i], "--predamp") && i + 1 < argc) o.predamp = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bc") && i + 2 < argc) { o.bc[0] = atof(argv[i + 1]); o.bc[1] = atof(argv[i + 2]); i += 2; }
        else if (!strcmp(argv[i], "--swapdip") && i + 1 < argc) o.swap_dip = atof(argv[++i]);
        else if (!strcmp(argv[i], "--slur") && i + 1 < argc) o.slur = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dip") && i + 1 < argc) o.dip = atof(argv[++i]);
        else if (!strcmp(argv[i], "--rock") && i + 1 < argc) o.rock = atof(argv[++i]);
        else if (!strcmp(argv[i], "--acc") && i + 2 < argc) { o.acc[0] = atof(argv[i + 1]); o.acc[1] = atof(argv[i + 2]); i += 2; }
        else if (!strcmp(argv[i], "--beta0") && i + 1 < argc) o.beta0 = atof(argv[++i]);
        else if (!strcmp(argv[i], "--vb") && i + 2 < argc) { o.vb[0] = atof(argv[i + 1]); o.vb[1] = atof(argv[i + 2]); i += 2; }
        else if (!strcmp(argv[i], "--solo")) o.solo = 1;
        else if (!strcmp(argv[i], "--att") && i + 1 < argc) o.att = !strcmp(argv[++i], "on");
        else if (!strcmp(argv[i], "--atta") && i + 1 < argc) o.att_acc = atof(argv[++i]);
        else if (!strcmp(argv[i], "--attk") && i + 1 < argc) o.att_k = atof(argv[++i]);
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc) o.att_hold = atof(argv[++i]);
        else if (!strcmp(argv[i], "--rel") && i + 2 < argc) { o.rel[0] = atof(argv[i + 1]); o.rel[1] = atof(argv[i + 2]); i += 2; }
        else if (!strcmp(argv[i], "--attv") && i + 1 < argc) o.att_v = atof(argv[++i]);
        else if (!strcmp(argv[i], "--attf") && i + 1 < argc) o.att_frac = atof(argv[++i]);
        else if (!strcmp(argv[i], "--sw") && i + 2 < argc) { o.sw[0] = atof(argv[i + 1]); o.sw[1] = atof(argv[i + 2]); i += 2; }
        else if (!strcmp(argv[i], "--frac") && i + 2 < argc) { o.frac[0] = atof(argv[i + 1]); o.frac[1] = atof(argv[i + 2]); i += 2; }
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    Note *nt;
    int n;
    if (notes_read(argv[2], &nt, &n) || n < 1) { fprintf(stderr, "cannot read notes from %s\n", argv[2]); return 1; }
    Played pl;
    float *y;
    long m;
    double rtf = 0.0;
    if (render_notes(nt, n, &o, &y, &m, &pl, &rtf)) { fprintf(stderr, "render failed\n"); return 1; }
    if (o.trace && played_write_trace(&pl, instrument_cello(), o.trace)) { perror(o.trace); return 1; }
    float *out = y;
    long nout = m;
    if (o.rt60 > 0.0) {
        RoomSpec rs;
        room_spec_default(&rs, o.dist);
        rs.rt60 = o.rt60;
        float *ir;
        long nir;
        if (room_ir_build(&rs, &ir, &nir)) { fprintf(stderr, "ir build failed\n"); return 1; }
        nout = room_apply(&rs, ir, nir, y, m, &out);
        free(ir);
        if (nout <= 0) { fprintf(stderr, "room failed\n"); return 1; }
    }
    float pk = 0;
    for (long i = 0; i < nout; i++) if (fabsf(out[i]) > pk) pk = fabsf(out[i]);
    float g = pk > 0 ? (float)pow(10.0, -1.0 / 20.0) / pk : 1.0f;
    for (long i = 0; i < nout; i++) out[i] *= g;
    if (wav_write_f32(argv[3], out, nout, 48000)) { perror(argv[3]); return 1; }
    printf("rendered %d notes (%s, seed %llu%s): %.2f s of sound, %ld samples at 48 kHz -> %s, real-time factor %.2f (audio s per cpu s, fs %.0f)\n", n,
           o.style == STYLE_EXPERT ? "expert" : "novice", (unsigned long long)o.seed, o.vib_off ? ", vibrato off" : "", (double)nout / 48000.0, nout,
           argv[3], rtf, o.fs);
    if (out != y) free(out);
    free(y);
    played_free(&pl);
    free(nt);
    return 0;
}
