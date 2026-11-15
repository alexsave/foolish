/* The ONE table of body modes. ASSUMED values: cello frequencies from
 * Rossing's ASA cello page and from violin modes scaled by 0.38; the Q values
 * and masses are assumptions, to be tuned against the measured target
 * spectrum later. */
#include "body_params.h"
#include "rng.h"
#include <math.h>
#include <stdlib.h>

typedef struct {
    const char *name;
    double f, Q, m, cu, cw, r;
} Lead;

static const Lead lead[] = {
    /* name   f     Q   m(kg) c_u  c_w  r */
    {"A0", 102.0, 25.0, 2.0, 0.3, 1.0, 1.0},
    {"CBR", 150.0, 30.0, 0.9, 0.8, 0.6, 0.7},
    {"B1-", 178.0, 35.0, 0.35, 1.0, 0.5, 0.7}, /* main wolf candidate, between E3 and F#3 */
    {"B1+", 205.0, 35.0, 0.45, 1.0, 0.5, 0.7},
    {"m260", 260.0, 40.0, 0.6, 0.9, 0.5, 0.7},
    {"m300", 300.0, 40.0, 0.6, 0.9, 0.5, 0.7},
    {"m345", 345.0, 40.0, 0.6, 0.9, 0.5, 0.7},
    {"m395", 395.0, 40.0, 0.6, 0.9, 0.5, 0.7},
    {"m440", 440.0, 40.0, 0.6, 0.9, 0.5, 0.7},
};
#define NLEAD ((int)(sizeof lead / sizeof lead[0]))

#define STAT_F0 480.0
#define STAT_F1 12000.0
#define STAT_GAP_MEAN 18.0 /* Hz */
#define STAT_M500 0.5      /* kg at 500 Hz */
#define STAT_M_SD_DB 6.0
#define BH_F 2000.0 /* bridge hill: single dof rocking resonance */
#define BH_Q 3.0

const char *body_mode_name(int i) { return i < NLEAD ? lead[i].name : "stat"; }

static double stat_Q(double f)
{
    if (f <= 2000.0) return 50.0;
    if (f >= 10000.0) return 30.0;
    return 50.0 - 20.0 * log(f / 2000.0) / log(5.0);
}

/* |H_bh|^2, 1 at low frequency: the mass is divided by it */
static double bh_gain2(double f)
{
    double x = f / BH_F;
    double re = 1.0 - x * x, im = x / BH_Q;
    return 1.0 / (re * re + im * im);
}

BodyModes *body_modes_build(void)
{
    Rng rng;
    rng_seed(&rng, ALTCELLO_BODY_SEED);
    int cap = NLEAD + (int)((STAT_F1 - STAT_F0) / 4.0) + 16;
    BodyModes *b = calloc(1, sizeof *b);
    b->f = malloc(sizeof(double) * (size_t)cap);
    b->Q = malloc(sizeof(double) * (size_t)cap);
    b->m = malloc(sizeof(double) * (size_t)cap);
    b->cu = malloc(sizeof(double) * (size_t)cap);
    b->cw = malloc(sizeof(double) * (size_t)cap);
    b->r = malloc(sizeof(double) * (size_t)cap);
    int n = 0;
    for (; n < NLEAD; n++) {
        b->f[n] = lead[n].f;
        b->Q[n] = lead[n].Q;
        b->m[n] = lead[n].m;
        b->cu[n] = lead[n].cu;
        b->cw[n] = lead[n].cw;
        b->r[n] = lead[n].r;
    }
    b->nlead = NLEAD;
    double f = STAT_F0;
    while (f <= STAT_F1 && n < cap) {
        /* draws in Hz and kg, in this order per mode: mass, c_u, c_w, then the next gap */
        double mean_m = STAT_M500 * sqrt(f / 500.0);
        double m = mean_m * pow(10.0, STAT_M_SD_DB * rng_gauss(&rng) / 20.0);
        m /= bh_gain2(f);
        b->f[n] = f;
        b->Q[n] = stat_Q(f);
        b->m[n] = m;
        b->cu[n] = 0.3 + 0.7 * rng_uniform(&rng);
        b->cw[n] = 0.3 + 0.7 * rng_uniform(&rng);
        b->r[n] = 1.0;
        n++;
        double u = rng_uniform(&rng);
        if (u < 1e-12) u = 1e-12;
        f += -STAT_GAP_MEAN * log(u);
    }
    b->n = n;
    return b;
}

void body_modes_free(BodyModes *b)
{
    if (!b) return;
    free(b->f); free(b->Q); free(b->m); free(b->cu); free(b->cw); free(b->r);
    free(b);
}
