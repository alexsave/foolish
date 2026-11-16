#include "legato.h"
#include "analysis.h"
#include "player.h"
#include "wav.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


void rec_free(Rec *r)
{
    free(r->vrel);
    free(r->bridge);
    free(r->aud);
    memset(r, 0, sizeof *r);
}

/* run an initialised sim for secs, record vrel, bridge force and the radiated pressure at 48 kHz */
void rec_sim(Sim *S, double secs, Rec *r)
{
    long n = (long)floor(secs * S->fs + 0.5);
    double *pr = malloc(sizeof(double) * (size_t)n);
    r->n = n;
    r->vrel = malloc(sizeof(double) * (size_t)n);
    r->bridge = malloc(sizeof(double) * (size_t)n);
    for (long i = 0; i < n; i++) {
        sim_step(S);
        pr[i] = S->out[7];
        r->vrel[i] = S->out[3];
        r->bridge[i] = S->out[0];
    }
    float *y;
    int fac = (int)(S->fs / 48000.0 + 0.5);
    r->naud = wav_decimate(pr, n, fac, &y);
    r->aud = malloc(sizeof(double) * (size_t)r->naud);
    for (long i = 0; i < r->naud; i++) r->aud[i] = y[i];
    r->audfs = S->fs / fac;
    free(y);
    free(pr);
}

double finger_for(const Instrument *I, int si, int semis)
{
    const StringSpec *sp = &I->str[si];
    double fn = sp->f0 * pow(2.0, semis / 12.0), x = sp->L * (1.0 - sp->f0 / fn);
    if (semis < INSTR_MAXSEMI) x = sim_vib_finger(sp->L, x, I->stop_cents[si][semis]);
    return x;
}

static double rms_at(const double *x, long n, double fs, double t, double w)
{
    long a = (long)(t * fs), b = a + (long)(w * fs);
    if (a < 0) a = 0;
    if (b > n) b = n;
    double s = 0.0;
    for (long i = a; i < b; i++) s += x[i] * x[i];
    return sqrt(s / (double)(b - a + 1));
}


int legato_default(LegatoArgs *a, const char *scen, const char *string)
{
    memset(a, 0, sizeof *a);
    a->ic = *instrument_cello();
    a->si = instrument_string_index(&a->ic, string);
    if (a->si < 0 || (strcmp(scen, "slur") && strcmp(scen, "lift") && strcmp(scen, "bow"))) return 1;
    a->scen = scen;
    a->ramp = 0.010; a->dip = 0.8; a->tsw = 1.0; a->F = -1.0; a->vB = 0.1; a->fw = 0.04; a->bw = 0.04; a->bdip = 0.75;
    a->s0 = !strcmp(scen, "lift") ? 2 : 7;
    a->s1 = !strcmp(scen, "slur") ? 9 : !strcmp(scen, "lift") ? 0 : 7;
    return 0;
}

