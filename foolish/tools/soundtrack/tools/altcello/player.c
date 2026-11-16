#include "player.h"
#include "rng.h"
#include "sim.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- note list */
int notes_read(const char *path, Note **out, int *n)
{
    FILE *f = fopen(path, "r");
    if (!f) return 1;
    int cap = 64, cnt = 0;
    Note *a = malloc(sizeof(Note) * (size_t)cap);
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char *tok[10];
        int nt = 0;
        for (char *s = strtok(line, " \t\r\n"); s && nt < 10; s = strtok(NULL, " \t\r\n")) tok[nt++] = s;
        if (nt == 0) continue;
        if (nt < 4) { fclose(f); free(a); return 2; }
        if (cnt == cap) { cap *= 2; a = realloc(a, sizeof(Note) * (size_t)cap); }
        Note *q = &a[cnt++];
        q->start = atof(tok[0]);
        q->dur = atof(tok[1]);
        q->midi = atoi(tok[2]);
        q->vel = atof(tok[3]);
        q->string = -1;
        q->vib = -1;
        q->depth_c = q->rate_hz = -1.0;
        q->slide = 0;
        if (nt > 4 && strcmp(tok[4], "-")) {
            static const char *nm[4] = {"C", "G", "D", "A"};
            q->string = -2;
            for (int i = 0; i < 4; i++) if (!strcmp(tok[4], nm[i])) q->string = i;
            if (q->string == -2) { fclose(f); free(a); return 3; }
        }
        if (nt > 5 && strcmp(tok[5], "-")) q->vib = atoi(tok[5]) != 0;
        if (nt > 6 && strcmp(tok[6], "-")) q->depth_c = atof(tok[6]);
        if (nt > 7 && strcmp(tok[7], "-")) q->rate_hz = atof(tok[7]);
        if (nt > 8 && strcmp(tok[8], "-")) q->slide = atoi(tok[8]) != 0;
    }
    fclose(f);
    *out = a;
    *n = cnt;
    return 0;
}

void player_init(Player *p, const Instrument *inst, PlayStyle style, uint64_t seed)
{
    p->inst = inst;
    p->style = style;
    p->seed = seed;
    p->ctrl_rate = PLAYER_CTRL_RATE;
    p->vib_all_off = 0;
    p->damp_press = 0.12;
    p->damp_delay = 0.3;
    p->damp_ease = 3.0;
    p->vb_min = 0.06;
    p->vb_span = 0.07;
    p->bc_lo = 0.03;
    p->bc_hi = 0.05;
    p->dip = 0.75;
    p->slur = 0;
    p->swap_dip = 0.8;
    p->rock = 0.05;
    p->acc_lo = 0.06;
    p->acc_hi = 0.12;
    p->beta_base = 0.105;
    p->frac_min = 0.35;
    p->frac_span = 0.25;
    p->att_on = 1;
    p->att_acc = 0.0; /* 0: the Instrument's per string wedge acceleration (instrument_cello.c WEDGE); iteration 1 used 0.5 m/s^2 for all strings */
    p->att_k = 0.0;   /* 0: the Instrument's per string wedge slope */
    p->att_hold = 0.1; /* the wedge force is held until the Helmholtz motion has formed (G2 on C: 1 of 6 takes derailed at 0 s and 0.05 s hold, 5 of 6 periodic at 0.1 s) */
    p->f_ramp = 0.015; /* 0.005 rang the string 24 dB under the sustain before the bow moved, 0.015 36-40 dB (richness F0) */
    p->land_lead = 0.03;
    p->land_ramp = 0.01;
    p->predamp = 0;
    p->predamp_press = 0.12;
    p->predamp_delay = 0.05;
    p->predamp_ease = 0.03;
    p->rel_lo = 0.05;
    p->rel_hi = 0.15;
    p->att_v_frac = 0.85; /* was 0.4 with the 0.5 m/s^2 ramp: the speed at 50 ms was 0.025 m/s and the level 8-17 dB under Philharmonia */
    p->att_frac = 0.4; /* was 0.6 (candidate 2, friction curve); thermal friction: the first take of G2 reaches periodic motion in 0.06-0.07 s on all three seeds (0.17 s median, 0.45 s worst at 0.6), D3 0.06, A3 0.08 (charts/onset_scan.py) */
    p->sw_lo = 0.25;
    p->sw_hi = 0.45;
}

