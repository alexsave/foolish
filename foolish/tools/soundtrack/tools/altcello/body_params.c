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
    double f, Q, m, cu, cw;
} Lead;

static const Lead lead[] = {
    /* name   f     Q   m(kg) c_u  c_w */
    {"A0", 102.0, 25.0, 2.0, 0.3, 1.0},
    {"CBR", 150.0, 30.0, 0.9, 0.8, 0.6},
    {"B1-", 178.0, 35.0, 0.35, 1.0, 0.5}, /* main wolf candidate, between E3 and F#3 */
    {"B1+", 205.0, 35.0, 0.45, 1.0, 0.5},
    {"m260", 260.0, 40.0, 0.6, 0.9, 0.5},
    {"m300", 300.0, 40.0, 0.6, 0.9, 0.5},
    {"m345", 345.0, 40.0, 0.6, 0.9, 0.5},
    {"m395", 395.0, 40.0, 0.6, 0.9, 0.5},
    {"m440", 440.0, 40.0, 0.6, 0.9, 0.5},
};
#define NLEAD ((int)(sizeof lead / sizeof lead[0]))

/* statistical modes: loss factor eta(f) = eta0 (f / 500 Hz)^a replaces the stage 2 Q schedule (REFERENCES 7.1, DERIVED assumption) */
#define STAT_F0 480.0
#define STAT_F1 12000.0
#define STAT_GAP_MEAN 18.0 /* Hz */
#define STAT_M_SCALE 3.0   /* stage 2 iteration 2: statistical modes 3x heavier = about -10 dB of admittance, so the bridge hill
                              does not tower over the low modes (the table modes are unchanged) */
#define STAT_M500 (0.5 * STAT_M_SCALE) /* kg at 500 Hz */
#define STAT_M_SD_DB 6.0
/* Woodhouse bridge model, DERIVED cello values (REFERENCES 7.2): Re R_inf = 30 rad/(m s N), hill near 1.5 kHz with zeta_b = 0.3
 * from I = m a^2 = 2.1e-6 kg m^2 (zeta_b = R I Omega_b / 2 with Omega_b = sqrt(K / I)), so K = I (2 pi 1500)^2 = 187 N m/rad */
#define BH_R 30.0
#define BH_I0 2.1e-6
#define BH_F0 1500.0
#define BH_K (BH_I0 * (2.0 * M_PI * BH_F0) * (2.0 * M_PI * BH_F0))

const char *body_mode_name(int i) { return i < NLEAD ? lead[i].name : "stat"; }

BodyOpts body_opts_default(void)
{
    BodyOpts o = {0.012, 0.3, BH_I0, 3000.0};
    return o;
}

double body_eta(const BodyOpts *o, double f) { return o->eta0 * pow(f / 500.0, o->eta_exp); }

double body_bridge_gain(double f, double inertia)
{
    double w = 2.0 * M_PI * f;
    double nr = BH_K * BH_R, ni = w;
    double dr = BH_R * (BH_K - inertia * w * w), di = BH_R * w * BH_K * inertia * BH_R;
    return sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

double body_bridge_hill_freq(double inertia)
{
    double best = 0.0, bf = 0.0;
    for (double f = 200.0; f < 12000.0; f *= 1.0005) {
        double g = body_bridge_gain(f, inertia);
        if (g > best) { best = g; bf = f; }
    }
    return bf;
}

BodyModes *body_modes_build(void)
{
    BodyOpts o = body_opts_default();
    return body_modes_build_opts(&o);
}

BodyModes *body_modes_build_opts(const BodyOpts *o)
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
        b->r[n] = n == 0 ? 1.0 : 0.7; /* compact monopole strengths (stage 2 values); plate modes get signs below */
    }
    b->nlead = NLEAD;
    b->ncompact = 4;
    double f = STAT_F0;
    while (f <= STAT_F1 && n < cap) {
        /* draws in Hz and kg, in this order per mode: mass, c_u, c_w, then the next gap */
        double mean_m = STAT_M500 * sqrt(f / 500.0);
        double m = mean_m * pow(10.0, STAT_M_SD_DB * rng_gauss(&rng) / 20.0);
        m /= body_bridge_gain(f, o->bridge_inertia);
        b->f[n] = f;
        b->Q[n] = 1.0 / body_eta(o, f);
        b->m[n] = m;
        b->cu[n] = 0.3 + 0.7 * rng_uniform(&rng);
        b->cw[n] = 0.3 + 0.7 * rng_uniform(&rng);
        n++;
        double u = rng_uniform(&rng);
        if (u < 1e-12) u = 1e-12;
        f += -STAT_GAP_MEAN * log(u);
    }
    b->n = n;
    /* the fixed random signs of the plate path: one draw per plate mode, in mode order (own sub-seed, so the mode table
       above is not disturbed) */
    Rng rs;
    rng_seed(&rs, ALTCELLO_RADIATE_SEED);
    for (int i = b->ncompact; i < n; i++) b->r[i] = (rng_next(&rs) >> 63) ? 1.0 : -1.0;
    return b;
}

void body_modes_free(BodyModes *b)
{
    if (!b) return;
    free(b->f); free(b->Q); free(b->m); free(b->cu); free(b->cw); free(b->r);
    free(b);
}
