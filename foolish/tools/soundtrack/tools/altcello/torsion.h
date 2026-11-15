/* Torsion of the string: an angular coordinate theta(x,t),
 *
 *   I_p theta_tt = I_p c_t^2 theta_xx - 2 sigma_t I_p theta_t + tau - a g (K_t theta + R_t theta_t)
 *
 * fixed (theta = 0) at nut and bridge, on its own grid (stability limit of its
 * own, much faster, wave speed c_t). The bow touches the string SURFACE (radius
 * r_s): the contact velocity is u_t + r_s theta_t and a friction force f (N/m)
 * on u is a torque density tau = r_s f on theta. The finger pad, glued to the
 * surface, damps torsion with K_t = K r_s^2, R_t = R r_s^2 (the torque of the
 * same pad force acting at the radius r_s on the rotation r_s theta).
 *
 * Scheme (centred in time, sigma_t centred, same form as altstring.c):
 *   D_l th2_l = 2 th1_l + c_t^2 k^2 dxx th1 - (1 - sigma_t k) th0_l - C_l th0_l + k^2 tau_l / I_p
 *   D_l = 1 + sigma_t k + gam_l (K_t/2 + R_t/(2k)),  C_l = gam_l (K_t/2 - R_t/(2k)),
 *   gam_l = k^2 a g_l / I_p.
 * th0 = time n-1, th1 = n, th2 = n+1. Energy per unit I_p, see torsion_energy. */
#ifndef ALTCELLO_TORSION_H
#define ALTCELLO_TORSION_H
#include "string_params.h"

typedef struct {
    double fs, k, h, ct, sigma, Ip, rs, L, kk; /* kk = k^2 / Ip */
    int N;                                     /* grid points 0..N, theta_0 = theta_N = 0 */
    double *th0, *th1, *th2, *invD, *C;
    int fing_lo, fing_hi;
} Torsion;

Torsion *torsion_new(const StringSpec *sp, double fs);
void torsion_free(Torsion *t);
/* free update th2 = f(th1, th0) (no bow torque); the bow then adds -kk invD rs J F Phi */
void torsion_step_free(Torsion *t);
void torsion_commit(Torsion *t); /* rotate: th2 becomes th1 */
/* finger pad weights g on points lo..hi (a in 0..1); K, R are the TRANSVERSE pad constants,
 * the torsional ones K r_s^2, R r_s^2 are derived here. lo > hi clears it. */
void torsion_set_finger(Torsion *t, int lo, int hi, const double *g, double a, double K, double R);
/* numerical energy per unit I_p between th1 (n) and th0 (n-1) */
double torsion_energy(const Torsion *t);
#endif
