/* Sympathetic ringing of the open strings, measured in the simulation (a diagnostic tool, not part of the shipped instrument).
 *
 *   ring_tool change STRING S0 S1 [--tsw 2.0] [--seed N] [--trace OUT]
 *   ring_tool lift   STRING S0    [--tsw 2.0] [--seed N]
 *
 * One continuous bow on STRING, finger at S0 semitones above the open string (all four strings on the bridge, body on, no vibrato).
 * `change`: at tsw the finger moves to S1 (10 ms swap, press dip 0.8, bow unchanged: a slur). `lift`: at tsw the bow force goes to zero
 * in 5 ms and the finger stays.
 * The radiated pressure of every string alone comes from the shadow bodies (sim.h sim_enable_shadow): the body is linear, so the
 * pressure of the string that is in unison with the stopped note (the OPEN string whose pitch the stopped note has) and the pressure of
 * the bowed string are separate exact quantities; their sum is the total pressure.
 *
 * Output lines (all levels in dB re the bowed string's level in the same window and at the same frequencies, sums over partials 1..8):
 *   NOTE   k  f  open_dB bowed_dB ratio_dB      the last 0.5 s before tsw, partial k of the stopped pitch (open string at its own peak within 0.5 percent)
 *   NOTE_SUM ratio                              ratio over partials 1..8 (power sum)
 *   WIN  t0 t1 ring_dB broad_dB old_in_total_dB
 *       t0 t1: window after tsw (s); ring_dB = open string's power at the OLD partials re the bowed string's power at the NEW partials
 *       (for `lift`: at the old partials, both); broad_dB = rms of the open string's whole pressure re the bowed string's;
 *       old_in_total_dB = power at the old partials in the TOTAL pressure re the power at the new partials in the total pressure.
 */
#include "analysis.h"
#include "knobs.h"
#include "legato.h"
#include "player.h"
#include "sim.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Hann windowed single frequency DFT power of x[a, b) at f */
static double dft_pow(const double *x, long a, long b, double fs, double f)
{
    double re = 0.0, im = 0.0, w = 2.0 * M_PI * f / fs;
    long n = b - a;
    for (long i = 0; i < n; i++) {
        double h = 0.5 - 0.5 * cos(2.0 * M_PI * (i + 0.5) / n);
        re += h * x[a + i] * cos(w * (double)(a + i));
        im -= h * x[a + i] * sin(w * (double)(a + i));
    }
    return (re * re + im * im) / ((double)n * (double)n);
}
/* the largest of dft_pow within +-frac of f (0.2 Hz steps) */
static double peak_pow(const double *x, long a, long b, double fs, double f, double frac)
{
    double best = 0.0;
    double df = 0.25;
    for (double g = f * (1.0 - frac); g <= f * (1.0 + frac); g += df) {
        double p = dft_pow(x, a, b, fs, g);
        if (p > best) best = p;
    }
    return best;
}
static double partial_sum(const double *x, long a, long b, double fs, double f0, int K, double frac)
{
    double s = 0.0;
    for (int k = 1; k <= K; k++) s += peak_pow(x, a, b, fs, k * f0, frac);
    return s;
}
static double rms(const double *x, long a, long b)
{
    double s = 0.0;
    for (long i = a; i < b; i++) s += x[i] * x[i];
    return sqrt(s / (double)(b - a));
}
static double db(double p) { return 10.0 * log10(p + 1e-300); }

