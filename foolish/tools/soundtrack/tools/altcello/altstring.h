/* Stiff lossy string, two polarisations (0 = u horizontal, bowing direction;
 * 1 = w vertical), explicit finite differences, simply supported ends.
 *
 *   rho y_tt = T y_xx - E I y_xxxx - 2 s0 rho y_t + 2 s1 rho y_txx + f
 *
 * Scheme (Bilbao NSS ch. 7, sigma0 centred, sigma1 backward), with the
 * finger term implicit and centred, per point (see finger.c):
 *   D_l y2_l = 2 y1_l + (c^2k^2 + 2 s1 k) dxx y1 - kap^2 k^2 dxxxx y1
 *              - (1 - s0 k) y0_l - 2 s1 k dxx y0 - C_l y0_l + k^2 f_l / rho
 *   D_l = 1 + s0 k + gam_l (K/2 + R/(2k)),  C_l = gam_l (K/2 - R/(2k)),
 *   gam_l = k^2 a g_l / rho.
 * y0 = time n-1, y1 = n, y2 = n+1. Arrays are padded with 2 ghost points on
 * each side (index -2 .. N+2); ghosts hold the odd reflection of the
 * simply supported ends (y = y_xx = 0). */
#ifndef ALTCELLO_ALTSTRING_H
#define ALTCELLO_ALTSTRING_H
#include <stddef.h>
#include "string_params.h"

typedef struct {
    StringSpec sp;
    double fs, k, h, rho, c2, kap2, EI;
    double s0, s1;      /* loss parameters in use (tests may override) */
    int N;              /* grid points 0..N, y_0 = y_N = 0 */
    double *store[2][3];
    double *y0[2], *y1[2], *y2[2]; /* point at index 0 of the physical grid */
    double *invD, *C;   /* per point, length N+1 */
    double kk;          /* k^2 / rho */
    int fing_lo, fing_hi;
} String;

String *string_new(const StringSpec *sp, double fs);
void string_free(String *s);
double string_h_min(const StringSpec *sp, double fs); /* stability limit */
/* free update y2 = f(y1, y0) in polarisation pol; fext is a force density
 * in N/m per point (NULL = none) */
void string_step_free(String *s, int pol, const double *fext);
/* make y2 the new y1 (rotate buffers) and refresh ghost points */
void string_commit(String *s);
void string_ghosts(String *s, int pol, double *y);
/* finger contact on points lo..hi with weights g (a in 0..1); K in N/m^2,
 * R in N s/m^2. lo > hi clears it. */
void string_set_finger(String *s, int lo, int hi, const double *g, double a, double K, double R);
/* output: T y_x - E I y_xxx at L from state y (includes y[N], the bridge end);
   the force on the bridge is the negative of it */
double string_bridge_force(const String *s, const double *y);
/* numerical energy (Bilbao) between states cur (n+1) and prev (n), per
 * unit of rho (cur[N], prev[N] may be nonzero bridge displacements; the
 * energy then includes the string-to-bridge link): 1/2|dt cur|^2 + c^2/2 <dx+ cur, dx+ prev> + kap^2/2 <dxx cur, dxx prev> */
double string_energy(const String *s, const double *cur, const double *prev);
#endif
