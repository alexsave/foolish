/* The body mode set built from an Instrument (instrument.h): the named table modes, then the statistical modes drawn once
 * from ALTCELLO_BODY_SEED. The constants live in the instrument file, not here. */
#include "body_modes.h"
#include "rng.h"
#include <math.h>
#include <stdlib.h>

/* the bridge stiffness K = I (2 pi f0)^2 of the Woodhouse bridge model, from the instrument's hill frequency */
static double bridge_K(const BodySpec *b)
{
    return b->bridge_inertia * (2.0 * M_PI * b->bridge_f0) * (2.0 * M_PI * b->bridge_f0);
}

const char *body_mode_name(const Instrument *I, int i) { return i < I->body.nlead ? I->body.lead[i].name : "stat"; }

BodyOpts body_opts_default(const Instrument *I)
{
    BodyOpts o = {I->body.eta0, I->body.eta_exp, I->body.bridge_inertia, I->radiation.fc, I->body.bridge_R, 1.0, 1.0};
    return o;
}

double body_eta(const BodyOpts *o, double f) { return o->eta0 * pow(f / 500.0, o->eta_exp); }

double body_bridge_gain(const Instrument *I, double f, double inertia)
{
    return body_bridge_gain_R(I, f, inertia, I->body.bridge_R);
}

double body_bridge_gain_R(const Instrument *I, double f, double inertia, double R)
{
    const BodySpec *b = &I->body;
    double K = bridge_K(b);
    double w = 2.0 * M_PI * f;
    double nr = K * R, ni = w;
    double dr = R * (K - inertia * w * w), di = R * w * K * inertia * R;
    return sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

double body_bridge_hill_freq(const Instrument *I, double inertia)
{
    double best = 0.0, bf = 0.0;
    for (double f = 200.0; f < 12000.0; f *= 1.0005) {
        double g = body_bridge_gain(I, f, inertia);
        if (g > best) { best = g; bf = f; }
    }
    return bf;
}

BodyModes *body_modes_build(const Instrument *I)
{
    BodyOpts o = body_opts_default(I);
    return body_modes_build_opts(I, &o);
}

BodyModes *body_modes_build_opts(const Instrument *I, const BodyOpts *o)
{
    const BodySpec *sp = &I->body;
    Rng rng;
    rng_seed(&rng, ALTCELLO_BODY_SEED);
    int cap = sp->nlead + (int)((sp->stat_f1 - sp->stat_f0) / 4.0) + 16;
    BodyModes *b = calloc(1, sizeof *b);
    b->f = malloc(sizeof(double) * (size_t)cap);
    b->Q = malloc(sizeof(double) * (size_t)cap);
    b->m = malloc(sizeof(double) * (size_t)cap);
    b->cu = malloc(sizeof(double) * (size_t)cap);
    b->cw = malloc(sizeof(double) * (size_t)cap);
    b->r = malloc(sizeof(double) * (size_t)cap);
    int n = 0;
    for (; n < sp->nlead; n++) {
        b->f[n] = sp->lead[n].f;
        b->Q[n] = sp->lead[n].Q;
        b->m[n] = sp->lead[n].m * o->lead_mass_scale;
        b->cu[n] = sp->lead[n].cu;
        b->cw[n] = sp->lead[n].cw;
        b->r[n] = n < sp->ncompact ? sp->compact_r[n] : 0.7; /* compact monopole strengths; plate modes get signs below */
    }
    b->nlead = sp->nlead;
    b->ncompact = sp->ncompact;
    double f = sp->stat_f0;
    while (f <= sp->stat_f1 && n < cap) {
        /* draws in Hz and kg, in this order per mode: mass, c_u, c_w, then the next gap */
        double mean_m = sp->stat_m500 * sqrt(f / 500.0);
        double m = mean_m * pow(10.0, sp->stat_m_sd_db * rng_gauss(&rng) / 20.0);
        m *= o->stat_mass_scale;
        m /= body_bridge_gain_R(I, f, o->bridge_inertia, o->bridge_R);
        b->f[n] = f;
        b->Q[n] = 1.0 / body_eta(o, f);
        b->m[n] = m;
        b->cu[n] = 0.3 + 0.7 * rng_uniform(&rng);
        b->cw[n] = 0.3 + 0.7 * rng_uniform(&rng);
        n++;
        double u = rng_uniform(&rng);
        if (u < 1e-12) u = 1e-12;
        f += -sp->stat_gap_mean * log(u);
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
