#include "sim.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

const char *const sim_channel_names[SIM_NCH] = {"bridge_u", "bridge_w", "vbow_u", "vrel", "fric", "vbridge_u", "vbridge_w", "pbody"};

static void common_init(Sim *S, const Instrument *I, double fs)
{
    S->inst = I;
    S->fs = fs;
    S->k = 1.0 / fs;
    finger_init(&S->finger, &I->finger);
    S->bow_width = I->bow.width;
    S->bow_noise_level = I->bow.noise_level;
    S->bow_on = 1;
    S->torsion_on = 1;
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

void sim_init(Sim *S, const Instrument *I, const StringSpec *sp, double fs)
{
    memset(S, 0, sizeof *S);
    S->str[0] = string_new(sp, fs);
    S->nstr = 1;
    S->bowed = 0;
    S->s = S->str[0];
    S->fwbuf = calloc((size_t)S->s->N + 1, sizeof(double));
    common_init(S, I, fs);
}

void sim_init_instrument(Sim *S, const Instrument *I, double fs, const SimOpts *o, const char *bowed_name)
{
    memset(S, 0, sizeof *S);
    int maxN = 0;
    for (int i = 0; i < I->nstr; i++) {
        if (!o->all_strings && strcmp(I->str[i].name, bowed_name)) continue;
        StringSpec sp = I->str[i];
        if (o->lossless) sp.sigma0 = sp.sigma1 = sp.zeta_t = 0.0;
        S->str[S->nstr] = string_new(&sp, fs);
        if (!strcmp(I->str[i].name, bowed_name)) S->bowed = S->nstr;
        if (S->str[S->nstr]->N > maxN) maxN = S->str[S->nstr]->N;
        S->nstr++;
    }
    S->s = S->str[S->bowed];
    S->fwbuf = calloc((size_t)maxN + 1, sizeof(double));
    if (o->body_on) {
        BodyOpts def = body_opts_default(I);
        const BodyOpts *bo = o->body_opts ? o->body_opts : &def;
        BodyModes *bm = body_modes_build_opts(I, bo);
        S->body = body_new(I, bm, fs, o->lossless, bo); /* NULL if bo->fc has no fitted plate filter (radiate_plate_fits) */
        if (!S->body) abort();
        body_modes_free(bm);
    }
    common_init(S, I, fs);
}

int sim_enable_shadow(Sim *S, const BodyOpts *opts)
{
    if (!S->body) return 1;
    BodyOpts def = body_opts_default(S->inst);
    const BodyOpts *bo = opts ? opts : &def;
    for (int i = 0; i < S->nstr; i++) {
        BodyModes *bm = body_modes_build_opts(S->inst, bo);
        S->shadow[i] = body_new(S->inst, bm, S->fs, 0, bo);
        body_modes_free(bm);
        if (!S->shadow[i]) return 1;
    }
    return 0;
}

void sim_free(Sim *S)
{
    for (int i = 0; i < S->nstr; i++) string_free(S->str[i]);
    S->nstr = 0;
    S->s = NULL;
    body_free(S->body);
    S->body = NULL;
    for (int i = 0; i < SIM_MAXSTR; i++) { body_free(S->shadow[i]); S->shadow[i] = NULL; }
    free(S->fwbuf);
    S->fwbuf = NULL;
    free(S->walk_F);
    free(S->walk_v);
    S->walk_F = S->walk_v = NULL;
}

double sim_vib_finger(double L, double xF, double cents)
{
    /* pitch f = f0 L / (L - xF): c cents about the stopped pitch moves the finger to L - (L - xF) 2^(-c/1200) */
    return L - (L - xF) * pow(2.0, -cents / 1200.0);
}

void sim_set_walk(Sim *S, double sd, double bw_hz, double seconds)
{
    long n = (long)(seconds * SIM_WALK_RATE) + 3;
    free(S->walk_F);
    free(S->walk_v);
    S->walk_F = calloc((size_t)n, sizeof(double));
    S->walk_v = calloc((size_t)n, sizeof(double));
    S->walk_n = n;
    S->walk_sd = sd;
    S->walk_bw = bw_hz;
    Rng r;
    rng_seed(&r, ALTCELLO_WALK_SEED);
    double a = exp(-2.0 * M_PI * bw_hz / SIM_WALK_RATE), b = sd * sqrt(1.0 - a * a), yF = 0.0, yv = 0.0;
    for (long i = 0; i < n; i++) { /* two draws per index, F then v */
        yF = a * yF + b * rng_gauss(&r);
        yv = a * yv + b * rng_gauss(&r);
        S->walk_F[i] = yF;
        S->walk_v[i] = yv;
    }
}

static double walk_eval(const double *tab, long n, double t)
{
    double x = t * SIM_WALK_RATE;
    long i = (long)x;
    if (i < 0) return tab[0];
    if (i >= n - 1) return tab[n - 1];
    double u = x - (double)i;
    return tab[i] + u * (tab[i + 1] - tab[i]);
}

void sim_start(Sim *S)
{
    rng_seed(&S->rng, ALTCELLO_SEED);
    bow_noise_init(&S->noise, S->bow_noise_level, S->fs);
    S->n = 0;
    S->vrel_prev = 0.0;
    S->stuck = 0;
    hair_reset(&S->hair);
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

void sim_pluck_torsion(Sim *S, int idx, double x0, double halfwidth, double amp)
{
    Torsion *t = S->str[idx]->tor;
    for (int l = 1; l < t->N; l++) {
        double d = (l * t->h - x0) / halfwidth;
        t->th1[l] = t->th0[l] = fabs(d) < 1.0 ? 0.5 * amp * (1.0 + cos(M_PI * d)) : 0.0;
    }
}

void sim_pluck(Sim *S, double x0, double halfwidth, double amp)
{
    sim_pluck_string(S, S->bowed, 0, x0, halfwidth, amp);
}

void sim_bow_weights(const Sim *S, double beta, int *lo, int *hi, double *J, int *tlo, int *thi, double *Jt)
{
    const String *s = S->s;
    const Torsion *tor = s->tor;
    double xB = s->sp.L * (1.0 - beta);
    bow_weights(s, xB, S->bow_width, lo, hi, J);
    /* the torsion grid can be as coarse as the bow hair: use at least two grid steps so the weights
       interpolate smoothly as the bow moves (raised cosine partition of unity) */
    double wt = S->bow_width > 2.0 * tor->h ? S->bow_width : 2.0 * tor->h;
    bow_weights_grid(tor->h, tor->N, xB, wt, tlo, thi, Jt);
}

/* the played passage (S->play): switch the bowed string at its step, hand the contours of this instant to the controls below */
static void play_apply(Sim *S, double t, double *vB, double *F, double *beta)
{
    const PlayTracks *P = S->play;
    long i = (long)(t * P->rate);
    if (i < 0) i = 0;
    if (i >= P->n) i = P->n - 1;
    int b = P->bowed[i];
    if (b != S->bowed && b < S->nstr) {
        S->bowed = b;
        S->s = S->str[b];
        S->vrel_prev = 0.0;
        S->stuck = 0;
    }
    *vB = trk_eval(P->vB, P->n, P->rate, t);
    *F = trk_eval(P->FB, P->n, P->rate, t);
    *beta = trk_eval(P->beta, P->n, P->rate, t);
}

void sim_step(Sim *S)
{
    double t = S->n * S->k;
    double k = S->k;
    double pvB = 0.0, pF = 0.0, pbeta = 0.1;
    if (S->play) play_apply(S, t, &pvB, &pF, &pbeta);
    String *s = S->s;
    Torsion *tor = s->tor;

    /* bridge: the strings' end force drives the body (leapfrog of the whole
       instrument: both the body and the strings use the state at time n) */
    double zu = 0.0, zw = 0.0;
    if (S->body) {
        double Fu = 0.0, Fw = 0.0;
        for (int i = 0; i < S->nstr; i++) {
            double fu = -string_bridge_force(S->str[i], S->str[i]->y1[0]), fw = -string_bridge_force(S->str[i], S->str[i]->y1[1]);
            Fu += fu;
            Fw += fw;
            if (S->shadow[i]) {
                body_step(S->shadow[i], fu, fw);
                S->out_pstr[i] = S->shadow[i]->p;
                S->out_fstr[i] = fu;
            }
        }
        body_step(S->body, Fu, Fw);
        zu = S->body->zu;
        zw = S->body->zw;
    }

    for (int i = 0; i < S->nstr; i++) {
        if (S->play) {
            finger_apply(&S->finger, S->str[i], trk_eval(S->play->xF[i], S->play->n, S->play->rate, t),
                         trk_eval(S->play->aF[i], S->play->n, S->play->rate, t));
            continue;
        }
        const Env *ex = i == S->bowed ? &S->xF : &S->xFo[i];
        const Env *ea = i == S->bowed ? &S->aF : &S->aFo[i];
        double xf = env_eval(ex, t);
        if (i == S->bowed && S->vib_depth_c > 0.0 && t > S->vib_t0 && env_eval(ea, t) > 0.0)
            xf = sim_vib_finger(S->str[i]->sp.L, xf, S->vib_depth_c * sin(2.0 * M_PI * S->vib_rate * (t - S->vib_t0)));
        finger_apply(&S->finger, S->str[i], xf, env_eval(ea, t));
    }

    double nz = bow_noise_next(&S->noise, &S->rng, S->n, S->fs); /* always drawn on schedule */
    double vB = 0, F = 0, fric = 0, vrel = 0, vbow = 0;
    int lo = 1, hi = 0;
    double J[BOW_MAXPTS], Jt[BOW_MAXPTS];
    int tlo = 1, thi = 0;
    if (S->bow_on) {
        vB = S->play ? pvB : env_eval(&S->vB, t);
        F = S->play ? pF : env_eval(&S->FB, t);
        if (S->walk_sd > 0.0) {
            vB *= 1.0 + walk_eval(S->walk_v, S->walk_n, t);
            F *= 1.0 + walk_eval(S->walk_F, S->walk_n, t);
        }
        double beta = S->play ? pbeta : env_eval(&S->beta, t);
        sim_bow_weights(S, beta, &lo, &hi, J, &tlo, &thi, Jt);
    }
    for (int i = 0; i < S->nstr; i++) {
        if (i == S->bowed) continue;
        string_step_free(S->str[i], 0, NULL);
        string_step_free(S->str[i], 1, NULL);
        torsion_step_free(S->str[i]->tor);
    }
    torsion_step_free(tor);
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
        /* the string surface also moves with r_s theta_t: add the torsional contact velocity and
           admittance (derivation in bow.c); the scalar equation stays scalar */
        double qt = 0.0, Gt = 0.0;
        if (S->torsion_on) {
            const double *t0 = tor->th0;
            double *t2 = tor->th2;
            for (int l = tlo; l <= thi; l++) {
                qt += Jt[l - tlo] * tor->h * (t2[l] - t0[l]);
                Gt += tor->h * (tor->kk * tor->invD[l]) * Jt[l - tlo] * Jt[l - tlo];
            }
            qt *= tor->rs;
            Gt *= tor->rs * tor->rs;
        }
        q = (q + qt) / (2.0 * k) - vB;
        G = F * (G + Gt) / (2.0 * k);
        if (S->hair.on) {
            Hair *h = &S->hair;
            h->dfree = h->A * h->d1 + h->B * h->d0;
            q -= (h->dfree - h->d0) / (2.0 * k);
            G += F * h->g / (2.0 * k);
        }
        double Phi;
        int stuck;
        vrel = bow_solve(&S->inst->bow.friction, q, G, S->vrel_prev, S->stuck, nz, &Phi, &stuck);
        for (int l = lo; l <= hi; l++) u2[l] -= s->kk * s->invD[l] * J[l - lo] * F * Phi;
        if (S->torsion_on)
            for (int l = tlo; l <= thi; l++) tor->th2[l] -= tor->kk * tor->invD[l] * tor->rs * Jt[l - tlo] * F * Phi;
        fric = F * Phi;
        if (S->hair.on) S->hair.d2 = S->hair.dfree + S->hair.g * fric;
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
    if (S->hair.on) {
        if (!S->bow_on) S->hair.d2 = S->hair.A * S->hair.d1 + S->hair.B * S->hair.d0;
        S->hair.dprev = S->hair.d0;
        S->hair.d0 = S->hair.d1;
        S->hair.d1 = S->hair.d2;
    }
    for (int i = 0; i < S->nstr; i++) {
        string_commit(S->str[i]);
        torsion_commit(S->str[i]->tor);
    }
    S->n++;
}

double sim_energy(const Sim *S)
{
    double E = 0.0;
    for (int i = 0; i < S->nstr; i++) {
        const String *s = S->str[i];
        for (int p = 0; p < 2; p++) E += s->rho * string_energy(s, s->y1[p], s->y0[p]);
        E += s->tor->Ip * torsion_energy(s->tor);
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
