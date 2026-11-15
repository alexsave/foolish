/* ORACLE: an independent bowed-string reference, the McIntyre-Schumacher-Woodhouse / Smith digital waveguide
 * (a test and diagnostic tool, NOT part of the shipped instrument).
 *
 * Two delay-line pairs (bow to bridge and back, bow to nut and back) carry velocity waves; the string's wave
 * impedance Z0 = sqrt(T rho) is the only string constant besides f0. Each end is a rigid termination (reflection -1) with
 * a one-pole low-pass loss filter H(z) = g (1 - p) / (1 - p z^-1) per reflection (the whole string loss lives in these two
 * filters; no body). The bow is a point (no width), there is no torsion and no stiffness. The bow junction is solved by the
 * graphical (Friedlander) intersection with hysteresis, independently of bow.c.
 *
 * Junction (derived in oracle_msw.c): v = a1 + a2 + f_s / (2 Z0), with a1, a2 the waves arriving from the nut and bridge
 * sides and f_s = -F_B phi(v - v_B) the friction force on the string: the same normalisation as the finite difference model
 * (friction force = F_B phi(v_rel), phi(0+) = mu_static). Delay lines read with 3rd order Lagrange interpolation so that f0
 * is exact. Same sample rate as the model (96 kHz). */
#ifndef ALTCELLO_ORACLE_MSW_H
#define ALTCELLO_ORACLE_MSW_H
#include "instrument.h"

typedef struct {
    double fs, f0;
    double Z0;      /* N s/m */
    double beta;    /* bow position, fraction of the string length from the bridge */
    double g, p;    /* per reflection: H(z) = g (1 - p) / (1 - p z^-1) */
    Friction fr;
} OracleSpec;

typedef struct Oracle Oracle;
Oracle *oracle_new(const OracleSpec *s);
void oracle_free(Oracle *o);
/* One sample. vB bow speed, FB bow force (N), fext an extra transverse force on the string at the bow point (used to pluck;
 * 0 when bowing). Outputs: v_rel = v_string - vB at the bow, the bridge force (sign convention of string_bridge_force). */
void oracle_step(Oracle *o, double vB, double FB, double fext, double *vrel, double *bridge);

/* the bow junction alone: solve v = q - G phi(v) (Friedlander construction with hysteresis); *stuck is the previous branch on
 * entry (1 = sticking) and the new one on exit; *Phi is the friction coefficient (force / F_B) */
double oracle_junction(const Friction *fr, double q, double G, int *stuck, double *Phi);
/* amplitude of the loss filter H at angular frequency w (rad/sample), and the phase delay (samples) */
double oracle_H_mag(double g, double p, double w);
double oracle_H_phase_delay(double p, double w);
/* decay rate (1/s, amplitude) of a partial at w that a loop with two such reflections gives: -2 f0 ln |H| */
double oracle_alpha_model(double g, double p, double f0, double fs, double w);
/* least squares fit of (g, p) to measured partial decay rates alpha[k-1], k = 1..K, partial k at k f0: minimises the sum of
 * the squared relative errors of the decay rate (grid search then refinement). Returns the rms relative error. */
double oracle_fit_loss(const double *alpha, int K, double f0, double fs, double *g, double *p);
/* wave impedance of a string spec */
double oracle_Z0(const StringSpec *sp);
/* The oracle for string sp of instrument I at bow position beta: Z0 from the string, the loss filter fitted to the partial
 * decays that the finite difference string (no bow, rigid bridge, the instrument's loss) gives for a pluck (a T1 style
 * measurement). alpha_out (if not NULL, room for Kmax values) receives the measured decay rates; K_out the count used. */
#define ORACLE_KFIT_MAX 64
double oracle_spec_from_string(const Instrument *I, const StringSpec *sp, double fs, double beta, double fit_fmax, OracleSpec *out,
                               double *alpha_out, int *K_out);
#endif
