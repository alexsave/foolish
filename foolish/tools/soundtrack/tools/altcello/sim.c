#include "sim.h"
#include "analysis.h"
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
    S->bow_noise_fc = I->bow.noise_fc;
    S->bow_noise_rate = I->bow.noise_rate;
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
    S->sidx[0] = 0;
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
        if (o->lossless) {
            sp.sigma0 = sp.sigma1 = sp.zeta_t = 0.0;
            for (int r = 0; r < STR_NREL; r++) sp.rel_s1[r] = 0.0;
        }
        S->str[S->nstr] = string_new(&sp, fs);
        S->sidx[S->nstr] = i;
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
    bow_noise_init_ex(&S->noise, S->bow_noise_level, S->fs, S->bow_noise_fc, S->bow_noise_rate);
    S->n = 0;
    S->vrel_prev = 0.0;
    S->nz_hp[0] = S->nz_hp[1] = 0.0;
    S->stuck = 0;
    hair_reset(&S->hair);
    thermal_init(&S->th, &S->inst->bow.thermal, S->bow_width, S->k);
    for (int g = 0; g < TH_MAXN; g++) thermal_init(&S->thg[g], &S->inst->bow.thermal, S->bow_width, S->k);
    analysis_slip_thr_for(S->inst->bow.model == BOW_THERMAL);
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
        thermal_reset(&S->th);
        for (int g = 0; g < TH_MAXN; g++) thermal_reset(&S->thg[g]);
    }
    *vB = trk_eval(P->vB, P->n, P->rate, t);
    *F = trk_eval(P->FB, P->n, P->rate, t);
    *beta = trk_eval(P->beta, P->n, P->rate, t);
}


/* DISTRIBUTED THERMAL CONTACT (roughness iteration 1, F3). The bow hair is a ribbon across `width`: the contact weights J (bow.c) are split into
 * n = ThermalSpec.ncont groups of equal weight along the string (the torsion points go to the group of the nearest transverse point), and every group is a
 * contact of its own with its own bristle state z_k and temperature tau_k (van Walstijn et al. 2026 use N = 4 points under the bow). The one-step string
 * admittance is diagonal in the grid points, so groups of disjoint points do not couple within a step and every group solves the SCALAR equation
 *   v_k = q_k - G_k Phi_k(v_k),   q_k = (qu_k + c_k qt_k) / (2 k W_k) - vB,   G_k = F p_k (Gu_k + c_k^2 Gt_k) / (2 k W_k),
 * W_k = sum of J h over the group (the share of the ribbon), c_k = W_k / Wt_k (the torsion weights of the group rescaled to the same share, so the load of the
 * group is the same on both polarisations), p_k = (1 + tilt x_k) / norm the pressure profile across the ribbon (x_k in [-1, 1] the group's position,
 * sum W_k p_k = 1), so the normal force of group k is F W_k p_k and its force density on the string F p_k Phi_k J_l. The power given to the string by group k is
 * F p_k W_k Phi_k v_k + the torsion share, the same product as the lumped contact's, so every group dissipates what its bristle model dissipates (>= 0).
 * Returns the weighted mean relative velocity and the total friction force; th.tau and th.z hold the weighted means. */