/* ---------------------------------------------------------------- playability table */
static int band_node(const Instrument *I, int s, int ib, int iv, double *lo, double *hi)
{
    PlayBand b = I->playability[s][ib][iv];
    if (b.fmax <= 0.0 || b.fmin <= 0.0) return 0;
    *lo = log(b.fmin);
    *hi = log(b.fmax);
    return 1;
}

int player_band(const Instrument *I, int str, double beta, double vb, double *fmin, double *fmax)
{
    const double *bg = I->play_beta, *vg = I->play_vb;
    if (beta < bg[0]) beta = bg[0];
    if (beta > bg[PLAY_NBETA - 1]) beta = bg[PLAY_NBETA - 1];
    if (vb < vg[0]) vb = vg[0];
    if (vb > vg[PLAY_NVB - 1]) vb = vg[PLAY_NVB - 1];
    int ib = 0, iv = 0;
    while (ib < PLAY_NBETA - 2 && beta > bg[ib + 1]) ib++;
    while (iv < PLAY_NVB - 2 && vb > vg[iv + 1]) iv++;
    double ub = (beta - bg[ib]) / (bg[ib + 1] - bg[ib]);
    double uv = log(vb / vg[iv]) / log(vg[iv + 1] / vg[iv]);
    double lo[2][2], hi[2][2];
    for (int a = 0; a < 2; a++)
        for (int b = 0; b < 2; b++)
            if (!band_node(I, str, ib + a, iv + b, &lo[a][b], &hi[a][b])) return 0;
    double L = (1 - ub) * ((1 - uv) * lo[0][0] + uv * lo[0][1]) + ub * ((1 - uv) * lo[1][0] + uv * lo[1][1]);
    double H = (1 - ub) * ((1 - uv) * hi[0][0] + uv * hi[0][1]) + ub * ((1 - uv) * hi[1][0] + uv * hi[1][1]);
    *fmin = exp(L);
    *fmax = exp(H);
    return 1;
}

static void band_or_anchor(const Instrument *I, int str, double beta, double vb, double *fmin, double *fmax)
{
    if (!player_band(I, str, beta, vb, fmin, fmax)) {
        *fmin = 0.5 * I->helmholtz_force[str];
        *fmax = 2.0 * I->helmholtz_force[str];
    }
}

double player_band_frac(const Instrument *I, int str, double beta, double vb, double F)
{
    double lo, hi;
    band_or_anchor(I, str, beta, vb, &lo, &hi);
    return log(F / lo) / log(hi / lo);
}

/* ---------------------------------------------------------------- random streams */
enum { PU_NOTE, PU_PHRASE, PU_VIB, PU_VIBDRIFT, PU_PITCH, PU_BETA, PU_SPEED, PU_FORCE };

typedef struct {
    Rng r;
    long *cnt;
} Dr;

static void dr_open(Dr *d, const Player *p, long *cnt, long idx, int purpose)
{
    uint64_t s = ALTCELLO_PLAYER_SEED ^ (p->seed * 0x9e3779b97f4a7c15ULL) ^ ((uint64_t)(idx + 1) * 0xd1b54a32d192ed03ULL) ^
                 ((uint64_t)(purpose + 1) * 0x94d049bb133111ebULL);
    rng_seed(&d->r, s);
    d->cnt = cnt;
}
static double d_uni(Dr *d, double a, double b)
{
    (*d->cnt)++;
    return a + (b - a) * rng_uniform(&d->r);
}
static double d_gauss(Dr *d)
{
    (*d->cnt)++;
    return rng_gauss(&d->r);
}

/* slow band-limited drift: white gaussian through two one-pole low-pass sections at bw Hz (a burn-in of 1/bw seconds first), scaled to
   the stationary standard deviation sd; n samples at `rate`; the draw count depends on n, rate and bw only */
static void bl_noise(Dr *d, double *out, long n, double rate, double bw, double sd)
{
    double a = exp(-2.0 * M_PI * bw / rate);
    double var = (1.0 - a) * (1.0 + a * a) / ((1.0 + a) * (1.0 + a) * (1.0 + a));
    double g = sd / sqrt(var);
    double y1 = 0.0, y2 = 0.0;
    long burn = (long)(rate / bw);
    for (long i = -burn; i < n; i++) {
        y1 = a * y1 + (1.0 - a) * d_gauss(d);
        y2 = a * y2 + (1.0 - a) * y1;
        if (i >= 0) out[i] = g * y2;
    }
}

