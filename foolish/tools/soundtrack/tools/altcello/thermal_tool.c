/* Diagnostics of the thermal elasto-plastic bow friction (friction_thermal.h): a diagnostic tool, not part of the shipped instrument.
 *
 *   thermal_tool steady FB_N
 *       the steady sliding map at bow force FB_N (width of the instrument): per speed v, the contact temperature and the friction
 *       coefficient the model settles at (time stepping) against the closed forms of Appendix B (muss(tau(v))), and the Stribeck
 *       coefficient mustr(v)
 *   thermal_tool note STRING BETA F VB SECS OUT [--curve] [--body] [--semis N] [--last N] [--stride K]
 *       a bowed note (string alone unless --body, F and v_B ramped in over 30 ms); the last N periods (default 3) are written to OUT
 *       (t_ms vrel phi tau_K z_m) and a summary line gives the peak contact temperature (K above ambient, and in C with 20 C ambient)
 *       over the last 10 periods. --curve runs the friction curve (phi = friction force / F_B, tau = 0).
 *   knobs: ALTCELLO_KNOBS (knobs.h), e.g. th_xi=3,th_b=0.05
 */
#include "friction_thermal.h"
#include "knobs.h"
#include "sim.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    double *tau, *z, *phi;
    long n;
} Aux;

int main(int argc, char **argv)
{
    knobs_install();
    const Instrument *I = instrument_cello();
    if (argc >= 3 && !strcmp(argv[1], "steady")) {
        double F = atof(argv[2]);
        Thermal T;
        double Fb = F / I->bow.width;
        printf("# Fb %.1f N/m, %s\n# v  tau_sim  tau_B3  mu_sim  muss(tau_B3)  mu_str(v)  y\n", Fb, "Table 1 constants unless knobs");
        for (double v = 0.003; v < 5.0; v *= 1.6) {
            thermal_init(&T, &I->bow.thermal, I->bow.width, 1.0 / 96000.0);
            double phi = 0.0;
            for (int i = 0; i < 96000 / 2; i++) phi = thermal_advance(&T, F, v);
            double tb = thermal_tau_of_v(&I->bow.thermal, Fb, v);
            double mss = thermal_mu_ss(&I->bow.thermal, tb);
            printf("%.4f %.4f %.4f %.5f %.5f %.5f %.4f\n", v, T.tau, tb, phi, mss, thermal_mu_str_tau(&I->bow.thermal, tb), thermal_y(&I->bow.thermal, tb));
        }
        return 0;
    }
    if (argc >= 8 && !strcmp(argv[1], "note")) {
        const StringSpec *sp = instrument_string(I, argv[2]);
        if (!sp) { fprintf(stderr, "unknown string\n"); return 2; }
        double beta = atof(argv[3]), F = atof(argv[4]), vB = atof(argv[5]), secs = atof(argv[6]), fs = 96000.0;
        const char *out = argv[7];
        int curve = 0, body = 0, semis = 0, last = 3, stride = 1;
        for (int i = 8; i < argc; i++) {
            if (!strcmp(argv[i], "--curve")) curve = 1;
            else if (!strcmp(argv[i], "--body")) body = 1;
            else if (!strcmp(argv[i], "--semis") && i + 1 < argc) semis = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--last") && i + 1 < argc) last = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--stride") && i + 1 < argc) stride = atoi(argv[++i]);
        }
        Instrument ic = *I;
        ic.bow.model = curve ? BOW_CURVE : BOW_THERMAL;
        Sim S;
        if (body) {
            SimOpts so = {1, 1, 0, NULL};
            sim_init_instrument(&S, &ic, fs, &so, argv[2]);
        } else
            sim_init(&S, &ic, sp, fs);
        double ratio = semis > 0 ? pow(2.0, semis / 12.0) : 1.0;
        (void)ratio;
        sim_set_bow(&S, beta, vB, F, 0.03);
        sim_start(&S);
        long n = (long)(secs * fs);
        Aux a = {calloc((size_t)n, sizeof(double)), calloc((size_t)n, sizeof(double)), calloc((size_t)n, sizeof(double)), n};
        double *vr = calloc((size_t)n, sizeof(double));
        for (long i = 0; i < n; i++) {
            sim_step(&S);
            a.tau[i] = curve ? 0.0 : S.th.tau;
            a.z[i] = curve ? 0.0 : S.th.z;
            double Fi = fabs(S.out[4]);
            vr[i] = S.out[3];
            a.phi[i] = (curve || Fi > 0.0) ? S.out[4] / (F > 0.0 ? F : 1.0) : 0.0;
        }
        long P = (long)(fs / sp->f0 + 0.5), i0 = n - last * P, i10 = n - 10 * P;
        if (i0 < 0) i0 = 0;
        double tmax = 0.0, vmax = 0.0;
        for (long i = i10; i < n; i++) {
            if (a.tau[i] > tmax) tmax = a.tau[i];
            if (fabs(vr[i]) > vmax) vmax = fabs(vr[i]);
        }
        FILE *f = fopen(out, "w");
        fprintf(f, "# t_ms vrel phi tau_K z_m\n");
        for (long i = i0; i < n; i += stride) fprintf(f, "%.4f %.6f %.6f %.4f %.4e\n", 1000.0 * (double)(i - i0) / fs, vr[i], a.phi[i], a.tau[i], a.z[i]);
        fclose(f);
        printf("%s %s beta %.3f F %.3f vB %.3f: peak tau (last 10 periods) %.2f K above ambient = %.1f C, peak |vrel| %.3f m/s\n", argv[2], curve ? "curve" : "thermal", beta,
               F, vB, tmax, 20.0 + tmax, vmax);
        return 0;
    }
    fprintf(stderr, "usage: see thermal_tool.c\n");
    return 2;
}
