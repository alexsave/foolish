// models.c - see models.h. Each function follows its Python line for line;
// the comments name the Python's own variables where the port is not obvious.
#include "models.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tables.h"

#define PI_D 3.14159265358979323846
#define NB 40                                    // bands in every measured template (CS40)
#define S_(sec) ((int)((sec) * SR))
static double db2(double db) { return pow(10, db / 20); }
static double clip(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
static double maxd(double a, double b) { return a > b ? a : b; }

// A template plus a per-band jitter of +-j dB (the models' `+ rng.uniform(-j, j, 40)`).
static void jitter(Rng *r, double *t, int n, double j) { for (int i = 0; i < n; i++) t[i] += uni(r, -j, j); }
// np.where(cs < split, tmpl, other) and its mirror.
static void below(const double *tmpl, double split, double other, double *out) {
    for (int i = 0; i < NB; i++) out[i] = CS40[i] < split ? tmpl[i] : other;
}
static void above(const double *tmpl, double split, double other, double *out) {
    for (int i = 0; i < NB; i++) out[i] = CS40[i] >= split ? tmpl[i] : other;
}
// eq(..) at the models' usual cuts: 120 Hz and 15 kHz.
static void eq40(Sig *y, const double *t) { eq(y, CS40, t, NB, 120, 15000); }
static Sig cnoise40(Rng *r, int n, const double *t) { return colored_noise(r, n, CS40, t, NB, 120, 15000); }
// x / rms(x): the models' `b / np.sqrt(np.mean(b ** 2))`.
static void unit_rms(Sig *s) { const double q = sig_rms(s->x, s->n); if (q > 0) sig_scale(s, 1 / q); }
// A dB curve over time: interp(tt, T * ts, D + jitter, left, right) into env (linear).
static void curve(double *env, int n, double t0, const double *T, const double *D, int k, double ts,
                  double j, double edge, Rng *r) {
    double tt[64], dd[64];
    for (int i = 0; i < k; i++) { tt[i] = T[i] * ts; dd[i] = D[i] + uni(r, -j, j); }
    for (int i = 0; i < n; i++) env[i] = db2(interp((double)i / SR - t0, tt, dd, k, edge, edge));
}
// The two-pole table mode of the throw-in, the cover and land229: a resonator at
// `fm`, `bw` wide, excited by the take's 150 Hz-15 kHz, set `mode_db` over its
// neighbours' level from +10 to +90 ms.
static void table_mode(Sig *y, double t0, double fm, double bw, double mode_db) {
    Sig ex = bp_copy(y, 150, 15000);
    Sig md = reson(&ex, fm, 0, bw);
    const int a = S_(t0 + 0.01), b = S_(t0 + 0.09);
    Sig n1 = bp_copy(y, 170, 205), n2 = bp_copy(y, 255, 290), m1 = bp_copy(&md, 215, 245);
    double qn = 0, qm = 0;
    for (int i = a; i < b; i++) { const double v = n1.x[i] + n2.x[i]; qn += v * v; qm += m1.x[i] * m1.x[i]; }
    const double nb = sqrt(qn / (b - a) / (70.0 / 30)), mm = sqrt(qm / (b - a)) + 1e-12;
    sig_add(y, &md, nb * db2(mode_db) / mm);
    sig_free(&ex); sig_free(&md); sig_free(&n1); sig_free(&n2); sig_free(&m1);
}
// Scale so the 150 Hz-15 kHz peak (or `lo` Hz up) sits at `db`: the models' last line.
static void norm_band(Sig *y, double lo, double db) {
    Sig b = bp_copy(y, lo, 15000);
    const double p = sig_peak(b.x, b.n) + 1e-12;
    sig_free(&b);
    sig_scale(y, db2(db) / p);
}
static void norm_peak(Sig *y, double db) { sig_scale(y, db2(db) / (sig_peak(y->x, y->n) + 1e-12)); }

// ---- deal/an/thumb2.py: thumb9 ---------------------------------------------
static const double ENV_T[18] = { -150, -120, -100, -80, -60, -40, -20, -8, 0, 8, 20, 40, 60, 80, 100, 120, 160, 200 };
static const double ENV_D[18] = { -30, -23.1, -18.8, -12.2, -10.5, -7.7, -6.7, -5.5, 0.0, -3.0, -5.2, -8.9, -14.6, -16.8, -22.4, -25.6, -34.0, -42.0 };
static const double LOW_V[3] = { 2050, 1000, 1850 }, HIGH_V[3] = { 2200, 2950, 3200 };
static Sig track_res(Rng *r, const double *f, int n, double bw) {
    Sig x = noise(r, n);
    Sig y = reson(&x, 0, f, bw);
    sig_free(&x);
    const double s = sig_rms(y.x, n);          // std of a zero-mean resonance
    sig_scale(&y, 1 / (s + 1e-12));
    return y;
}
Sig m_thumb(Rng *r) {
    double ts[18], dd[18];
    const double k = uni(r, 0.85, 1.15);
    for (int i = 0; i < 18; i++) { ts[i] = ENV_T[i] / 1000 * k; dd[i] = ENV_D[i] + uni(r, -1.5, 1.5); }
    dd[8] = 0;
    const double sp = uni(r, -0.008, 0.008);
    for (int i = 7; i < 10; i++) ts[i] += sp;
    const int n = S_(ts[17] - ts[0] + 0.01);
    double *env = malloc(sizeof(double) * (size_t)n), emax = 0;
    for (int i = 0; i < n; i++) { env[i] = db2(interp((double)i / SR + ts[0], ts, dd, 18, -60, -60)); emax = maxd(emax, env[i]); }
    // the stick-slip wobble: noise low-passed at 15-50 Hz, 12% deep
    Sig wob = noise(r, n);
    bp(&wob, 0, uni(r, 15, 50));
    const double wsd = sig_rms(wob.x, n);
    // the resonances' V and rise, timed against the audible part (within 20 dB of the peak)
    int a0 = -1, a1 = 0;
    for (int i = 0; i < n; i++) if (env[i] > emax * 0.1) { if (a0 < 0) a0 = i; a1 = i; }
    const double ls = LOW_V[0] * uni(r, 0.92, 1.08), lm = LOW_V[1] * uni(r, 0.92, 1.08), le = LOW_V[2] * uni(r, 0.92, 1.08);
    const double hs = HIGH_V[0] * uni(r, 0.92, 1.08), hm = HIGH_V[1] * uni(r, 0.92, 1.08), he = HIGH_V[2] * uni(r, 0.92, 1.08);
    double *lo = malloc(sizeof(double) * (size_t)n), *hi = malloc(sizeof(double) * (size_t)n);
    for (int i = 0; i < n; i++) {
        const double u = clip((double)(i - a0) / (a1 - a0 > 1 ? a1 - a0 : 1), 0, 1);
        lo[i] = u < 0.5 ? lm + (ls - lm) * (1 - 2 * u) * (1 - 2 * u) : lm + (le - lm) * (2 * u - 1) * (2 * u - 1);
        hi[i] = u < 0.5 ? hs + (hm - hs) * 2 * u : hm + (he - hm) * (2 * u - 1);
    }
    Sig r1 = track_res(r, lo, n, uni(r, 160, 260)), r2 = track_res(r, hi, n, uni(r, 450, 700));
    Sig hiss = colored_noise(r, n, THUMB_CS, THUMB_TMPL, THUMB_CS_N, 120, 0);
    const double hsd = sig_rms(hiss.x, n) + 1e-12;
    const double g1 = uni(r, 0.33, 0.48), g2 = uni(r, 0.18, 0.3), gr = uni(r, 0.4, 0.6);
    const int pad = S_(0.02);
    Sig y = sig_new(n + 2 * pad);
    for (int i = 0; i < n; i++) {
        const double w = 1 + 0.12 * wob.x[i] / (wsd + 1e-9);
        y.x[pad + i] = (hiss.x[i] / hsd * 1.45 + (g1 * r1.x[i] + g2 * r2.x[i]) * gr) * env[i] * maxd(w, 0.1);
    }
    norm_peak(&y, uni(r, -41.5, -35.5));
    free(env); free(lo); free(hi);
    sig_free(&wob); sig_free(&r1); sig_free(&r2); sig_free(&hiss);
    return y;
}

// ---- deal/an/grab2.py --------------------------------------------------------
static const double GBT[13] = { -0.045, -0.008, -0.003, 0.003, 0.008, 0.015, 0.025, 0.040, 0.060, 0.080, 0.100, 0.130, 0.170 };
static const double GBL[13] = { -32, -27.4, -23.9, -19.1, -17.8, -20.2, -29.0, -28.4, -26.1, -29.5, -31.4, -34, -40 };
static const double GBHT[13] = { -0.045, -0.008, -0.003, 0.005, 0.010, 0.020, 0.030, 0.040, 0.060, 0.080, 0.100, 0.130, 0.160 };
static const double GBH[13] = { -40, -36, -30, -18, -22, -24, -27, -29, -34, -39, -44, -49, -55 };
// A two-band bed at `scale`: lows (under 1.5 kHz) on el, highs on eh, each noise in its half of `tmpl`.
static void bed2(Rng *r, Sig *y, const double *tmpl, const double *el, const double *eh, double scale) {
    double lt[NB], ht[NB];
    below(tmpl, 1500, -60, lt); above(tmpl, 1500, -60, ht);
    Sig b1 = cnoise40(r, y->n, lt), b2 = cnoise40(r, y->n, ht);
    unit_rms(&b1); unit_rms(&b2);
    for (int i = 0; i < y->n; i++) y->x[i] += (b1.x[i] * el[i] + b2.x[i] * eh[i]) * scale;
    sig_free(&b1); sig_free(&b2);
}
Sig m_grab(Rng *r) {
    const double t0 = 0.05; const int n = S_(0.22);
    Sig cl = sig_new(n);
    burst_add(r, &cl, t0, 0, uni(r, 0.0007, 0.001), 0.00005, 2.5);
    for (int k = irange(r, 3, 8); k > 0; k--) {
        const double t = uni(r, -0.040, -0.004), d = uni(r, -26, -12), w = uni(r, 0.0012, 0.003);
        burst_add(r, &cl, t0 + t, d, w, 0.00005, 2.5);
    }
    {
        const double s = uni(r, 0, 1) < 0.5 ? -1 : 1, t = s * uni(r, 0.001, 0.007), d = uni(r, -9, -1.5), w = uni(r, 0.0008, 0.0025);
        burst_add(r, &cl, t0 + t, d, w, 0.00005, 2.5);
    }
    for (int k = irange(r, 0, 3); k > 0; k--) {
        const double t = uni(r, 0.015, 0.030), d = uni(r, -25, -3), w = uni(r, 0.0012, 0.003);
        burst_add(r, &cl, t0 + t, d, w, 0.00005, 2.5);
    }
    eq40(&cl, DEAL_CLICK);
    const double pc = sig_peak(cl.x + S_(t0) - 48, 144);
    double *el = malloc(sizeof(double) * n), *eh = malloc(sizeof(double) * n);
    const double ts = uni(r, 0.85, 1.15);
    curve(el, n, t0, GBT, GBL, 13, ts, 2, -80, r);
    curve(eh, n, t0, GBHT, GBH, 13, ts, 1.5, -80, r);
    bed2(r, &cl, DEAL_BED, el, eh, pc * 0.5);
    norm_peak(&cl, uni(r, -37, -24));
    free(el); free(eh);
    return cl;
}

// ---- deal/an/land_syn.py land229 2 ------------------------------------------
static const double LT[15] = { -0.012, -0.008, -0.003, 0.003, 0.006, 0.010, 0.015, 0.020, 0.030, 0.040, 0.060, 0.080, 0.100, 0.130, 0.170 };
static const double LL[15] = { -30, -16, -14, -6, -10, -12, -18.4, -15.3, -16.0, -17.6, -21.9, -23.1, -22, -21, -27 };
static const double LH[15] = { -34, -24, -18, -13, -19, -23, -25, -28, -30, -32, -36, -38, -40, -44, -50 };
Sig m_land(Rng *r) {
    const int n = S_(0.26); const double t0 = 0.05;
    Sig con = sig_new(n);
    burst_add(r, &con, t0, 0, uni(r, 0.002, 0.003), 0.0003, 2.5);
    { const double t = uni(r, -0.0012, -0.0006), d = uni(r, -2, 0), w = uni(r, 0.002, 0.003); burst_add(r, &con, t0 + t, d, w, 0.0003, 2.5); }
    { const double t = uni(r, 0.005, 0.015), d = uni(r, -8, -4), w = uni(r, 0.0015, 0.0025); burst_add(r, &con, t0 + t, d, w, 0.0003, 2.5); }
    if (uni(r, 0, 1) < 0.7) { const double t = uni(r, -0.025, -0.008), d = uni(r, -20, -9), w = uni(r, 0.0015, 0.0025); burst_add(r, &con, t0 + t, d, w, 0.0003, 2.5); }
    double t[NB];
    for (int i = 0; i < NB; i++) t[i] = LAND4_CLICK[i] + (CS40[i] < 320 ? 2.5 : 0);
    jitter(r, t, NB, 2);
    eq40(&con, t);
    const double pc = sig_peak(con.x + S_(t0) - 96, 240);
    double *el = malloc(sizeof(double) * n), *eh = malloc(sizeof(double) * n);
    const double ts = uni(r, 0.85, 1.15);
    curve(el, n, t0, LT, LL, 15, ts, 2, -80, r);
    curve(eh, n, t0, LT, LH, 15, ts, 2, -80, r);
    bed2(r, &con, LAND4_BED, el, eh, pc * 0.5);
    table_mode(&con, t0, 229.0, 6.0, 2.0);
    norm_peak(&con, uni(r, -40, -35));
    free(el); free(eh);
    return con;
}

// ---- throwin/an/throw_syn.py and cover/an/cov_syn.py -------------------------
// One synth with two sets of tables, as the cover's Python is the throw-in's
// with every table re-measured.
static const double TBT[18] = { -20, -8, -3, 0, 3, 6, 10, 15, 20, 30, 40, 60, 80, 100, 130, 170, 220, 280 };
static const double TBL[18] = { -40, -11, -1, 2, 3, 0, -7, -10, -11, -12, -17, -21, -25, -24, -29, -31, -38, -44 };
static const double TBH[18] = { -45, -24, -20, -20, -20.3, -18.1, -19, -24, -29, -36, -33, -37, -40, -42, -47, -50, -52, -57 };
static const double CBL[18] = { -34.5, -29.5, -18, -14, -14.2, -10.3, -16.3, -18.9, -19.0, -21.8, -29.7, -30.9, -30.3, -34.0, -42.0, -40.2, -46.6, -51.4 };
static const double CBH[18] = { -36.3, -21.1, -15.0, -20.6, -25.5, -24.2, -31.8, -35.1, -37.0, -39.1, -41.4, -41.0, -43.8, -47.5, -50.2, -53.8, -58.0, -60 };
static Sig landing(Rng *r, int cover) {
    const int n = S_(0.40); const double t0 = 0.06, RISE = 0.0001;
    const double *SS = cover ? COV_STRIKE : TIN_STRIKE, *BS = cover ? COV_BED : TIN_BED, *FIT = cover ? COV_FIT : TIN_FIT;
    const double STILT = cover ? 0 : 3, MODE = cover ? 3.3 : 4.5;
    Sig st = sig_new(n);
    if (!cover) {
        burst_add(r, &st, t0, 0, uni(r, 0.0015, 0.0035), RISE, 3);
        { const double t = -uni(r, 0.005, 0.014), d = uni(r, -8, -2), w = uni(r, 0.0025, 0.0035); burst_add(r, &st, t0 + t, d, w, RISE, 3); }
        if (uni(r, 0, 1) < 0.5) { const double t = -uni(r, 0.0015, 0.014), d = uni(r, -17, -5), w = uni(r, 0.0015, 0.0035); burst_add(r, &st, t0 + t, d, w, RISE, 3); }
        if (uni(r, 0, 1) < 0.5) { const double t = uni(r, 0.001, 0.003), d = uni(r, -10, -5), w = uni(r, 0.002, 0.0035); burst_add(r, &st, t0 + t, d, w, RISE, 3); }
        if (uni(r, 0, 1) < 0.33) { const double t = uni(r, 0.013, 0.035), d = uni(r, -25, -19), w = uni(r, 0.0015, 0.0028); burst_add(r, &st, t0 + t, d, w, RISE, 3); }
    } else {
        burst_add(r, &st, t0, 0, uni(r, 0.0012, 0.0025), RISE, 3);
        if (uni(r, 0, 1) < 5.0 / 9) { const double t = uni(r, 0.0009, 0.003), d = uni(r, -11, -5), w = uni(r, 0.0022, 0.0032); burst_add(r, &st, t0 + t, d, w, RISE, 3); }
        if (uni(r, 0, 1) < 4.0 / 9) { const double t = -uni(r, 0.002, 0.0085), d = uni(r, -11, -1), w = uni(r, 0.0015, 0.003); burst_add(r, &st, t0 + t, d, w, RISE, 3); }
        if (uni(r, 0, 1) < 4.0 / 9) { const double t = uni(r, 0.0056, 0.013), d = uni(r, -25, -3), w = uni(r, 0.0013, 0.0028); burst_add(r, &st, t0 + t, d, w, RISE, 3); }
    }
    double top[NB], t[NB];
    for (int i = 0; i < NB; i++) top[i] = -clip(log2(CS40[i] / 7000) * 18, 0, 30);
    for (int i = 0; i < NB; i++) t[i] = SS[i] + top[i] - STILT * maxd(log2(CS40[i] / 1000), 0);
    jitter(r, t, NB, 1.5);
    eq40(&st, t);
    // the slap: a short noise burst as the face meets the cloth
    {
        const int L = S_(0.03), k0 = S_(t0) - 24;
        const double dec = uni(r, 0.003, 0.005);
        Sig sl = sig_new(n);
        for (int i = 0; i < L; i++) { const double q = (double)i / SR; sl.x[k0 + i] = normal(r) * fmin(1, q / 0.0008) * exp(-q / dec); }
        for (int i = 0; i < NB; i++) t[i] = SS[i] + top[i] - 10 * maxd(log2(CS40[i] / 700), 0);
        eq40(&sl, t);
        sig_add(&st, &sl, sig_peak(st.x, n) / (sig_peak(sl.x, n) + 1e-12) * db2(-6));
        sig_free(&sl);
    }
    Sig hs = bp_copy(&st, 3000, 15000);
    const double ref = max_moving_rms(&hs, 12, 0, n);
    sig_free(&hs);
    double *el = malloc(sizeof(double) * n), *eh = malloc(sizeof(double) * n), T[18];
    for (int i = 0; i < 18; i++) T[i] = TBT[i] / 1000;
    const double ts = uni(r, 0.85, 1.15);
    curve(el, n, t0, T, cover ? CBL : TBL, 18, ts, 1.5, -80, r);
    curve(eh, n, t0, T, cover ? CBH : TBH, 18, ts, 1.5, -80, r);
    double lt[NB], ht[NB];
    below(BS, 1500, -60, lt); above(BS, 1500, -60, ht);
    Sig b1 = cnoise40(r, n, lt), b2 = cnoise40(r, n, ht);
    bp(&b1, 150, 1500); bp(&b2, 1500, 15000);
    unit_rms(&b1); unit_rms(&b2);
    for (int i = 0; i < n; i++) st.x[i] += (b1.x[i] * el[i] + b2.x[i] * eh[i]) * ref;
    sig_free(&b1); sig_free(&b2); free(el); free(eh);
    table_mode(&st, t0, cover ? uni(r, 226, 234) : uni(r, 222, 238), 6.0, MODE);
    eq40(&st, FIT);
    norm_band(&st, 150, cover ? uni(r, -16.6, -1.5) : uni(r, -21, -10));
    return st;
}
Sig m_throw(Rng *r) { return landing(r, 0); }
Sig m_cover(Rng *r) { return landing(r, 1); }

// ---- throwin/an/tgrab_syn.py tgrab2 9 ---------------------------------------
static const double TGT[16] = { -0.030, -0.015, -0.008, -0.004, -0.002, 0.0, 0.002, 0.004, 0.008, 0.012, 0.020, 0.030, 0.050, 0.080, 0.130, 0.200 };
static const double TGL[16] = { -25.9, -22.3, -19.1, -11.2, -4.3, -2.9, -1.7, -2.6, -7.6, -12.7, -16.4, -19.6, -19.9, -22.0, -30, -45 };
static const double TGH[16] = { -35.7, -34.8, -19.5, -12.0, -7.2, -6.9, -7.9, -15.7, -24.0, -25.3, -26.0, -28.0, -29.0, -34.7, -42, -55 };
Sig m_tgrab(Rng *r) {
    const int N = S_(0.5), P0 = S_(TGRAB_REF_S); const double t0 = TGRAB_REF_S;
    Sig con = sig_new(N);
    burst_add(r, &con, t0, 0, uni(r, 0.00075, 0.0019), 0.0002, 3);
    if (uni(r, 0, 1) < 7.0 / 8) { const double t = -uni(r, 0.0026, 0.011), d = uni(r, -12, 0), w = uni(r, 0.0016, 0.0027); burst_add(r, &con, t0 + t, d, w, 0.0002, 3); }
    if (uni(r, 0, 1) < 0.5) { const double t = uni(r, 0.0015, 0.0018), d = uni(r, -12, -9), w = uni(r, 0.0029, 0.0034); burst_add(r, &con, t0 + t, d, w, 0.0002, 3); }
    double t[NB];
    memcpy(t, TGRAB_SPEC, sizeof t); jitter(r, t, NB, 1.5);
    eq40(&con, t);
    double *el = malloc(sizeof(double) * N), *eh = malloc(sizeof(double) * N);
    const double ts = uni(r, 0.85, 1.15);
    curve(el, N, t0, TGT, TGL, 16, ts, 2, -90, r);
    curve(eh, N, t0, TGT, TGH, 16, ts, 2, -90, r);
    double lt[NB], ht[NB];
    below(TGRAB_SPEC, 1500, -60, lt); above(TGRAB_SPEC, 1500, -60, ht);
    Sig lo = cnoise40(r, N, lt), hi = cnoise40(r, N, ht);
    bp(&lo, 150, 1500); bp(&hi, 1500, 15000); unit_rms(&lo); unit_rms(&hi);
    Sig bed = sig_new(N);
    for (int i = 0; i < N; i++) bed.x[i] = lo.x[i] * el[i] + hi.x[i] * eh[i];
    const double ref = max_moving_rms(&bed, 240, P0 - 96, P0 + 96);
    sig_scale(&con, ref * db2(3.0) / (max_moving_rms(&con, 240, P0 - 96, P0 + 96) + 1e-12));
    sig_add(&bed, &con, 1);
    eq40(&bed, TGRAB_FIT);
    norm_band(&bed, 150, uni(r, -40.2, -25.9));
    sig_free(&con); sig_free(&lo); sig_free(&hi); free(el); free(eh);
    return bed;
}

// ---- flip/an/flip_syn.py flipE 3 --------------------------------------------
static const double FBT[11] = { 0.005, 0.020, 0.040, 0.060, 0.090, 0.130, 0.180, 0.250, 0.350, 0.450, 0.540 };
static const double FBL[11] = { -28, -35, -34, -37, -36, -37, -44, -54, -62, -70, -80 };
static const double FBH[11] = { -13.5, -25.9, -31.4, -35.2, -38.4, -45.2, -48.5, -57.9, -64, -71, -80 };
static void eqf(Sig *y, const double *t) { eq(y, CS40, t, NB, 100, 15000); }
static double lvl_snap(const Sig *y, int S0) {
    Sig b = bp_copy(y, 3000, 12000);
    const double m = max_moving_rms(&b, 12, S0 - 96, S0 + 96);
    sig_free(&b);
    return m;
}
Sig m_flip(Rng *r) {
    const int N = S_(0.9), S0 = S_(FLIP_REF_S); const double t0 = FLIP_REF_S;
    const int hard = uni(r, 0, 1) < 4.0 / 7;
    double t[NB];
    Sig snap = sig_new(N);
    if (hard) {
        burst_add(r, &snap, t0, 0, uni(r, 0.0025, 0.0040), 0.0001, 3);
        if (uni(r, 0, 1) < 0.25) burst_add(r, &snap, t0 + 0.0016, -8, uni(r, 0.002, 0.003), 0.0001, 3);
        if (uni(r, 0, 1) < 0.25) { const double tt = uni(r, 0.012, 0.018); burst_add(r, &snap, t0 + tt, -16, 0.002, 0.0001, 3); }
        for (int i = 0; i < NB; i++) t[i] = FLIP_SNAP[i] - 12 * clip(log2(1500 / CS40[i]), 0, 1);
        jitter(r, t, NB, 1.5);
        eqf(&snap, t);
        // the release: the edge leaving the fingers, a dark burst under the click
        const int Lr = S_(0.02); const double dec = uni(r, 0.002, 0.004);
        Sig rel = sig_new(N);
        for (int i = 0; i < Lr; i++) { const double q = (double)i / SR; rel.x[S0 - 24 + i] = normal(r) * fmin(1, q / 0.0005) * exp(-q / dec); }
        for (int i = 0; i < NB; i++) t[i] = FLIP_SNAP[i] - 8 * maxd(log2(CS40[i] / 1500), 0);
        eqf(&rel, t);
        sig_add(&snap, &rel, sig_peak(snap.x, N) / (sig_peak(rel.x, N) + 1e-12) * db2(-24));
        sig_free(&rel);
    } else {
        burst_add(r, &snap, t0 - 0.002, 0, uni(r, 0.004, 0.006), 0.0001, 3);
        for (int i = 0; i < NB; i++) t[i] = FLIP_SNAP[i] - (CS40[i] > 2500 ? 6 : 0);
        eqf(&snap, t);
    }
    const double ref = lvl_snap(&snap, S0);
    // the slide, ending at the snap
    const double L = uni(r, 0.054, 0.118);
    const int a = S_(t0 - L), n = S0 - a + S_(0.004);
    for (int i = 0; i < NB; i++) t[i] = FLIP_SLIDE[i] + FLIP_SFIT[i];
    jitter(r, t, NB, 1.5);
    Sig nz = colored_noise(r, n, CS40, t, NB, 100, 15000);
    const double fx[5] = { 0, 0.25, 0.5, 0.75, 1 };
    double fy[5];
    fy[0] = uni(r, 3300, 3800); fy[1] = uni(r, 2000, 2500); fy[2] = uni(r, 1400, 1700); fy[3] = uni(r, 1450, 1700); fy[4] = uni(r, 1500, 1800);
    double *fr = malloc(sizeof(double) * n);
    for (int i = 0; i < n; i++) fr[i] = interp((double)i / n, fx, fy, 5, fy[0], fy[4]);
    Sig res = reson(&nz, 0, fr, uni(r, 250, 450));
    const double rk = sig_rms(nz.x, n) / (sig_rms(res.x, n) + 1e-20);
    Sig body = sig_new(n);
    for (int i = 0; i < n; i++) body.x[i] = 0.85 * nz.x[i] + 0.15 * res.x[i] * rk;
    const int tp = S_(uni(r, 0.009, fmin(0.047, L * 0.45))), kp = (S0 - a) - tp;
    double rx[9], ry[9];
    static const double RISE[9] = { -21, -18, -16, -12, -8.5, -6, -4, -2, 0 };
    for (int i = 0; i < 9; i++) { rx[i] = i / 8.0; ry[i] = RISE[i] + uni(r, -1.5, 1.5); }
    static const double FX[6] = { 0, 5, 10, 20, 30, 60 }, FY[6] = { 0, -1.5, -4.5, -12, -16.4, -33 };
    const int ease = S_(0.015) < kp ? S_(0.015) : kp;
    for (int i = 0; i < n; i++) {
        double e;
        if (i < kp) {
            e = interp((double)i / kp, rx, ry, 9, ry[0], ry[8]);
            if (i < ease) e += -25 + 25.0 * i / (ease > 1 ? ease - 1 : 1);
        } else e = interp((double)(i - kp) / SR * 1000, FX, FY, 6, 0, -33);
        body.x[i] *= db2(e);
    }
    Sig bb = bp_copy(&body, 2000, 12000);
    const double hb = max_moving_rms(&bb, 240, 0, n);
    sig_free(&bb);
    Sig slide = sig_new(N);
    const double sg = ref * db2(hard ? uni(r, -23, -12) : uni(r, -6, -3)) / (hb + 1e-12);
    for (int i = 0; i < n; i++) slide.x[a + i] = body.x[i] * sg;
    // the whups: the card flapping as it turns
    Sig wh = sig_new(N);
    {
        const double u = uni(r, 0, 1);
        const int kc = u < 3.0 / 7 ? 1 : u < 4.0 / 7 ? 2 : u < 6.0 / 7 ? 3 : 4;
        double tw = kc == 1 ? uni(r, 0.019, 0.058) : uni(r, 0.019, 0.035);
        double f = uni(r, 0, 1) < 0.7 ? uni(r, 420, 570) : uni(r, 220, 340);
        double wt[NB];
        for (int i = 0; i < NB; i++) wt[i] = FLIP_WHUP[i] * 0.3;
        for (int i = 0; i < kc; i++) {
            const double db = i == 0 ? uni(r, -13, -1) : uni(r, -27, -7);
            const int Lw = S_(0.06); const double dec = uni(r, 0.005, 0.008);
            Sig x = noise(r, Lw);
            Sig p = reson(&x, f, 0, 320.0);
            for (int q = 0; q < Lw; q++) { const double s = (double)q / SR; p.x[q] *= fmin(1, s / 0.004) * exp(-maxd(s - 0.004, 0) / dec); }
            eqf(&p, wt);
            Sig pp = sig_new(Lw + 960);
            memcpy(pp.x + 480, p.x, sizeof(double) * Lw);
            bp(&pp, 150, 800);
            const double pk = max_moving_rms(&pp, 240, 0, pp.n);
            const int k = S0 + S_(tw) - S_(0.004);
            for (int q = 0; q < Lw && k + q < N; q++) wh.x[k + q] += p.x[q] / (pk + 1e-12) * ref * db2(db + 5.0);
            tw += uni(r, 0.040, 0.055);
            f *= i == 0 ? uni(r, 0.62, 0.72) : uni(r, 0.75, 0.85);
            sig_free(&x); sig_free(&p); sig_free(&pp);
        }
    }
    // the beds after the snap
    double *el = malloc(sizeof(double) * N), *eh = malloc(sizeof(double) * N);
    const double ts = uni(r, 0.85, 1.15);
    curve(el, N, t0, FBT, FBL, 11, ts, 2, -90, r);
    curve(eh, N, t0, FBT, FBH, 11, ts, 2, -90, r);
    Sig b1 = noise(r, N); bp(&b1, 150, 1500); unit_rms(&b1);
    double ht[NB]; above(FLIP_SLIDE, 1500.0001, -60, ht);   // np.where(cs > 1500, ...)
    Sig b2 = cnoise40(r, N, ht); bp(&b2, 1500, 12000); unit_rms(&b2);
    Sig rest = sig_new(N);
    for (int i = 0; i < N; i++) rest.x[i] = snap.x[i] + wh.x[i] + (b1.x[i] * el[i] + b2.x[i] * eh[i]) * ref;
    eqf(&rest, FLIP_FIT); eqf(&slide, FLIP_FIT);
    Sig ps = bp_copy(&slide, 100, 15000), pr = bp_copy(&rest, 100, 15000);
    const double Ls = db2(uni(r, -25, -20));
    sig_scale(&slide, Ls / (sig_peak(ps.x, N) + 1e-12));
    sig_scale(&rest, Ls * db2(hard ? uni(r, 0, 6) : uni(r, -4, -1)) / (sig_peak(pr.x, N) + 1e-12));
    sig_add(&slide, &rest, 1);
    sig_free(&ps); sig_free(&pr); sig_free(&rest); sig_free(&b1); sig_free(&b2); free(el); free(eh);
    sig_free(&snap); sig_free(&nz); sig_free(&res); sig_free(&body); sig_free(&wh); free(fr);
    return slide;
}

// ---- flip/an/deck_syn.py deck229c 5 -----------------------------------------
static const double DBT[17] = { -0.080, -0.040, -0.020, -0.008, -0.003, 0.0, 0.003, 0.008, 0.015, 0.025, 0.040, 0.060, 0.090, 0.130, 0.180, 0.250, 0.350 };
static const double DBL[17] = { -39, -32, -30, -30, -20, 0, -2.4, -9, -14.3, -21.5, -20.8, -24.9, -20.8, -30.4, -34.6, -43.8, -50.6 };
static const double DBH[17] = { -57, -52, -33, -39, -44, -41, -44, -42, -44, -47, -51, -56, -63, -68, -72, -76, -82 };
static void eqd(Sig *y, const double *t) { eq(y, CS40, t, NB, 90, 15000); }
Sig m_deck(Rng *r) {
    const int N = S_(0.8), P0 = S_(DECK_REF_S); const double t0 = DECK_REF_S, TABLE = 229.0 / 196.0;
    // the bed with its modes smoothed out below 500 Hz; the fit's low part moved up with the table
    double bsp[NB], fit[NB], lcs[NB];
    for (int i = 0; i < NB; i++) {
        double s = 0;
        for (int k = -2; k <= 2; k++) { int j = i + k; j = j < 0 ? 0 : j >= NB ? NB - 1 : j; s += DECK_BED[j]; }
        bsp[i] = CS40[i] < 500 ? s / 5 : DECK_BED[i];
        lcs[i] = log(CS40[i]);
    }
    for (int i = 0; i < NB; i++) fit[i] = CS40[i] < 600 ? interp(log(CS40[i] / TABLE), lcs, DECK_FIT, NB, DECK_FIT[0], DECK_FIT[NB - 1]) : DECK_FIT[i];
    const double ts = uni(r, 0.85, 1.15);
    double lt[NB], ht[NB];
    below(bsp, 1500, -60, lt); above(bsp, 1500, -60, ht);
    Sig lo = colored_noise(r, N, CS40, lt, NB, 90, 15000); bp(&lo, 90, 1500);
    double *el = malloc(sizeof(double) * N), *eh = malloc(sizeof(double) * N);
    curve(el, N, t0, DBT, DBL, 17, ts, 2, -90, r);
    unit_rms(&lo);
    for (int i = 0; i < N; i++) lo.x[i] *= el[i];
    // the table's modes, excited by the thud
    double fq[3], ex[3]; const double bw[3] = { 9, 11, 13 };
    fq[0] = (uni(r, 0, 1) < 5.0 / 6 ? 196.0 : 211.0) * TABLE; ex[0] = uni(r, 12, 20);
    fq[1] = 287.0 * TABLE; ex[1] = uni(r, 10, 13);
    fq[2] = 397.0 * TABLE; ex[2] = uni(r, 9, 18);
    for (int m = 0; m < 3; m++) {
        Sig md = reson(&lo, fq[m], 0, bw[m]);
        Sig a1 = bp_copy(&lo, fq[m] - 40, fq[m] - 15), a2 = bp_copy(&lo, fq[m] + 15, fq[m] + 40), mb = bp_copy(&md, fq[m] - 8, fq[m] + 8);
        double qn = 0, qm = 0; const int w0 = P0, w1 = P0 + 9600 < N ? P0 + 9600 : N;
        for (int i = w0; i < w1; i++) { const double v = a1.x[i] + a2.x[i]; qn += v * v; qm += mb.x[i] * mb.x[i]; }
        const double nb = sqrt(qn / (w1 - w0) / 2), mm = sqrt(qm / (w1 - w0)) + 1e-12;
        sig_add(&lo, &md, nb * db2(ex[m] - 8 - 12) / mm);
        sig_free(&md); sig_free(&a1); sig_free(&a2); sig_free(&mb);
    }
    const double thud = max_moving_rms(&lo, 240, 0, N);
    // the corner landing first
    {
        const int Lp = S_(0.03), kp = P0 - S_(uni(r, 0.010, 0.016));
        Sig pre = sig_new(N);
        for (int i = 0; i < Lp; i++) { const double q = (double)i / SR; pre.x[kp + i] = normal(r) * fmin(1, q / 0.001) * exp(-q / 0.003); }
        eqd(&pre, lt); bp(&pre, 90, 1500);
        sig_add(&lo, &pre, thud * db2(uni(r, -8, -4)) / (max_moving_rms(&pre, 240, 0, N) + 1e-12));
        sig_free(&pre);
    }
    Sig hi = colored_noise(r, N, CS40, ht, NB, 90, 15000); bp(&hi, 1500, 12000);
    curve(eh, N, t0, DBT, DBH, 17, ts, 2, -90, r);
    unit_rms(&hi);
    for (int i = 0; i < N; i++) hi.x[i] *= eh[i] * thud;
    // the contacts
    const int slap = uni(r, 0, 1) < 1.0 / 6;
    Sig ck = sig_new(N);
    const double first = slap ? 0.0 : -uni(r, 0.018, 0.024);
    burst_add(r, &ck, t0 + first, 0, uni(r, 0.0007, 0.0025), 0.0001, 3);
    for (int k = irange(r, 1, 9); k > 0; k--) { const double tt = -uni(r, 0.005, 0.040), d = uni(r, -19, -1), w = uni(r, 0.0007, 0.0028); burst_add(r, &ck, t0 + tt, d, w, 0.0001, 3); }
    if (slap) {
        { const double tt = uni(r, 0.001, 0.003), d = uni(r, -19, -13); burst_add(r, &ck, t0 + tt, d, 0.002, 0.0001, 3); }
        { const double tt = uni(r, 0.010, 0.012), d = uni(r, -28, -20); burst_add(r, &ck, t0 + tt, d, 0.002, 0.0001, 3); }
    }
    // the edge clatter
    Sig cl = sig_new(N);
    {
        const int Lc = S_(0.04), kc = S_(slap ? t0 - 0.003 : t0 + first - 0.005);
        for (int i = 0; i < Lc; i++) { const double q = (double)i / SR; cl.x[kc + i] = normal(r) * fmin(1, q / 0.005) * exp(-maxd(q - 0.005, 0) / 0.005); }
        double ct[NB]; above(DECK_CLAT, 1000, -40, ct);
        eqd(&cl, ct); bp(&cl, 1000, 15000);
        sig_scale(&cl, thud * db2(slap ? uni(r, -3, 1) : uni(r, -20, -14)) / (max_moving_rms(&cl, 240, 0, N) + 1e-12));
    }
    double t[NB];
    for (int i = 0; i < NB; i++) t[i] = DECK_CONTACT[i] - 20 * clip(log2(1500 / CS40[i]), 0, 1) - ((CS40[i] > 1000 && CS40[i] < 4000) ? 5 : 0);
    jitter(r, t, NB, 1.5);
    eqd(&ck, t);
    Sig cb = bp_copy(&ck, 3000, 12000);
    const double top = max_moving_rms(&cb, 12, 0, N);
    sig_free(&cb);
    sig_scale(&ck, thud * db2(slap ? uni(r, 5, 10) : uni(r, -15, -7)) / (top + 1e-12));
    Sig y = sig_new(N);
    for (int i = 0; i < N; i++) y.x[i] = lo.x[i] + hi.x[i] + cl.x[i] + ck.x[i];
    eqd(&y, fit);
    norm_band(&y, 100, uni(r, -25.7, -4.9));
    sig_free(&lo); sig_free(&hi); sig_free(&ck); sig_free(&cl); free(el); free(eh);
    return y;
}

// ---- pass/an/pass_syn.py pass7 4 --------------------------------------------
static const double PT[21] = { -0.02, 0, 0.020, 0.040, 0.060, 0.080, 0.100, 0.120, 0.140, 0.160, 0.180, 0.200, 0.220, 0.240, 0.260, 0.280, 0.300, 0.330, 0.360, 0.400, 0.450 };
static const double PEH[21] = { -60, -21.2, -15.5, -12.5, -9.4, -7.3, -6.2, -4.1, -2.3, -1.6, -4.2, -3.9, -6.1, -11.6, -17.3, -21.0, -17.2, -27.6, -30.7, -31.6, -45 };
static const double PEL[21] = { -60, -26.4, -21.6, -22.9, -21.9, -20.4, -17.2, -20.8, -20.2, -14.6, -18.1, -17.6, -20.4, -22.3, -24.5, -22.6, -18.9, -24.4, -29.0, -27.9, -45 };
// A pass envelope: the end points are not jittered (np.r_[0, jitter, 0]).
static void pass_curve(double *env, int n, double t0, const double *D, double ts, Rng *r) {
    double tt[21], dd[21];
    for (int i = 0; i < 21; i++) { tt[i] = PT[i] * ts; dd[i] = D[i] + ((i == 0 || i == 20) ? 0 : uni(r, -1.5, 1.5)); }
    for (int i = 0; i < n; i++) env[i] = db2(interp((double)i / SR - t0, tt, dd, 21, -90, -90));
}
static Sig pass_knock(Rng *r, int N, double at, double db, double P) {
    const int L = S_(0.06), k = S_(at); const double dec = uni(r, 0.006, 0.012);
    Sig z = sig_new(N);
    for (int i = 0; i < L && k + i < N; i++) { const double q = (double)i / SR; if (k + i >= 0) z.x[k + i] = normal(r) * fmin(1, q / 0.001) * exp(-q / dec); }
    double lt[NB]; below(PASS_SPEC, 1000, -40, lt);
    eq40(&z, lt); bp(&z, 150, 1000);
    const double rr = exp(-PI_D * 40 / SR), c = 2 * rr * cos(2 * PI_D * 229 / SR);
    Sig y = sig_new(N); double y1 = 0, y2 = 0;
    for (int i = k < 0 ? 0 : k; i < N && i < k + L + 4800; i++) { const double v = (1 - rr) * z.x[i] + c * y1 - rr * rr * y2; y.x[i] = v; y2 = y1; y1 = v; }
    sig_add(&z, &y, sig_peak(z.x, N) * 0.7 / (sig_peak(y.x, N) + 1e-12));
    sig_scale(&z, P * db2(db) / (max_moving_rms(&z, 240, 0, N) + 1e-12));
    sig_free(&y);
    return z;
}
Sig m_pass(Rng *r) {
    const int N = S_(0.7), O = S_(PASS_REF_S); const double o = PASS_REF_S, AM = 0.025;
    const double ts = uni(r, 0.85, 1.15);
    double *eh = malloc(sizeof(double) * N), *el = malloc(sizeof(double) * N);
    pass_curve(eh, N, o, PEH, ts, r);
    pass_curve(el, N, o, PEL, ts, r);
    double ht[NB], lt[NB];
    above(PASS_SPEC, 1000, -60, ht); below(PASS_SPEC, 1000, -60, lt);
    Sig hi = cnoise40(r, N, ht); bp(&hi, 1000, 15000); unit_rms(&hi);
    Sig lo = cnoise40(r, N, lt); bp(&lo, 150, 1000); unit_rms(&lo);
    for (int i = 0; i < N; i++) { hi.x[i] *= eh[i]; lo.x[i] *= el[i]; }
    const double f1 = uni(r, 55, 59), f2 = uni(r, 94, 98), p1 = uni(r, 0, 6.3), p2 = uni(r, 0, 6.3);
    Sig body = sig_new(N);
    for (int i = 0; i < N; i++) {
        const double t = (double)i / SR;
        body.x[i] = (hi.x[i] + lo.x[i]) * (1 + AM * (sin(2 * PI_D * f1 * t + p1) + 0.9 * sin(2 * PI_D * f2 * t + p2)));
    }
    Sig hb = bp_copy(&body, 3000, 15000);
    double *loc = malloc(sizeof(double) * N);
    moving_rms(hb.x, N, 480, loc);
    sig_free(&hb);
    Sig sn = sig_new(N);
    // the snags: as the fingers land, maybe one mid-slide, as the card stops
    #define SNAG(lo_, hi_, dlo, dhi, wlo, whi) do { \
        const double a = uni(r, lo_, hi_); const int k = O + (int)(a * ts * SR); \
        const double d = uni(r, dlo, dhi), w = uni(r, wlo, whi); \
        Sig s1 = sig_new(N); burst_add(r, &s1, o + a * ts, d, w, 0.0003, 3); \
        sig_add(&sn, &s1, loc[k < N ? k : N - 1] * 1.6); sig_free(&s1); } while (0)
    for (int k = irange(r, 1, 4); k > 0; k--) SNAG(0.002, 0.020, 12, 23, 0.0006, 0.0018);
    if (uni(r, 0, 1) < 0.5) SNAG(0.06, 0.22, 12, 14, 0.001, 0.002);
    for (int k = irange(r, 2, 4); k > 0; k--) SNAG(0.275, 0.345, 12, 22, 0.0006, 0.0018);
    #undef SNAG
    double st[NB];
    for (int i = 0; i < NB; i++) st[i] = CS40[i] >= 1500 ? PASS_SPEC[i] : PASS_SPEC[i] - 12;
    eq40(&sn, st);
    sig_scale(&sn, db2(6.0));
    double *e5 = malloc(sizeof(double) * N), P = 0;
    moving_rms(hi.x, N, 240, e5);
    for (int i = 0; i < N; i++) P = maxd(P, e5[i]);
    free(e5);
    // the table knocks: the fingers landing, the card stopping, a few weak ones between
    { const double at = o + uni(r, -0.095, 0.030) * ts, db = uni(r, -12, 5); Sig k1 = pass_knock(r, N, at, db, P); sig_add(&sn, &k1, 1); sig_free(&k1); }
    { const double at = o + uni(r, 0.284, 0.345) * ts, db = uni(r, -10, -3.5); Sig k2 = pass_knock(r, N, at, db, P); sig_add(&sn, &k2, 1); sig_free(&k2); }
    for (int k = irange(r, 1, 4); k > 0; k--) {
        const double at = o + uni(r, 0.11, 0.23) * ts, db = uni(r, -12, -9);
        Sig kk = pass_knock(r, N, at, db, P); sig_add(&sn, &kk, 1); sig_free(&kk);
    }
    sig_add(&body, &sn, 1);
    eq40(&body, PASS_FIT);
    norm_band(&body, 150, uni(r, -36.2, -30.6));
    sig_free(&hi); sig_free(&lo); sig_free(&sn); free(eh); free(el); free(loc);
    return body;
}