int legato_run(const LegatoArgs *a, LegatoOut *out)
{
    const char *scen = a->scen;
    Instrument ic = a->ic;
    int si = a->si, s0 = a->s0, s1 = a->s1, only_bowed = a->only_bowed;
    double ramp = a->ramp, dip = a->dip, tsw = a->tsw, F = a->F, vB = a->vB, fdip = a->fdip, fw = a->fw, bw = a->bw, bdip = a->bdip;
    uint64_t seed = a->seed;
    const StringSpec *sp = &ic.str[si];
    double beta_rel = 0.1, beta = beta_rel / pow(2.0, s0 / 12.0);
    double lo, hi;
    if (F < 0.0) { if (!player_band(&ic, si, beta_rel, vB, &lo, &hi)) { lo = 0.5 * ic.helmholtz_force[si]; hi = 2.0 * ic.helmholtz_force[si]; } F = sqrt(lo * hi); }
    double x0 = finger_for(&ic, si, s0), x1 = s1 > 0 ? finger_for(&ic, si, s1) : x0;
    double f_old = sp->f0 * pow(2.0, s0 / 12.0), f_new = sp->f0 * pow(2.0, s1 / 12.0);
    Sim S;
    SimOpts so = {1, !only_bowed, 0, NULL};
    sim_init_instrument(&S, &ic, FS96, &so, sp->name);
    env_const(&S.beta, beta);
    S.FB.n = 0;
    env_add(&S.FB, 0.0, 0.0);
    env_add(&S.FB, 0.005, F);
    S.vB.n = 0;
    env_add(&S.vB, 0.0, 0.0);
    env_add(&S.vB, 0.02, 0.0);
    env_add(&S.vB, 0.02 + vB / 2.0, vB); /* a = 2 m/s^2 */
    S.xF.n = 0;
    S.aF.n = 0;
    double t_end_ev = tsw + ramp;
    if (!strcmp(scen, "slur")) {
        env_add(&S.xF, 0.0, x0);
        env_add(&S.xF, tsw + 0.5 * ramp - 1e-5, x0);
        env_add(&S.xF, tsw + 0.5 * ramp, x1);
        env_add(&S.aF, 0.0, 1.0);
        for (int k = 0; k <= 8; k++) env_add(&S.aF, tsw + ramp * k / 8.0, 1.0 - dip * sin(M_PI * k / 8.0));
    } else if (!strcmp(scen, "lift")) {
        env_add(&S.xF, 0.0, x0);
        env_add(&S.aF, 0.0, 1.0);
        for (int k = 0; k <= 8; k++) env_add(&S.aF, tsw + ramp * k / 8.0, 0.5 * (1.0 + cos(M_PI * k / 8.0)));
    } else { /* bow change */
        env_const(&S.xF, x0);
        env_const(&S.aF, 1.0);
        S.vB.n = 0;
        env_add(&S.vB, 0.0, 0.0);
        env_add(&S.vB, 0.02, 0.0);
        env_add(&S.vB, 0.02 + vB / 2.0, vB);
        env_add(&S.vB, tsw - 0.5 * bw, vB);
        env_add(&S.vB, tsw + 0.5 * bw, -vB);
        S.FB.n = 0;
        env_add(&S.FB, 0.0, 0.0);
        env_add(&S.FB, 0.005, F);
        env_add(&S.FB, tsw - 0.5 * bw, F);
        env_add(&S.FB, tsw, bdip * F);
        env_add(&S.FB, tsw + 0.5 * bw, F);
        t_end_ev = tsw + 0.5 * bw;
    }
    if (fdip > 0.0 && strcmp(scen, "bow")) { /* the force eases to (1 - fdip) F over fw s around the event and returns */
        S.FB.n = 0;
        env_add(&S.FB, 0.0, 0.0);
        env_add(&S.FB, 0.005, F);
        for (int k = 0; k <= 10; k++) env_add(&S.FB, tsw - 0.5 * fw + fw * k / 10.0, F * (1.0 - fdip * 0.5 * (1.0 - cos(2.0 * M_PI * k / 10.0))));
    }
    sim_start(&S);
    if (seed) rng_seed(&S.rng, ALTCELLO_SEED ^ (seed * 0x9e3779b97f4a7c15ULL));
    Rec r;
    rec_sim(&S, tsw + 0.7, &r);
    sim_free(&S);
    double fs = r.audfs;
    /* the state before: periodic at the old pitch? */
    double pre = audio_periodic_time(r.aud, r.naud, fs, f_old, tsw - 0.3, 0.95, 10);
    int pre_ok = pre >= 0 && pre < tsw - 0.2;
    double t_ev = !strcmp(scen, "bow") ? tsw - 0.5 * bw : tsw;
    /* pitch track */
    double tc[4000], fq[4000];
    double W = 2.0 / f_new;
    long m = pitch_track(r.aud, r.naud, fs, 0.8 * (f_old < f_new ? f_old : f_new), 1.2 * (f_old > f_new ? f_old : f_new), W, tsw - 0.05, tsw + 0.6, 0.0005, tc, fq, 4000);
    double fin[64];
    int nf = 0;
    for (long k = 0; k < m && nf < 64; k++) if (tc[k] > tsw + 0.45 && tc[k] < tsw + 0.55 && fq[k] > 0) fin[nf++] = fq[k];
    for (int a = 0; a < nf; a++) for (int b = a + 1; b < nf; b++) if (fin[b] < fin[a]) { double q = fin[a]; fin[a] = fin[b]; fin[b] = q; }
    double f_fin = nf ? fin[nf / 2] : f_new;
    double t_p = -1.0, dipc = 0.0;
    long need = (long)ceil(5.0 / f_fin / 0.0005);
    long run = 0;
    for (long k = 0; k < m; k++) {
        double c = tc[k] >= t_ev && fq[k] > 0 ? cents(fq[k], f_fin) : 1e9;
        if (tc[k] >= t_ev && tc[k] < t_ev + 0.2 && fq[k] > 0 && c < dipc) dipc = c;
        if (tc[k] < t_ev) continue;
        run = fabs(c) < 10.0 ? run + 1 : 0;
        if (t_p < 0 && run >= need) t_p = tc[k - need + 1] - t_ev;
    }
    double rep = audio_periodic_time(r.aud, r.naud, fs, f_fin, t_ev, 0.95, 10);
    double lev0 = rms_at(r.aud, r.naud, fs, tsw - 0.1, 0.05), lmin = lev0;
    for (double t = t_ev; t < t_ev + 0.3; t += 0.005) { double v = rms_at(r.aud, r.naud, fs, t, 0.005); if (v < lmin) lmin = v; }
    out->pre_ok = pre_ok;
    out->f_fin = f_fin;
    out->cents_nominal = cents(f_fin, f_new);
    out->t_pitch = t_p;
    out->t_pitch_ramp = t_p >= 0 ? t_p - (t_end_ev - t_ev) : -1.0;
    out->t_reper = rep >= 0 ? rep - t_ev : -1.0;
    out->dip_c = dipc;
    out->lev_dip_db = 20.0 * log10(lmin / lev0 + 1e-12);
    out->ntrack = m;
    out->tc = malloc(sizeof(double) * (size_t)m);
    out->fq = malloc(sizeof(double) * (size_t)m);
    memcpy(out->tc, tc, sizeof(double) * (size_t)m);
    memcpy(out->fq, fq, sizeof(double) * (size_t)m);
    rec_free(&r);
    return 0;
}

void legato_out_free(LegatoOut *o)
{
    free(o->tc);
    free(o->fq);
    memset(o, 0, sizeof *o);
}