static void multi_contact(Sim *S, int lo, int hi, const double *J, int tlo, int thi, const double *Jt, double vB, double F, double nz, double *vrel_out, double *fric_out)
{
    String *s = S->s;
    Torsion *tor = s->tor;
    double k = S->k;
    int n = S->inst->bow.thermal.ncont;
    double tilt = S->inst->bow.thermal.tilt;
    int grp[BOW_MAXPTS], tgrp[BOW_MAXPTS];
    double W[TH_MAXN] = {0}, Wt[TH_MAXN] = {0}, tot = 0.0, cum = 0.0;
    for (int l = lo; l <= hi; l++) tot += J[l - lo] * s->h;
    for (int l = lo; l <= hi; l++) {
        double w = J[l - lo] * s->h;
        int g = (int)((cum + 0.5 * w) / tot * n);
        grp[l - lo] = g >= n ? n - 1 : g;
        W[grp[l - lo]] += w;
        cum += w;
    }
    double xc[TH_MAXN] = {0};
    cum = 0.0;
    for (int g = 0; g < n; g++) { xc[g] = 2.0 * (cum + 0.5 * W[g]) / tot - 1.0; cum += W[g]; }
    for (int m = tlo; m <= thi; m++) {
        int best = lo;
        double xm = m * tor->h, bd = 1e300;
        for (int l = lo; l <= hi; l++) {
            double d = fabs(l * s->h - xm);
            if (d < bd) { bd = d; best = l; }
        }
        tgrp[m - tlo] = grp[best - lo];
        Wt[tgrp[m - tlo]] += Jt[m - tlo] * tor->h;
    }
    double pw = 0.0, p[TH_MAXN], pn = 0.0, vm = 0.0, fr = 0.0, tm = 0.0, zm = 0.0;
    for (int g = 0; g < n; g++) { p[g] = 1.0 + tilt * xc[g]; if (p[g] < 0.05) p[g] = 0.05; pn += W[g] * p[g]; }
    const double *u0 = s->y0[0];
    double *u2 = s->y2[0];
    double qu[TH_MAXN] = {0}, Gu[TH_MAXN] = {0}, qt[TH_MAXN] = {0}, Gt[TH_MAXN] = {0}, c[TH_MAXN];
    for (int l = lo; l <= hi; l++) {
        int g = grp[l - lo];
        qu[g] += J[l - lo] * s->h * (u2[l] - u0[l]);
        Gu[g] += s->h * (s->kk * s->invD[l]) * J[l - lo] * J[l - lo];
    }
    for (int g = 0; g < n; g++) c[g] = Wt[g] > 0.0 ? W[g] / Wt[g] : 0.0;
    if (S->torsion_on)
        for (int m = tlo; m <= thi; m++) {
            int g = tgrp[m - tlo];
            qt[g] += Jt[m - tlo] * tor->h * (tor->th2[m] - tor->th0[m]);
            Gt[g] += tor->h * (tor->kk * tor->invD[m]) * Jt[m - tlo] * Jt[m - tlo];
        }
    double Phi[TH_MAXN] = {0}, vg[TH_MAXN] = {0};
    for (int g = 0; g < n; g++) {
        if (W[g] <= 0.0) continue;
        double pg = p[g] / pn, Fg = F * pg; /* normalised: sum W p = 1 */
        double cg = c[g], rs = tor->rs;
        double q = (qu[g] + rs * cg * qt[g]) / (2.0 * k * W[g]) - vB;
        double G = Fg * (Gu[g] + rs * rs * cg * cg * Gt[g]) / (2.0 * k * W[g]);
        double v = thermal_solve(&S->thg[g], Fg, q, G, nz, &Phi[g]);
        vg[g] = v;
        vm += W[g] * v;
        pw += F * pg * W[g] * Phi[g] * v;
        fr += W[g] * Fg * Phi[g];
        tm += W[g] * S->thg[g].tau;
        zm += W[g] * S->thg[g].z;
        p[g] = pg;
    }
    for (int l = lo; l <= hi; l++) {
        int g = grp[l - lo];
        u2[l] -= s->kk * s->invD[l] * J[l - lo] * F * p[g] * Phi[g];
    }
    if (S->torsion_on)
        for (int m = tlo; m <= thi; m++) {
            int g = tgrp[m - tlo];
            tor->th2[m] -= tor->kk * tor->invD[m] * tor->rs * c[g] * Jt[m - tlo] * F * p[g] * Phi[g];
        }
    if (S->check_contact) { /* diagnosis (test_contact): the contact velocity of every point recomputed from the updated states equals vB + its v_rel */
        double vu[TH_MAXN] = {0}, vt[TH_MAXN] = {0};
        for (int l = lo; l <= hi; l++) vu[grp[l - lo]] += J[l - lo] * s->h * (u2[l] - u0[l]);
        if (S->torsion_on)
            for (int m = tlo; m <= thi; m++) vt[tgrp[m - tlo]] += Jt[m - tlo] * tor->h * (tor->th2[m] - tor->th0[m]);
        for (int g = 0; g < n; g++) {
            if (W[g] <= 0.0) continue;
            double vc = (vu[g] / W[g] + (Wt[g] > 0.0 ? tor->rs * vt[g] / Wt[g] : 0.0)) / (2.0 * k), e = fabs(vc - vB - vg[g]);
            if (e > S->check_err) S->check_err = e;
        }
    }
    S->th.tau = tm / tot;
    S->th.z = zm / tot;
    S->th.qdiss = S->th.Hz = 0.0;
    for (int g = 0; g < n; g++) { S->th.qdiss += W[g] * S->thg[g].qdiss; S->th.Hz += W[g] * S->thg[g].Hz; }
    *vrel_out = vm / tot;
    *fric_out = fr;
    S->pwr = pw;
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
    double zu = 0.0, zw = 0.0, zus[SIM_MAXSTR] = {0}, zws[SIM_MAXSTR] = {0}; /* the FE body gives each string its own notch displacement */
    double vub = 0.0, vwb = 0.0;
    if (S->body && S->body->nfe > 0) {
        double Fu[BODY_NS] = {0}, Fw[BODY_NS] = {0};
        for (int i = 0; i < S->nstr; i++) {
            double fu = -string_bridge_force(S->str[i], S->str[i]->y1[0]), fw = -string_bridge_force(S->str[i], S->str[i]->y1[1]);
            Fu[S->sidx[i]] = fu;
            Fw[S->sidx[i]] = fw;
            if (S->shadow[i]) {
                double su[BODY_NS] = {0}, sw[BODY_NS] = {0};
                su[S->sidx[i]] = fu;
                sw[S->sidx[i]] = fw;
                body_step_str(S->shadow[i], su, sw);
                S->out_pstr[i] = S->shadow[i]->p;
                S->out_fstr[i] = fu;
            }
        }
        body_step_str(S->body, Fu, Fw);
        for (int i = 0; i < S->nstr; i++) { zus[i] = S->body->zus[S->sidx[i]]; zws[i] = S->body->zws[S->sidx[i]]; }
        vub = S->body->vus[S->sidx[S->bowed]];
        vwb = S->body->vws[S->sidx[S->bowed]];
    } else if (S->body) {
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
        for (int i = 0; i < S->nstr; i++) { zus[i] = zu; zws[i] = zw; }
        vub = S->body->vu;
        vwb = S->body->vw;
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
    {
        int bi = S->sidx[S->bowed];
        double hpc = S->inst->bow.noise_hp[bi];
        if (hpc > 0.0) nz = bow_noise_highpass(S->nz_hp, hpc, S->fs, nz);
        nz *= S->inst->bow.noise_scale[bi];
    }
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

        if (S->inst->bow.model == BOW_THERMAL && S->inst->bow.thermal.ncont > 1 && !S->hair.on) {
            multi_contact(S, lo, hi, J, tlo, thi, Jt, vB, F, nz, &vrel, &fric);
            vbow = vrel + vB;
            S->vrel_prev = vrel;
            S->stuck = 0;
        } else {
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
            if (S->inst->bow.model == BOW_THERMAL) {
                vrel = thermal_solve(&S->th, F, q, G, nz, &Phi);
                stuck = 0;
            } else
                vrel = bow_solve(&S->inst->bow.friction, q, G, S->vrel_prev, S->stuck, nz, &Phi, &stuck);
            for (int l = lo; l <= hi; l++) u2[l] -= s->kk * s->invD[l] * J[l - lo] * F * Phi;
            if (S->torsion_on)
                for (int l = tlo; l <= thi; l++) tor->th2[l] -= tor->kk * tor->invD[l] * tor->rs * Jt[l - tlo] * F * Phi;
            fric = F * Phi;
            S->pwr = fric * vrel;
            if (S->hair.on) S->hair.d2 = S->hair.dfree + S->hair.g * fric;
            vbow = vrel + vB;
            S->vrel_prev = vrel;
            S->stuck = stuck;
        }
    } else {
        string_step_free(s, 1, NULL);
    }
    for (int i = 0; i < S->nstr; i++) {
        S->str[i]->y2[0][S->str[i]->N] = zus[i];
        S->str[i]->y2[1][S->str[i]->N] = zws[i];
    }
    S->out[0] = string_bridge_force(s, s->y2[0]);
    S->out[1] = string_bridge_force(s, s->y2[1]);
    S->out[2] = vbow;
    S->out[3] = vrel;
    S->out[4] = fric;
    S->out[5] = S->body ? vub : 0.0;
    S->out[6] = S->body ? vwb : 0.0;
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
