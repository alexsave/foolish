/* altcello command line.
 *   altcello probe STRING BETA VB FB SECONDS OUTPREFIX [--fs 96000] [--finger XF] [--pluck]
 * Writes OUTPREFIX.f32 (interleaved float32 LE, channels in OUTPREFIX.txt) and
 * OUTPREFIX_snap.f32 (u displacement field every 4th sample for the last 2
 * periods, header OUTPREFIX_snap.txt). Prints the real-time factor. */
#include "sim.h"
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

static int probe(int argc, char **argv)
{
    if (argc < 8) {
        fprintf(stderr, "usage: altcello probe STRING BETA VB FB SECONDS OUTPREFIX [--fs 96000] [--finger XF] [--pluck] [--noise STD]\n");
        return 2;
    }
    const StringSpec *sp = string_spec(argv[2]);
    if (!sp) {
        fprintf(stderr, "unknown string %s\n", argv[2]);
        return 2;
    }
    double beta = atof(argv[3]), vB = atof(argv[4]), FB = atof(argv[5]), secs = atof(argv[6]);
    const char *prefix = argv[7];
    double fs = 96000.0, xF = -1.0;
    int pluck = 0;
    double noise = 0.03;
    for (int i = 8; i < argc; i++) {
        if (!strcmp(argv[i], "--fs") && i + 1 < argc) fs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--finger") && i + 1 < argc) xF = atof(argv[++i]);
        else if (!strcmp(argv[i], "--pluck")) pluck = 1;
        else if (!strcmp(argv[i], "--noise") && i + 1 < argc) noise = atof(argv[++i]);
    }
    Sim S;
    sim_init(&S, sp, fs);
    S.bow_noise_level = noise;
    sim_set_bow(&S, beta, vB, FB, 0.03);
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

    double cpu = c1 - c0;
    printf("string=%s fs=%.0f N=%d h=%.4g mm B=%.3e  simulated %.3f s in %.3f s cpu  real-time factor %.1f\n",
           sp->name, fs, S.s->N, S.s->h * 1e3, string_B(sp), secs, cpu, cpu > 0 ? secs / cpu : 0.0);
    trace_free(tr);
    free(sn.buf);
    sim_free(&S);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "probe")) return probe(argc, argv);
    fprintf(stderr, "usage: altcello probe ...\n");
    return 2;
}
