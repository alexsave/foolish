#include "sim.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

const char *const sim_channel_names[SIM_NCH] = {"bridge_u", "bridge_w", "vbow_u", "vrel", "fric"};

void sim_init(Sim *S, const StringSpec *sp, double fs)
{
    memset(S, 0, sizeof *S);
    S->s = string_new(sp, fs);
    S->fwbuf = calloc((size_t)S->s->N + 1, sizeof(double));
    S->fs = fs;
    S->k = 1.0 / fs;
    finger_init(&S->finger);
    S->bow_width = 0.010;
    S->bow_noise_level = 0.03;
    S->bow_on = 1;
    env_const(&S->vB, 0.0);
    env_const(&S->FB, 0.0);
    env_const(&S->beta, 0.1);
    env_const(&S->xF, 0.0);
    env_const(&S->aF, 0.0);
}

void sim_free(Sim *S)
{
    string_free(S->s);
    free(S->fwbuf);
    S->s = NULL;
}

void sim_start(Sim *S)
{
    rng_seed(&S->rng, ALTCELLO_SEED);
    bow_noise_init(&S->noise, S->bow_noise_level, S->fs);
    S->n = 0;
    S->vrel_prev = 0.0;
    S->stuck = 0;
}

void sim_set_bow(Sim *S, double beta, double vB, double FB, double ramp)
{
    env_const(&S->beta, beta);
    S->vB.n = 0;
    S->FB.n = 0;
    env_add(&S->vB, 0.0, 0.0);
    env_add(&S->vB, ramp, vB);
    env_add(&S->FB, 0.0, 0.0);
    env_add(&S->FB, ramp, FB);
}

void sim_pluck(Sim *S, double x0, double halfwidth, double amp)
{
    String *s = S->s;
    for (int l = 1; l < s->N; l++) {
        double d = (l * s->h - x0) / halfwidth;
        double v = fabs(d) < 1.0 ? 0.5 * amp * (1.0 + cos(M_PI * d)) : 0.0;
        s->y1[0][l] = s->y0[0][l] = v;
    }
    string_ghosts(s, 0, s->y1[0]);
    string_ghosts(s, 0, s->y0[0]);
}

void sim_step(Sim *S)
{
    String *s = S->s;
    double t = S->n * S->k;
    double k = S->k;
    double xF = env_eval(&S->xF, t), aF = env_eval(&S->aF, t);
    finger_apply(&S->finger, s, xF, aF);

    double nz = bow_noise_next(&S->noise, &S->rng, S->n, S->fs); /* always drawn on schedule */
    double vB = 0, F = 0, fric = 0, vrel = 0, vbow = 0;
    int lo = 1, hi = 0;
    double J[BOW_MAXPTS];
    if (S->bow_on) {
        vB = env_eval(&S->vB, t);
        F = env_eval(&S->FB, t);
        double beta = env_eval(&S->beta, t);
        double gate = fabs(S->vrel_prev) > 1e-4 ? 1.0 : 0.0;
        F *= 1.0 + nz * gate;
        if (F < 0.0) F = 0.0;
        bow_weights(s, s->sp.L * (1.0 - beta), S->bow_width, &lo, &hi, J);
    }
    /* w polarisation: static downward load through J (no friction) */
    string_step_free(s, 0, NULL);
    if (S->bow_on) {
        double *buf = S->fwbuf;
        for (int l = lo; l <= hi; l++) buf[l] = -F * J[l - lo];
        string_step_free(s, 1, buf);
        for (int l = lo; l <= hi; l++) buf[l] = 0.0;

        double q = 0.0, G = 0.0;
        const double *u0 = s->y0[0];
        double *u2 = s->y2[0];
        for (int l = lo; l <= hi; l++) {
            q += J[l - lo] * s->h * (u2[l] - u0[l]);
            G += s->h * (s->kk * s->invD[l]) * J[l - lo] * J[l - lo];
        }
        q = q / (2.0 * k) - vB;
        G = F * G / (2.0 * k);
        double Phi;
        int stuck;
        vrel = bow_solve(q, G, S->vrel_prev, S->stuck, &Phi, &stuck);
        for (int l = lo; l <= hi; l++) u2[l] -= s->kk * s->invD[l] * J[l - lo] * F * Phi;
        fric = F * Phi;
        vbow = vrel + vB;
        S->vrel_prev = vrel;
        S->stuck = stuck;
    } else {
        string_step_free(s, 1, NULL);
    }
    S->out[0] = string_bridge_force(s, s->y2[0]);
    S->out[1] = string_bridge_force(s, s->y2[1]);
    S->out[2] = vbow;
    S->out[3] = vrel;
    S->out[4] = fric;
    string_commit(s);
    S->n++;
}

Trace *sim_run(Sim *S, double seconds, SimCb cb, void *user)
{
    long n = (long)floor(seconds * S->fs + 0.5);
    Trace *tr = calloc(1, sizeof *tr);
    tr->n = n;
    tr->fs = S->fs;
    for (int c = 0; c < SIM_NCH; c++) tr->ch[c] = calloc((size_t)n, sizeof(double));
    for (long i = 0; i < n; i++) {
        sim_step(S);
        for (int c = 0; c < SIM_NCH; c++) tr->ch[c][i] = S->out[c];
        if (cb) cb(S, i, user);
    }
    return tr;
}

void trace_free(Trace *t)
{
    if (!t) return;
    for (int c = 0; c < SIM_NCH; c++) free(t->ch[c]);
    free(t);
}