static double rc(double u) { return u <= 0.0 ? 0.0 : u >= 1.0 ? 1.0 : 0.5 * (1.0 - cos(M_PI * u)); }
static double clampd(double x, double a, double b) { return x < a ? a : x > b ? b : x; }
/* arch: 0 at both ends, 1 at u = pk (half cosines) */
static double hump(double u, double pk)
{
    if (u <= 0.0 || u >= 1.0) return 0.0;
    return u < pk ? 0.5 * (1.0 + cos(M_PI * (u - pk) / pk)) : 0.5 * (1.0 + cos(M_PI * (u - pk) / (1.0 - pk)));
}

/* ---------------------------------------------------------------- string and position */
static double midi_hz(int m) { return 440.0 * pow(2.0, (m - 69) / 12.0); }

static int is_open_pitch(const Instrument *I, int midi)
{
    for (int s = 0; s < I->nstr; s++) if (I->midi_open[s] == midi) return s + 1;
    return 0;
}

/* lowest position: the highest open string not above the pitch, but an expert stops a note with vibrato instead of playing it open (a
   string below, 7 semitones up) unless there is no string below */
static int pick_string(const Instrument *I, int midi, int vib_on, const NotePlan *prev, int prev_midi, int stay_ok)
{
    int open = is_open_pitch(I, midi);
    if (open && !vib_on) return open - 1;
    /* legato on the same string: steps within 5 up / 3 down, not beyond 7 semitones above the open string (ASSUMED: up to fourth position) */
    if (stay_ok && prev) {
        int d = midi - prev_midi, semis = midi - I->midi_open[prev->string];
        if (d >= -3 && d <= 5 && semis >= (vib_on ? 1 : 0) && semis <= 7) return prev->string;
    }
    int best = -1;
    for (int s = 0; s < I->nstr; s++) {
        int semis = midi - I->midi_open[s];
        if (semis < 0) continue;
        if (semis == 0 && vib_on && s > 0) continue; /* stop it on the string below */
        if (best < 0 || I->midi_open[s] > I->midi_open[best]) best = s;
    }
    if (best < 0) best = 0;
    if (midi - I->midi_open[best] == 0 && vib_on && best > 0) best--;
    return best;
}

void played_free(Played *p)
{
    trk_free(&p->trk);
    free(p->cents);
    free(p->beta_rel);
    free(p->plan);
    memset(p, 0, sizeof *p);
}

