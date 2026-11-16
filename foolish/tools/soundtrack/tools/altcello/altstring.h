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
 *
 * Viscoelastic relaxation loss (standard linear solid, richness iteration 1): the loss term 2 s1 dxx (dt- y) of the scheme above acts on a LOW-PASSED
 * velocity, 2 sum_i s1_i dxx g_i with
 *   g_i^n = a_i g_i^{n-1} + (1 - a_i) (y^n - y^{n-1}) / k,   a_i = exp(-k / tau_i)
 * (g_i is the strain relaxation: a one-pole low-pass of the velocity, corner 1 / (2 pi tau_i), unit gain at DC). It is the force of a spring E_i = 2 s1_i rho / tau_i
 * in series with a dashpot, in parallel with the string tension (the Zener model). Continuous: the amplitude decay rate of a partial is
 *   alpha(w) = s0 + sum_i  s1_i (w / c)^2 / (1 + (w tau_i)^2)  = s0 + sum_i A_i x_i^2 / (1 + x_i^2),  A_i = s1_i / (c tau_i)^2,  x_i = w tau_i:
 * it grows like w^2 below the corner and SATURATES at A_i above it, so a few terms with corners spread over the audio band give a decay rate that grows like
 * w (a constant quality factor) instead of the w^2 (Q falling like 1/f) of s1 alone. The same terms add a small dispersion (a relative frequency rise
 * below A_i tau_i, 0.2 percent at most, Kramers-Kronig).
 * ENERGY (proved in TESTS.md T2r, tested by test_string_free and test_body_energy): with D^n = dx+ (y^n - y^{n-1}) / k, G_i^n = dx+ g_i^n and the Bilbao energy H
 * of the lossless scheme, the quantity
 *   E = H + k sum_i s1_i ( <G_i^n, D^{n+1}> + (1 + a_i) / (2 (1 - a_i)) |G_i^n|^2 )
 * never increases (the extra terms are the energy stored in the relaxation); the proof pairs the loss with the centred velocity and uses that
 * the dissipation of step n is s1 k (1 - a) ... precisely (s1 k / (1 - a)) (|G^n + G^{n-1}|^2 (1 - a) / 2) >= 0 for 0 <= a < 1 (x^2 + (1-a) x z - a z^2 - ((1+a)/2)(x^2 - z^2) = (1-a)(x+z)^2/2).
 * y0 = time n-1, y1 = n, y2 = n+1. Arrays are padded with 2 ghost points on
 * each side (index -2 .. N+2); ghosts hold the odd reflection of the
 * simply supported ends (y = y_xx = 0). */
#ifndef ALTCELLO_ALTSTRING_H
#define ALTCELLO_ALTSTRING_H
#include <stddef.h>
#include "instrument.h"
#include "torsion.h"

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
    Torsion *tor;       /* torsional motion of this string, own grid (torsion.h) */
    int nrel;           /* relaxation terms in use */
    double rel_s[STR_NREL];  /* s1_i in use (tests may override) */
    double rel_a[STR_NREL];  /* one-pole coefficient exp(-k / tau_i) */
    double *g[2][STR_NREL];  /* low-passed velocity per polarisation and term, points 0..N */
    double *gs;         /* scratch: sum_i s1_i g_i, points 0..N */
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