int main(int argc, char **argv)
{
    knobs_install();
    if (argc < 4) { fprintf(stderr, "usage: ring_tool change STRING S0 S1 | lift STRING S0 [--tsw T] [--seed N]\n"); return 2; }
    int lift = !strcmp(argv[1], "lift");
    const Instrument *I = instrument_cello();
    int si = instrument_string_index(I, argv[2]);
    if (si < 0) { fprintf(stderr, "unknown string\n"); return 2; }
    int s0 = atoi(argv[3]), s1 = lift ? s0 : (argc > 4 ? atoi(argv[4]) : s0);
    double tsw = 2.0;
    uint64_t seed = 0;
    for (int i = lift ? 4 : 5; i < argc; i++) {
        if (!strcmp(argv[i], "--tsw") && i + 1 < argc) tsw = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
    }
    const StringSpec *sp = &I->str[si];
    double f_old = sp->f0 * pow(2.0, s0 / 12.0), f_new = sp->f0 * pow(2.0, s1 / 12.0);
    /* the open string in unison with the old note (or the nearest open string) */
    int oi = -1;
    double bestc = 1e9;
    for (int i = 0; i < I->nstr; i++) {
        if (i == si) continue;
        double c = fabs(cents(f_old, I->str[i].f0));
        if (c < bestc) { bestc = c; oi = i; }
    }
    double vB = 0.1, beta_rel = 0.1, beta = beta_rel / pow(2.0, s0 / 12.0), lo, hi;
    if (!player_band(I, si, beta_rel, vB, &lo, &hi)) { lo = 0.5 * I->helmholtz_force[si]; hi = 2.0 * I->helmholtz_force[si]; }
    double F = sqrt(lo * hi);
    Sim S;
    SimOpts so = {1, 1, 0, NULL};
    sim_init_instrument(&S, I, FS96, &so, sp->name);
    if (sim_enable_shadow(&S, NULL)) { fprintf(stderr, "shadow failed\n"); return 1; }
    env_const(&S.beta, beta);
    S.FB.n = 0;
    env_add(&S.FB, 0.0, 0.0);
    env_add(&S.FB, 0.005, F);
    S.vB.n = 0;
    env_add(&S.vB, 0.0, 0.0);
    env_add(&S.vB, 0.02, 0.0);
    env_add(&S.vB, 0.02 + vB / 2.0, vB);
    double x0 = s0 > 0 ? finger_for(I, si, s0) : 0.0, x1 = s1 > 0 ? finger_for(I, si, s1) : 0.0, ramp = 0.010;
    S.xF.n = 0;
    S.aF.n = 0;
    if (lift) {
        env_const(&S.xF, x0);
        env_const(&S.aF, s0 > 0 ? 1.0 : 0.0);
        S.FB.n = 0;
        env_add(&S.FB, 0.0, 0.0);
        env_add(&S.FB, 0.005, F);
        env_add(&S.FB, tsw, F);
        env_add(&S.FB, tsw + 0.005, 0.0);
    } else {
        env_add(&S.xF, 0.0, x0);
        env_add(&S.xF, tsw + 0.5 * ramp - 1e-5, x0);
        env_add(&S.xF, tsw + 0.5 * ramp, x1);
        env_add(&S.aF, 0.0, s0 > 0 ? 1.0 : 0.0);
        for (int k = 0; k <= 8; k++) env_add(&S.aF, tsw + ramp * k / 8.0, (s0 > 0 || s1 > 0) ? 1.0 - 0.8 * sin(M_PI * k / 8.0) : 0.0);
    }
    sim_start(&S);
    if (seed) rng_seed(&S.rng, ALTCELLO_SEED ^ (seed * 0x9e3779b97f4a7c15ULL));
    double total_s = tsw + 0.8;
    long n = (long)(total_s * FS96);
    double *pt = malloc(sizeof(double) * (size_t)n), *pb = malloc(sizeof(double) * (size_t)n), *po = malloc(sizeof(double) * (size_t)n), *pall = malloc(sizeof(double) * (size_t)n);
    for (long i = 0; i < n; i++) {
        sim_step(&S);
        pt[i] = S.out[7];
        pb[i] = S.out_pstr[si];
        po[i] = S.out_pstr[oi];
        pall[i] = 0.0;
        for (int j = 0; j < S.nstr; j++) pall[i] += S.out_pstr[j];
    }
    double maxd = 0.0, mx = 0.0;
    for (long i = 0; i < n; i++) { double d = fabs(pt[i] - pall[i]); if (d > maxd) maxd = d; if (fabs(pt[i]) > mx) mx = fabs(pt[i]); }
    printf("RING string %s semis %d -> %d  f_old %.2f f_new %.2f  open string %s (%.1f c from the old pitch)  F %.3f N beta %.4f\n", sp->name, s0, s1, f_old, f_new, I->str[oi].name,
           cents(f_old, I->str[oi].f0), F, beta);
    /* the note */
    long a = (long)((tsw - 0.5) * FS96), b = (long)(tsw * FS96);
    double sum_o = 0.0, sum_b = 0.0;
    for (int k = 1; k <= 8; k++) {
        double po_k = peak_pow(po, a, b, FS96, k * f_old, 0.005), pb_k = peak_pow(pb, a, b, FS96, k * f_old, 0.005);
        sum_o += po_k;
        sum_b += pb_k;
        printf("NOTE %d %.1f %.1f %.1f %.1f\n", k, k * f_old, db(po_k), db(pb_k), db(po_k) - db(pb_k));
    }
    printf("NOTE_SUM %.2f\n", db(sum_o) - db(sum_b));
    printf("NOTE_BROAD %.2f\n", db(rms(po, a, b) * rms(po, a, b)) - db(rms(pb, a, b) * rms(pb, a, b)));
    /* after the event */
    double w0[3] = {0.1, 0.3, 0.5};
    for (int j = 0; j < 3; j++) {
        long a1 = (long)((tsw + w0[j]) * FS96), b1 = (long)((tsw + w0[j] + 0.2) * FS96);
        if (b1 > n) b1 = n;
        double f_cmp = lift ? f_old : f_new;
        double Po = partial_sum(po, a1, b1, FS96, f_old, 8, 0.005);
        double Pbn = partial_sum(pb, a1, b1, FS96, f_cmp, 8, 0.005);
        double Pbo = partial_sum(pb, a1, b1, FS96, f_old, 8, 0.005);
        double Ptot_old = partial_sum(pt, a1, b1, FS96, f_old, 8, 0.005), Ptot_new = partial_sum(pt, a1, b1, FS96, f_cmp, 8, 0.005);
        double ro = rms(po, a1, b1), rb = rms(pb, a1, b1);
        printf("WIN %.2f %.2f ring_dB %.2f broad_dB %.2f old_in_total_dB %.2f open_old_dB %.1f bowed_new_dB %.1f bowed_old_dB %.1f\n", w0[j], w0[j] + 0.2, db(Po) - db(Pbn),
               db(ro * ro) - db(rb * rb), lift ? 0.0 : db(Ptot_old) - db(Ptot_new), db(Po), db(Pbn), db(Pbo));
        (void)Ptot_new;
    }
    printf("CHECK max |p_total - sum of the string pressures| / max|p| = %.3g\n", maxd / mx);
    return 0;
}