int player_play(const Player *p, const Note *nt, int n, Played *out)
{
    const Instrument *I = p->inst;
    const double R = p->ctrl_rate;
    const int expert = p->style == STYLE_EXPERT;
    memset(out, 0, sizeof *out);
    if (n < 1) return 1;
    long *cnt = &out->ndraws;
    NotePlan *pl = calloc((size_t)n, sizeof(NotePlan));
    out->plan = pl;
    out->nnotes = n;
    double last_end = 0.0;
    for (int k = 0; k < n; k++) if (nt[k].start + nt[k].dur > last_end) last_end = nt[k].start + nt[k].dur;
    out->dur = last_end + 1.0;
    long N = (long)ceil(out->dur * R) + 2;
    trk_alloc(&out->trk, R, N);
    out->cents = calloc((size_t)N, sizeof(double));
    out->beta_rel = calloc((size_t)N, sizeof(double));
    PlayTracks *T = &out->trk;
    for (int s = 0; s < TRK_NSTR; s++) for (long i = 0; i < N; i++) T->xF[s][i] = 0.3;
    double *wsum = calloc((size_t)N, sizeof(double)), *bacc = calloc((size_t)N, sizeof(double)), *sacc = calloc((size_t)N, sizeof(double));

    /* ---- pass 1: string, links, scalar draws */
    double Wk[1024];       /* bow-change window length into note k (s) */
    double Tdamp[1024], damp_a[1024];
    if (n > 1024) { free(wsum); free(bacc); return 2; }
    double beta_phrase = 0.1;
    struct { double onset, rate, dmult, dacc, lead, pk, bwb, bwv, bwf, sdb, frac_nov; double scoop, settle, tsw, trel; } sc[1024];
    for (int k = 0; k < n; k++) {
        NotePlan *q = &pl[k];
        const Note *nn = &nt[k];
        q->t_s = nn->start;
        q->t_e = nn->start + nn->dur;
        q->linked_prev = k > 0 && nn->start - (nt[k - 1].start + nt[k - 1].dur) < 0.15;
        q->linked_next = k + 1 < n && nt[k + 1].start - q->t_e < 0.15;
        q->vib_on = nn->vib < 0 ? 1 : nn->vib;
        if (p->vib_all_off) q->vib_on = 0;
        if (nn->string >= 0) q->string = nn->string;
        else q->string = pick_string(I, nn->midi, q->vib_on, k > 0 ? &pl[k - 1] : NULL, k > 0 ? nt[k - 1].midi : 0, q->linked_prev);
        q->semis = nn->midi - I->midi_open[q->string];
        q->ratio = q->semis > 0 ? midi_hz(nn->midi) / I->str[q->string].f0 : 1.0;
        Dr d;
        dr_open(&d, p, cnt, k, PU_NOTE);
        sc[k].scoop = d_uni(&d, 6.0, 12.0);
        sc[k].settle = d_uni(&d, 0.04, 0.06);
        sc[k].onset = d_uni(&d, 0.2, 0.3);
        sc[k].rate = d_uni(&d, 4.9, 5.3); /* 5.2 - 5.6 before: measured 5.9 Hz against 5.4 Hz (median) of the real notes */
        sc[k].dmult = clampd(1.0 + 0.15 * d_gauss(&d), 0.6, 1.5);
        sc[k].dacc = d_uni(&d, p->acc_lo, p->acc_hi);
        sc[k].lead = d_uni(&d, 0.02, 0.04);
        Wk[k] = d_uni(&d, p->bc_lo, p->bc_hi);
        sc[k].pk = d_uni(&d, 0.4, 0.6);
        sc[k].bwb = d_uni(&d, 0.5, 3.0);
        sc[k].bwv = d_uni(&d, 0.5, 3.0);
        sc[k].bwf = d_uni(&d, 0.5, 3.0);
        sc[k].sdb = d_uni(&d, 0.02, 0.03);
        Tdamp[k] = d_uni(&d, 0.08, 0.15);
        sc[k].frac_nov = d_uni(&d, 0.85, 0.95);
        sc[k].tsw = d_uni(&d, p->sw_lo, p->sw_hi); /* appended last: the earlier draws keep their values */
        sc[k].trel = d_uni(&d, p->rel_lo, p->rel_hi); /* appended after the swell: the earlier draws keep their values */
        damp_a[k] = p->damp_press;
        if (!q->linked_prev) { /* a new phrase: beta drawn once */
            Dr dp;
            dr_open(&dp, p, cnt, k, PU_PHRASE);
            double v = nn->vel;
            double b = p->beta_base - 0.03 * (v - 0.5) / 0.5 + 0.004 * d_gauss(&dp);
            beta_phrase = expert ? clampd(b, 0.08, 0.13) : 0.1;
        }
        q->beta0 = beta_phrase;
        q->vB0 = clampd(p->vb_min + p->vb_span * nn->vel, 0.06, 0.18);
        q->frac0 = expert ? clampd(p->frac_min + p->frac_span * nn->vel, 0.20, 0.80) : sc[k].frac_nov;
        q->depth_c = (nn->depth_c >= 0.0 ? nn->depth_c : 8.0) * sc[k].dmult /* 11 before: the measured depth of the three Philharmonia notes is 7.4 c (median), ours was 10.3 */;
        q->rate_hz = nn->rate_hz >= 0.0 ? nn->rate_hz : sc[k].rate;
        q->scoop_c = sc[k].scoop;
        q->T_acc = sc[k].dacc;
        q->t_vib0 = expert ? q->t_s + sc[k].onset : q->t_s;
        if (!expert) Wk[k] = 0.01;
    }
    /* directions */
    int sgn[1024];
    for (int k = 0; k < n; k++) sgn[k] = k == 0 ? 1 : pl[k].linked_prev ? (p->slur ? sgn[k - 1] : -sgn[k - 1]) : 1;

    /* ---- pass 2: bow contours */
    for (int k = 0; k < n; k++) {
        NotePlan *q = &pl[k];
        const Note *nn = &nt[k];
        double ts = q->t_s, te = q->t_e, dur = te - ts;
        double tbp = q->linked_prev ? 0.5 * (nt[k - 1].start + nt[k - 1].dur + ts) : 0.0, Wp = q->linked_prev ? Wk[k] : 0.0;
        double tbn = q->linked_next ? 0.5 * (te + nt[k + 1].start) : 0.0, Wn = q->linked_next ? Wk[k + 1] : 0.0;
        long ia = (long)floor((ts - 0.12) * R), ib = (long)ceil((te + 0.12) * R);
        if (ia < 0) ia = 0;
        if (ib > N - 1) ib = N - 1;
        long m = ib - ia + 1;
        double *db = malloc(sizeof(double) * (size_t)m * 3);
        double *dv = db + m, *df = db + 2 * m;
        Dr d;
        dr_open(&d, p, cnt, k, PU_BETA);
        bl_noise(&d, db, m, R, sc[k].bwb, sc[k].sdb);
        dr_open(&d, p, cnt, k, PU_SPEED);
        bl_noise(&d, dv, m, R, sc[k].bwv, 0.03);
        dr_open(&d, p, cnt, k, PU_FORCE);
        bl_noise(&d, df, m, R, sc[k].bwf, 0.05);
        double lead = sc[k].lead, Tacc = sc[k].dacc;
        const int att = expert && p->att_on && !q->linked_prev;
        double T0 = 0.0, Tsw = sc[k].tsw, r0 = p->att_v_frac, Trel = sc[k].trel, F_att = 0.0;
        if (att) {
            const double a_w = p->att_acc > 0.0 ? p->att_acc : I->att_acc[q->string];
            F_att = (p->att_k > 0.0 ? p->att_k : I->att_k[q->string]) * a_w; /* the wedge: the force proportional to the acceleration */
            T0 = r0 * q->vB0 / a_w;
            Tacc = T0 + Tsw;
            lead = 0.02;
            q->T_acc = Tacc;
        }
        q->t_sus0 = q->linked_prev ? tbp + 0.5 * Wp : ts + Tacc;
        q->t_sus1 = q->linked_next ? tbn - 0.5 * Wn : te - 0.04;
        for (long i = ia; i <= ib; i++) {
            double t = (double)i / R, u = (t - ts) / dur, uc = clampd(u, 0.0, 1.0);
            double w = 1.0;
            if (q->linked_prev) w *= rc((t - (tbp - 0.5 * Wp)) / Wp);
            if (q->linked_next) w *= 1.0 - rc((t - (tbn - 0.5 * Wn)) / Wn);
            if (w <= 0.0) continue;
            double beta, vplat, F;
            double ar = expert ? 0.08 * (2.0 * hump(uc, sc[k].pk) - 1.0) : 0.0;
            if (expert) {
                beta = clampd(q->beta0 * (1.0 + db[i - ia]), 0.07, 0.14);
                vplat = q->vB0 * (1.0 + ar) * (1.0 + dv[i - ia]);
            } else {
                beta = 0.1;
                vplat = q->vB0;
            }
            double prof = 1.0, sw = 1.0;
            if (att) { /* the attack profile of the speed, 0 at the motion start; sw is the swell from the reduced speed to the plateau */
                double ua = t - ts;
                sw = rc((ua - T0) / Tsw);
                prof = ua <= 0.0 ? 0.0 : ua < T0 ? r0 * ua / T0 : r0 + (1.0 - r0) * sw;
                vplat *= prof;
            }
            double lo, hi;
            band_or_anchor(I, q->string, beta, vplat, &lo, &hi);
            double D = log(hi / lo), lnF;
            double frac_t = att ? p->att_frac + (q->frac0 - p->att_frac) * sw : q->frac0;
            if (expert) {
                lnF = log(lo) + frac_t * D + log(1.0 + ar) + log(1.0 + df[i - ia]);
                lnF = clampd(lnF, log(lo) + 0.2 * D, log(lo) + 0.8 * D);
            } else lnF = log(lo) + q->frac0 * D;
            if (att) { /* the Guettler wedge: F = k a while the bow accelerates, then the force relaxes to the sustain force over Trel (raised cosine in ln F, no
                          step); the 0.2 - 0.8 clamp above is the sustain force, it governs the force from the end of the relaxation on */
                double ua = t - ts, wr = rc((ua - T0 - p->att_hold) / Trel);
                if (wr < 1.0) lnF = (1.0 - wr) * (log(F_att) + log(1.0 + df[i - ia])) + wr * lnF;
            }
            F = exp(lnF);
            double V = vplat, fa = 1.0, fr = 1.0, va = 1.0, vr = 1.0;
            if (!q->linked_prev) {
                if (att) {
                    va = 1.0; /* the profile is in V already */
                    double tf = ts - lead - p->f_ramp;
                    fa = t < tf ? 0.0 : rc((t - tf) / p->f_ramp);
                } else if (expert) {
                    va = t < ts ? 0.0 : t >= ts + Tacc ? 1.0 : (t - ts) / Tacc;
                    double t0 = ts - lead - 0.02;
                    if (t < t0) fa = 0.0;
                    else if (t < ts - lead) fa = 0.8 * rc((t - t0) / 0.02);
                    else if (t < ts + Tacc) fa = 0.8 + 0.2 * (t - (ts - lead)) / (lead + Tacc);
                } else {
                    va = t < ts ? 0.0 : t >= ts + 0.01 ? 1.0 : (t - ts) / 0.01;
                    fa = t < ts - 0.001 ? 0.0 : t >= ts ? 1.0 : (t - (ts - 0.001)) / 0.001;
                }
            }
            if (!q->linked_next) {
                if (expert) {
                    double tl = te - 0.04;
                    fr = t < tl ? 1.0 : 1.0 - rc((t - tl) / 0.04);
                    vr = t < tl ? 1.0 : 1.0 - rc((t - tl) / 0.06);
                } else {
                    vr = t < te ? 1.0 : t >= te + 0.01 ? 0.0 : 1.0 - (t - te) / 0.01;
                    fr = t < te + 0.15 ? 1.0 : t >= te + 0.151 ? 0.0 : 1.0 - (t - (te + 0.15)) / 0.001;
                }
            }
            T->vB[i] += w * sgn[k] * V * va * vr;
            T->FB[i] += w * F * fa * fr;
            bacc[i] += w * beta;
            sacc[i] += w * beta / q->ratio;
            wsum[i] += w;
        }
        free(db);
        /* the string under the bow: from the middle of the bow change (a new bow: when the force starts) on */
        long isw = q->linked_prev ? (long)floor(tbp * R) : (long)floor((ts - lead - 0.02) * R);
        if (isw < 0) isw = 0;
        for (long i = isw; i < N; i++) T->bowed[i] = (signed char)q->string;
        if (k == 0) for (long i = 0; i < isw && i < N; i++) T->bowed[i] = (signed char)q->string;
        (void)nn;
    }
    /* force dip through each bow change (expert) */
    if (expert)
        for (int k = 0; k + 1 < n; k++)
            if (pl[k].linked_next) {
                double tb = 0.5 * (pl[k].t_e + nt[k + 1].start), W = Wk[k + 1];
                for (long i = (long)floor((tb - 0.5 * W) * R); i <= (long)ceil((tb + 0.5 * W) * R) && i < N; i++) {
                    double u = ((double)i / R - (tb - 0.5 * W)) / W;
                    if (u < 0.0 || u > 1.0 || i < 0) continue;
                    T->FB[i] *= 1.0 - (1.0 - p->dip) * 0.5 * (1.0 - cos(2.0 * M_PI * u));
                }
            }
    {
        double last = 0.1, lastr = 0.1;
        for (long i = 0; i < N; i++) {
            if (wsum[i] > 1e-9) { last = sacc[i] / wsum[i]; lastr = bacc[i] / wsum[i]; }
            T->beta[i] = last;
            out->beta_rel[i] = lastr;
        }
    }
    free(wsum);
    free(bacc);
    free(sacc);

    /* ---- pass 3: fingers */
    for (int k = 0; k < n; k++) {
        NotePlan *q = &pl[k];
        const Note *nn = &nt[k];
        int s = q->string;
        double L = I->str[s].L, ts = q->t_s, te = q->t_e;
        double xnom = 0.0;
        int stopped = q->semis > 0;
        if (stopped) {
            double fo = I->str[s].f0, fn = midi_hz(nn->midi);
            xnom = L * (1.0 - fo / fn);
            if (q->semis < INSTR_MAXSEMI) xnom = sim_vib_finger(L, xnom, I->stop_cents[s][q->semis]);
        }
        int same_swap = k > 0 && pl[k - 1].string == s && q->linked_prev && pl[k - 1].semis > 0;
        double tL = expert ? ts - p->land_lead : ts;
        if (expert && nn->slide && same_swap) tL = ts - 0.10;
        long iL = (long)ceil(tL * R - 1e-9);
        if (iL < 0) iL = 0;
        long ie = (long)floor(te * R);
        if (!stopped) { /* open string: the finger of any earlier note on this string lifts */
            for (long i = iL; i < N; i++) {
                double u = ((double)i / R - tL) / (expert ? p->land_ramp : 0.001);
                T->aF[s][i] *= 1.0 - rc(u);
            }
            continue;
        }
        if (expert && p->predamp && !same_swap) {
            int j = k - 1;
            while (j >= 0 && pl[j].string != s) j--;
            if (j >= 0 && pl[j].semis == 0) {
                double td = pl[j].t_e + p->predamp_delay;
                if (td + p->predamp_ease < tL) {
                    for (long i = (long)ceil(td * R - 1e-9); i < iL && i < N; i++) {
                        T->xF[s][i] = xnom;
                        T->aF[s][i] = p->predamp_press * rc((i / R - td) / p->predamp_ease);
                    }
                }
            }
        }
        double xold = T->xF[s][iL];
        long m = N - iL;
        Dr d;
        double *dp = calloc((size_t)m, sizeof(double));
        dr_open(&d, p, cnt, k, PU_PITCH);
        if (expert) bl_noise(&d, dp, m, R, 0.5, 2.0);
        /* vibrato cycles: one gaussian pair per cycle, the number of cycles from the note length only */
        int ncyc = (int)(nn->dur * 6.5) + 4;
        double *Tc = malloc(sizeof(double) * (size_t)ncyc * 2);
        double *Ac = Tc + ncyc;
        {
            Dr dv;
            dr_open(&dv, p, cnt, k, PU_VIB);
            for (int j = 0; j < ncyc; j++) {
                Tc[j] = 0.115 * d_gauss(&dv);
                Ac[j] = clampd(1.0 + 0.2 * d_gauss(&dv), 0.4, 1.6);
            }
            long nd = (long)((nn->dur + 0.5) * R) + 2;
            double *drv = malloc(sizeof(double) * (size_t)nd);
            dr_open(&dv, p, cnt, k, PU_VIBDRIFT);
            bl_noise(&dv, drv, nd, R, 1.0, 0.04);
            /* turn the cycle draws into durations using the rate drift at the cycle start */
            double tc = 0.0;
            for (int j = 0; j < ncyc; j++) {
                long ii = (long)(tc * R);
                if (ii >= nd) ii = nd - 1;
                double Tj = (1.0 / q->rate_hz) * clampd(1.0 + Tc[j], 0.5, 1.6) * (1.0 + drv[ii]);
                Tc[j] = Tj;
                tc += Tj;
            }
            free(drv);
        }
        double tvend = te;
        for (long i = iL; i < N; i++) {
            double t = (double)i / R;
            double c = 0.0, rock = 0.0;
            if (expert) {
                if (!nn->slide || !same_swap) c -= q->scoop_c * (1.0 - rc((t - tL) / sc[k].settle));
                c += dp[i - iL];
                if (q->vib_on && t >= q->t_vib0 && t <= tvend) {
                    double tau = t - q->t_vib0, tc = 0.0, env = rc(tau / 0.3);
                    int j = 0;
                    while (j < ncyc - 1 && tau >= tc + Tc[j]) { tc += Tc[j]; j++; }
                    double ph = 2.0 * M_PI * (tau - tc) / Tc[j];
                    c += q->depth_c * env * (Ac[j] * sin(ph) - 0.4); /* below-and-around the pitch: the mean sits 0.4 D flat (ASSUMED) */
                    rock = p->rock * (1.0 + cos(ph)) * env;             /* press 0.9 to 1.0 */
                }
            } else if (q->vib_on && t >= ts && t <= tvend) {
                c += (nn->depth_c >= 0.0 ? nn->depth_c : 20.0) * sin(2.0 * M_PI * 6.5 * (t - ts));
            }
            double a;
            if (t <= te) {
                a = 1.0 - rock;
                if (same_swap && expert && !nn->slide) { /* the old finger lifts and the new one lands within 10 ms: a dips, the position swaps in the middle */
                    double u = (t - tL) / p->land_ramp;
                    if (u >= -1e-6 && u <= 1.0) a *= 1.0 - p->swap_dip * sin(M_PI * u);
                } else {
                    double up = rc((t - tL) / (expert ? p->land_ramp : 0.001));
                    if (!(nn->slide && same_swap)) a = fmax(a * up, up < 1.0 ? T->aF[s][i] : 0.0);
                }
            } else if (expert) {
                /* the left hand of the last note of a phrase keeps the note stopped while it rings and lets go slowly (the finger lift after the bow
                   lift re-excites the string: 13-17 dB rises 0.1-0.2 s after the lift when it eases off at once, release_study.py); a legato change keeps the
                   old quick damping of the old string */
                double delay = q->linked_next ? 0.0 : p->damp_delay, slow = q->linked_next ? 1.0 : p->damp_ease;
                double tt = t - te - delay, ease = rc(tt / (slow * Tdamp[k]));
                a = damp_a[k] + (1.0 - damp_a[k]) * (1.0 - ease);
                if (tt > 0.5) a *= 1.0 - rc((tt - 0.5) / 0.05);
            } else a = t < te + 0.001 ? 1.0 - (t - te) / 0.001 : 0.0;
            if (a < 0.0) a = 0.0;
            double x;
            if (t > te) x = T->xF[s][ie < N ? ie : N - 1];
            else if (nn->slide && same_swap && expert) {
                double xn = sim_vib_finger(L, xnom, c);
                x = xold + (xn - xold) * rc((t - tL) / 0.12);
            } else if (same_swap && expert && (t - tL) / 0.01 < 0.5) x = xold;
            else x = sim_vib_finger(L, xnom, c);
            T->xF[s][i] = x;
            T->aF[s][i] = a;
            if (t <= te + 1e-12) out->cents[i] = c;
            else if (i > ie) out->cents[i] = 0.0;
        }
        free(dp);
        free(Tc);
    }
    return 0;
}

