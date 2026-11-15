#include "sim.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

const char *const sim_channel_names[SIM_NCH] = {"bridge_u", "bridge_w", "vbow_u", "vrel", "fric", "vbridge_u", "vbridge_w", "pbody"};
const char *const sim_string_names[SIM_MAXSTR] = {"C2", "G2", "D3", "A3"};

static void common_init(Sim *S, double fs)
{
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
    for (int i = 0; i < SIM_MAXSTR; i++) {
        env_const(&S->xFo[i], 0.0);
        env_const(&S->aFo[i], 0.0);
    }
}

void sim_init(Sim *S, const StringSpec *sp, double fs)
{
    memset(S, 0, sizeof *S);
    S->str[0] = string_new(sp, fs);
    S->nstr = 1;
    S->bowed = 0;
    S->s = S->str[0];
    S->fwbuf = calloc((size_t)S->s->N + 1, sizeof(double));
    common_init(S, fs);
}

void sim_init_instrument(Sim *S, double fs, const SimOpts *o, const char *bowed_name)
{
    memset(S, 0, sizeof *S);
    int maxN = 0;
    for (int i = 0; i < SIM_MAXSTR; i++) {
        if (!o->all_strings && strcmp(sim_string_names[i], bowed_name)) continue;
        StringSpec sp = *string_spec(sim_string_names[i]);
        if (o->lossless) sp.sigma0 = sp.sigma1 = 0.0;
        S->str[S->nstr] = string_new(&sp, fs);
        if (!strcmp(sim_string_names[i], bowed_name)) S->bowed = S->nstr;
        if (S->str[S->nstr]->N > maxN) maxN = S->str[S->nstr]->N;
        S->nstr++;
    }
    S->s = S->str[S->bowed];
    S->fwbuf = calloc((size_t)maxN + 1, sizeof(double));
    if (o->body_on) {
        BodyModes *bm = body_modes_build();
        S->body = body_new(bm, fs, o->lossless);
        body_modes_free(bm);
    }
    common_init(S, fs);
}

void sim_free(Sim *S)
{
    for (int i = 0; i < S->nstr; i++) string_free(S->str[i]);
    S->nstr = 0;
    S->s = NULL;
    body_free(S->body);
    S->body = NULL;
    free(S->fwbuf);
    S->fwbuf = NULL;
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

void sim_pluck_string(Sim *S, int idx, int pol, double x0, double halfwidth, double amp)
{
    String *s = S->str[idx];
    for (int l = 1; l < s->N; l++) {
        double d = (l * s->h - x0) / halfwidth;
        double v = fabs(d) < 1.0 ? 0.5 * amp * (1.0 + cos(M_PI * d)) : 0.0;
        s->y1[pol][l] = s->y0[pol][l] = v;
    }
    string_ghosts(s, pol, s->y1[pol]);
    string_ghosts(s, pol, s->y0[pol]);
}

void sim_pluck(Sim *S, double x0, double halfwidth, double amp)
{
    sim_pluck_string(S, S->bowed, 0, x0, halfwidth, amp);
}

void sim_step(Sim *S)
{
    double t = S->n * S->k;
    double k = S->k;
    String *s = S->s;

    /* bridge: the strings' end force drives the body (leapfrog of the whole
       instrument: both the body and the strings use the state at time n) */
    double zu = 0.0, zw = 0.0;
    if (S->body) {
        double Fu = 0.0, Fw = 0.0;
        for (int i = 0; i < S->nstr; i++) {
            Fu -= string_bridge_force(S->str[i], S->str[i]->y1[0]);
            Fw -= string_bridge_force(S->str[i], S->str[i]->y1[1]);
        }
        body_step(S->body, Fu, Fw);
        zu = S->body->zu;
        zw = S->body->zw;
    }

    for (int i = 0; i < S->nstr; i++) {
        const Env *ex = i == S->bowed ? &S->xF : &S->xFo[i];
        const Env *ea = i == S->bowed ? &S->aF : &S->aFo[i];
        finger_apply(&S->finger, S->str[i], env_eval(ex, t), env_eval(ea, t));
    }

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
    for (int i = 0; i < S->nstr; i++) {
        if (i == S->bowed) continue;
        string_step_free(S->str[i], 0, NULL);
        string_step_free(S->str[i], 1, NULL);
    }
    string_step_free(s, 0, NULL);
    if (S->bow_on) {
        /* w polarisation: static downward load through J (no friction) */
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
    for (int i = 0; i < S->nstr; i++) {
        S->str[i]->y2[0][S->str[i]->N] = zu;
        S->str[i]->y2[1][S->str[i]->N] = zw;
    }
    S->out[0] = string_bridge_force(s, s->y2[0]);
    S->out[1] = string_bridge_force(s, s->y2[1]);
    S->out[2] = vbow;
    S->out[3] = vrel;
    S->out[4] = fric;
    S->out[5] = S->body ? S->body->vu : 0.0;
    S->out[6] = S->body ? S->body->vw : 0.0;
    S->out[7] = S->body ? S->body->p : 0.0;
    for (int i = 0; i < S->nstr; i++) string_commit(S->str[i]);
    S->n++;
}

double sim_energy(const Sim *S)
{
    double E = 0.0;
    for (int i = 0; i < S->nstr; i++) {
        const String *s = S->str[i];
        for (int p = 0; p < 2; p++) E += s->rho * string_energy(s, s->y1[p], s->y0[p]);
    }
    if (S->body) E += body_energy(S->body);
    return E;
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