// ---- discard/an/disc_syn.py ----------------------------------------------------
#define ROUGH 3.0
// The level's slow wander in dB: noise boxed over `corr` s, scaled to `sd`.
static void wander(Rng *r, double *out, int n, double sd, double corr) {
    const int w = (int)(corr * SR);
    double *x = malloc(sizeof(double) * (size_t)(n + w)), s = 0, q = 0;
    for (int i = 0; i < n + w; i++) x[i] = normal(r);
    double acc = 0;
    for (int i = 0; i < w; i++) acc += x[i];
    for (int i = 0; i < n; i++) { out[i] = acc / w; acc += x[i + w] - x[i]; }
    for (int i = 0; i < n; i++) s += out[i];
    s /= n;
    for (int i = 0; i < n; i++) q += (out[i] - s) * (out[i] - s);
    q = sqrt(q / n) + 1e-12;
    for (int i = 0; i < n; i++) out[i] = out[i] / q * sd;
    free(x);
}
// smooth(v, k): numpy's edge-padded box of k.
static void smooth(const double *v, int n, int k, double *out) {
    for (int i = 0; i < n; i++) {
        double s = 0;
        for (int j = -k / 2; j <= k / 2; j++) { int m = i + j; m = m < 0 ? 0 : m >= n ? n - 1 : m; s += v[m]; }
        out[i] = s / k;
    }
}
// two_band: the template's halves, each on its own dB envelope.
static Sig two_band(Rng *r, int n, const double *tmpl, const double *el, const double *eh) {
    double lt[NB], ht[NB];
    below(tmpl, 1500, -60, lt); above(tmpl, 1500, -60, ht);
    Sig lo = cnoise40(r, n, lt); bp(&lo, 150, 1500); unit_rms(&lo);
    Sig hi = cnoise40(r, n, ht); bp(&hi, 1500, 15000); unit_rms(&hi);
    Sig y = sig_new(n);
    for (int i = 0; i < n; i++) y.x[i] = lo.x[i] * db2(el[i]) + hi.x[i] * db2(eh[i]);
    sig_free(&lo); sig_free(&hi);
    return y;
}
// strike: a click in the template, its peak at `db`, added in at `gain`.
static void strike_add(Rng *r, Sig *y, double at, double width, const double *tmpl, double gain) {
    Sig z = sig_new(y->n);
    burst_add(r, &z, at, 0, width, 0.0002, 3);
    eq40(&z, tmpl);
    sig_add(y, &z, gain / (sig_peak(z.x, z.n) + 1e-12));
    sig_free(&z);
}
static void no_rumble(const double *v, double *out) {
    double ref = 0; int got = 0;
    for (int i = 0; i < NB; i++) if (CS40[i] >= 280 && !got) { ref = v[i]; got = 1; }
    for (int i = 0; i < NB; i++) out[i] = CS40[i] < 280 ? fmin(v[i], ref) : v[i];
}
typedef struct { const double *spec, *fit, *ex, *elo, *ehi, *durs; int n_env, n_durs; double rough; int pass_timbre; } StrokeKind;
static Sig stroke_of(Rng *r, double dur, const StrokeKind *K) {
    const double pre = STROKE_PRE_S, post = 0.12;
    if (dur <= 0) dur = clip(K->durs[irange(r, 0, K->n_durs)] * uni(r, 0.9, 1.1), 0.25, 0.95);
    const int n = S_(pre + dur + post);
    double EL[64], EH[64], jl[64], jh[64];
    smooth(K->elo, K->n_env, 3, EL); smooth(K->ehi, K->n_env, 3, EH);
    for (int i = 0; i < K->n_env; i++) { jl[i] = EL[i] + uni(r, -1.5, 1.5); }
    for (int i = 0; i < K->n_env; i++) { jh[i] = EH[i] + uni(r, -1.5, 1.5); }
    double *el = malloc(sizeof(double) * n), *eh = malloc(sizeof(double) * n);
    double *sl = malloc(sizeof(double) * n), *fast = malloc(sizeof(double) * n), *low = malloc(sizeof(double) * n);
    wander(r, sl, n, K->rough, 0.04);
    wander(r, fast, n, 0.8, 0.008);
    double tmpl[NB];
    double lo_off = 0;
    if (K->pass_timbre) {   // SHFF = 1: the strokes take the pass's timbre, its lows 14 dB under its highs
        for (int i = 0; i < NB; i++) tmpl[i] = PASS_SPEC[i] + PASS_FIT[i];
        lo_off = -14.0;
    } else {
        for (int i = 0; i < NB; i++) tmpl[i] = K->spec[i] + K->fit[i];
    }
    wander(r, low, n, 3.0, 0.015);
    for (int i = 0; i < n; i++) {
        const double u = ((double)i / SR - pre) / dur;
        el[i] = interp(u, K->ex, jl, K->n_env, -70, -70) + sl[i] + low[i] + lo_off;
        eh[i] = interp(u, K->ex, jh, K->n_env, -70, -70) + sl[i] + fast[i];
    }
    Sig y = two_band(r, n, tmpl, el, eh);
    Sig yb = bp_copy(&y, 3000, 15000);
    double *loc = malloc(sizeof(double) * n);
    moving_rms(yb.x, n, 480, loc);
    sig_free(&yb);
    double ct[NB];
    for (int i = 0; i < NB; i++) ct[i] = CS40[i] >= 1500 ? tmpl[i] : tmpl[i] - 12;
    for (int c = poisson(r, 14 * dur); c > 0; c--) {   // card edges catching
        const double a = pre + uni(r, 0.02, 1.0) * dur;
        const int k = S_(a) < n ? S_(a) : n - 1;
        const double w = uni(r, 0.001, 0.0025), g = db2(uni(r, 6, 13));
        strike_add(r, &y, a, w, ct, loc[k] * g * 3);
    }
    free(el); free(eh); free(sl); free(fast); free(low); free(loc);
    return y;
}
double m_stroke_dur(Rng *r) { return SCR_DUR[irange(r, 0, SCR_DUR_N)]; }
Sig m_stroke(Rng *r, double dur) {
    const StrokeKind K = { SCR_SPEC, SCR_FIT, SCR_ENV_X, SCR_ENV_LO, SCR_ENV_HI, SCR_DUR, SCR_ENV_X_N, SCR_DUR_N, ROUGH, 1 };
    return stroke_of(r, dur, &K);
}
Sig m_insert(Rng *r) {
    static double ins[NB];
    no_rumble(INS_SPEC, ins);
    const StrokeKind K = { ins, INS_FIT, INS_ENV_X, INS_ENV_LO, INS_ENV_HI, INS_DUR, INS_ENV_X_N, INS_DUR_N, ROUGH * 1.3, 0 };
    return stroke_of(r, 0, &K);
}
typedef struct { double at, db, w; } Knock;
static Sig event(Rng *r, const double *spec, const double *fit, const double *ex, const double *elo, const double *ehi,
                 int ne, const Knock *k, int nk, double kg) {
    const double pre = EVENT_PRE_S, post = 0.32;
    const int n = S_(pre + post);
    double *EL = malloc(sizeof(double) * ne), *EH = malloc(sizeof(double) * ne), *T = malloc(sizeof(double) * ne);
    double *jl = malloc(sizeof(double) * ne), *jh = malloc(sizeof(double) * ne);
    smooth(elo, ne, 5, EL); smooth(ehi, ne, 5, EH);
    const double ts = uni(r, 0.95, 1.25);
    for (int i = 0; i < ne; i++) { T[i] = ex[i] * ts; jl[i] = EL[i] + uni(r, -1.5, 1.5); }
    for (int i = 0; i < ne; i++) jh[i] = EH[i] + uni(r, -1.5, 1.5);
    double *el = malloc(sizeof(double) * n), *eh = malloc(sizeof(double) * n);
    for (int i = 0; i < n; i++) {
        const double tt = ((double)i / SR - pre) * 1000;
        el[i] = interp(tt, T, jl, ne, -70, -70); eh[i] = interp(tt, T, jh, ne, -70, -70);
    }
    double tmpl[NB];
    for (int i = 0; i < NB; i++) tmpl[i] = spec[i] + fit[i];
    Sig y = two_band(r, n, tmpl, el, eh);
    const double ref = sig_rms(y.x + S_(pre) - 240, 480);
    for (int i = 0; i < nk; i++) strike_add(r, &y, pre + k[i].at, k[i].w, tmpl, ref * db2(k[i].db) * kg);
    free(EL); free(EH); free(T); free(jl); free(jh); free(el); free(eh);
    return y;
}
static int ticks(Rng *r, Knock *k, int count, double t0, double t1, double lo, double hi) {
    for (int i = 0; i < count; i++) { k[i].at = uni(r, t0, t1); k[i].db = uni(r, lo, hi); k[i].w = uni(r, 0.0005, 0.0012); }
    return count;
}
Sig m_lift(Rng *r) {
    Knock k[8]; int n = 0;
    k[n].at = 0; k[n].db = uni(r, 0, 2); k[n].w = uni(r, 0.0008, 0.0015); n++;
    k[n].at = uni(r, 0.035, 0.062); k[n].db = uni(r, -10, -6); k[n].w = uni(r, 0.001, 0.002); n++;
    n += ticks(r, k + n, irange(r, 2, 4), -0.025, 0.11, -14, -6);
    return event(r, LIFT_SPEC, LIFT_FIT, LIFT_ENV_X, LIFT_ENV_LO, LIFT_ENV_HI, LIFT_ENV_X_N, k, n, 4);
}
Sig m_place(Rng *r) {
    Knock k[12]; int n = 0;
    k[n].at = 0; k[n].db = uni(r, -2, 0); k[n].w = uni(r, 0.0012, 0.002); n++;
    k[n].at = uni(r, 0.015, 0.075); k[n].db = uni(r, -13, -5); k[n].w = uni(r, 0.0012, 0.002); n++;
    n += ticks(r, k + n, irange(r, 4, 8), 0.0, 0.3, -20, -10);
    return event(r, PLACE_SPEC, PLACE_FIT, PLACE_ENV_X, PLACE_ENV_LO, PLACE_ENV_HI, PLACE_ENV_X_N, k, n, 7);
}
Sig m_plift(Rng *r) {
    static double pl[NB];
    no_rumble(PLIFT_SPEC, pl);
    Knock k[4]; int n = 0;
    k[n].at = 0; k[n].db = uni(r, -1, 1); k[n].w = uni(r, 0.0008, 0.0015); n++;
    n += ticks(r, k + n, irange(r, 0, 3), -0.02, 0.08, -18, -10);
    return event(r, pl, PLIFT_FIT, PLIFT_ENV_X, PLIFT_ENV_LO, PLIFT_ENV_HI, PLIFT_ENV_X_N, k, n, 3);
}