int played_write_trace(const Played *pl, const Instrument *I, const char *prefix)
{
    char path[1024];
    snprintf(path, sizeof path, "%s_ctrl.f32", prefix);
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    const PlayTracks *T = &pl->trk;
    for (long i = 0; i < T->n; i++) {
        int b = T->bowed[i];
        double vb = fabs(T->vB[i]), lo, hi;
        band_or_anchor(I, b, pl->beta_rel[i], vb < 0.05 ? 0.05 : vb, &lo, &hi);
        float row[11] = {(float)T->vB[i], (float)T->FB[i], (float)T->beta[i], (float)pl->beta_rel[i], (float)b, (float)T->aF[b][i], (float)T->xF[b][i],
                         (float)pl->cents[i], (float)lo, (float)hi, (float)(T->FB[i] > 0 ? log(T->FB[i] / lo) / log(hi / lo) : -1.0)};
        fwrite(row, sizeof(float), 11, f);
    }
    fclose(f);
    snprintf(path, sizeof path, "%s_ctrl.txt", prefix);
    f = fopen(path, "w");
    if (!f) return 1;
    fprintf(f, "rate=%.0f n=%ld columns=vB,FB,beta_sim,beta_rel,string,aF,xF,cents,Fmin,Fmax,frac (float32 LE, 11 per row; Fmin Fmax at the table, frac = log-band position)\n", T->rate, T->n);
    fprintf(f, "# note t_s t_e string semis linked_prev linked_next vib depth_c rate_hz t_vib0 beta0 vB0 frac0 t_sus0 t_sus1 scoop_c T_acc ratio\n");
    for (int k = 0; k < pl->nnotes; k++) {
        const NotePlan *q = &pl->plan[k];
        fprintf(f, "%d %.4f %.4f %d %d %d %d %d %.3f %.3f %.4f %.4f %.4f %.4f %.4f %.4f %.3f %.4f %.4f\n", k, q->t_s, q->t_e, q->string, q->semis,
                q->linked_prev, q->linked_next, q->vib_on, q->depth_c, q->rate_hz, q->t_vib0, q->beta0, q->vB0, q->frac0, q->t_sus0, q->t_sus1,
                q->scoop_c, q->T_acc, q->ratio);
    }
    fclose(f);
    return 0;
}
