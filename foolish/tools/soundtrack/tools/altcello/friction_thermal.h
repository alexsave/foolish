/* Thermal elasto-plastic bow friction, a selectable alternative to the friction curve of bow.h.
 *
 * Source: M. van Walstijn, V. Chatziioannou, A. Lampis, E. Matusiak, "A thermal elasto-plastic friction model for bowed-string
 * simulation", Acta Acustica 10, 47 (2026) [vW26]. Equation numbers below are the paper's. The lumped version is used: ONE
 * bristle state z and ONE contact temperature tau for the whole bow contact (the paper uses N = 4 points under the bow and finds
 * little difference with more; our bow is 1 cm over a few grid points and v_rel is the weighted contact velocity of bow.h, so one
 * lumped contact fits).
 *
 * Model (per unit length of the contact; Fb = F_B / bow width, N/m; v = relative velocity of bow and string surface):
 *   friction force per unit length   F = Fb sigma0 z                                              (8)
 *   bristle (elasto-plastic) state   dz/dt = v [ 1 - alpha(z,v,tau) z / zss(v,tau) ]              (9)
 *   steady bristle displacement      zss(v,tau) = y(tau) mustr(v) / sigma0                        (10)
 *   adhesion map                     alpha = 0 for v z <= 0 or |z| <= zba; 1 for |z| >= |zss|; else
 *                                    (1 + sin(pi theta)) / 2, theta = (|z| - (|zss| + zba)/2) / (|zss| - zba)   (11)-(14)
 *                                    zba = chi y(tau) mud / sigma0 (break-away displacement)
 *   frictional heat                  Qf = Fb sigma0 z varsigma, varsigma = alpha z v / zss >= 0     (20), (21)
 *   heat balance                     a_tau Fb dtau/dt + [ b_tau sqrt(|v| Fb) + c_tau Fb ] tau = Qf  (22), (A.4)
 *   temperature factor               y(tau)    = (1 + ya (tau/tau_g)^xi) / (1 + (tau/tau_g)^xi)    (30)
 *   steady friction coefficient      muss(tau) = (mus + mud ya (tau/tau_g)^xi) / (1 + (tau/tau_g)^xi)  (31)
 *   Stribeck coefficient             mustr(tau) = muss / y = (mus + mud ya r) / (1 + ya r), r = (tau/tau_g)^xi   (32)
 *   steady map tau <-> v             [ b_tau sqrt(v/Fb) + c_tau ] tau = muss(tau) v                  (28), Appendix B
 *                                    v(tau) = (1/Fb) [ (b_tau tau + sqrt(b_tau^2 tau^2 + 4 c_tau tau muss Fb)) / (2 muss) ]^2   (B.2)
 * mustr(v) = mustr(tau_ss(v)) is the Stribeck curve of the model: it is read from a table built from (B.2) (v(tau) is monotone),
 * rebuilt when Fb changes by more than 1 percent.
 *
 * DISCREPANCY in the paper's text as extracted: eq (22) prints sqrt(|v|/Fb) in the bracket, but (24) and (A.4) give the conduction
 * term kappa_r tau sqrt(2 l_c |v| / (pi D_r)) = b_tau sqrt(|v| Fb) tau (l_c = Fb/p_b), and (28) is (22) divided by Fb:
 * [ b_tau sqrt(v/Fb) + c_tau ] tau = muss v. We use the dimensionally consistent form (22) = a_tau dtau/dt + [ b_tau sqrt(|v|/Fb) +
 * c_tau ] tau = sigma0 z varsigma (Qf / Fb), which reproduces (28). With the printed bracket the steady temperature at Table 1
 * would be about 300 K.
 *
 * Constants (ThermalSpec in instrument.h, set in instrument_cello.c): Table 1 of [vW26] (cello G string): mus 1.05, mud 0.3,
 * sigma0 1e6 /m, tau_g 25 K, ya 0.4, xi 2.0, chi 0.5, a_tau 1e-6, b_tau 0.22, c_tau 1e-4. Fig 4 (steady curves) uses xi 3 and
 * b_tau 0.05 for Fb = 100 N/m; Fig 7 varies b_tau by a factor 2 around Table 1's 0.22. The Table 1 values are the simulation ones.
 *
 * Time stepping. The string solve (sim.c) is explicit; the friction is the scalar junction equation  v = q - G Phi(v),  Phi =
 * sigma0 z+(v) the friction coefficient (force / F_B), z+(v) the bristle state at the end of the step as a function of the step's
 * relative velocity v (held constant over the step). z+ and the temperature are advanced by NSUB substeps h = dt / NSUB, each
 * linearly implicit (unconditionally stable, 0 <= alpha <= 1):
 *   z' = (z + h v) / (1 + h alpha |v| / |zss|),  alpha evaluated at the predictor z + h v (so a step that crosses zss uses
 *                                                 alpha = 1 and stays bounded by |zss|),
 *   tau' = (tau + (h / a_tau) sigma0 alpha |v| z'^2 / |zss|) / (1 + (h / a_tau)(b_tau sqrt(|v|/Fb) + c_tau)),
 * with y(tau) and the table value mustr(v) frozen over the substep. The bristle relaxes in |zss| / |v| (about 1 us at 0.5 m/s, less
 * than the 10.4 us step at 96 kHz): it is stiff, the implicit form is the stability criterion, NSUB is the accuracy criterion (set by
 * convergence in the tests: see TESTS.md). The scalar equation is solved by Illinois regula falsi inside the bracket
 * q -+ G Phimax (Phi is bounded by about mus), starting from the previous step's velocity to stay on the continuous branch. No
 * stick/slip hysteresis rule is needed: sticking is the pre-sliding creep v ~ dz/dt of the bristle.
 *
 * Bow noise: a multiplicative fluctuation (1 + n) of the friction FORCE (not of the state z), n the BowNoise of bow.h, switched in
 * by a ramp on the relative speed: 0 below v_slip, 1 above 2 v_slip (the friction curve model multiplies only the sliding branch).
 *
 * Contact: below F_B = 1e-3 N the contact is off (z = 0, the temperature relaxes with its v = 0 rate c_tau / a_tau). Fb is floored at
 * 2 N/m in the heat constants (F_B -> 0 would send b_tau sqrt(|v|/Fb) to infinity).
 */
#ifndef ALTCELLO_FRICTION_THERMAL_H
#define ALTCELLO_FRICTION_THERMAL_H
#include "instrument.h"

#define TH_NTAB 256
#define TH_FB_MIN 2.0       /* N/m floor of Fb in the heat constants */
#define TH_F_CONTACT 1.0e-3 /* N: below this bow force there is no contact */

typedef struct {
    ThermalSpec p;
    double width; /* bow width, m: Fb = F_B / width */
    double dt;    /* s, one audio step */
    double z, tau;    /* state at the end of the last step: bristle displacement (m, signed like v), contact temperature (K above ambient) */
    double vprev;     /* relative velocity of the last step (starting point of the root search) */
    double qdiss;     /* last step: dissipated friction power, W (F_B sigma0 z varsigma) */
    double Hz;        /* last step: bristle elastic energy, J (0.5 F_B sigma0 z^2 width) */
    double fb_tab;    /* Fb the table was built for (0: none) */
    double tv[TH_NTAB], tm[TH_NTAB]; /* v(tau_k), mustr(tau_k) */
} Thermal;

void thermal_init(Thermal *T, const ThermalSpec *p, double width, double dt);
void thermal_reset(Thermal *T);

/* the curves of the paper (no state) */
double thermal_y(const ThermalSpec *p, double tau);       /* (30) */
double thermal_mu_ss(const ThermalSpec *p, double tau);   /* (31) */
double thermal_mu_str_tau(const ThermalSpec *p, double tau); /* (32) */
double thermal_v_of_tau(const ThermalSpec *p, double Fb, double tau); /* (B.2) */
/* the steady contact temperature at a constant v > 0 by Newton on (B.3) (independent of the table), and muss there */
double thermal_tau_of_v(const ThermalSpec *p, double Fb, double v);
double thermal_mu_ss_of_v(const ThermalSpec *p, double Fb, double v);

/* One audio step of the contact: solve v = q - G Phi(v) (q, G as in bow.c: G includes F_B), advance z and tau. noise: relative
   fluctuation n of the friction force. Returns v_rel, *Phi = friction coefficient (force / F_B, signed like v). */
double thermal_solve(Thermal *T, double F_B, double q, double G, double noise, double *Phi);
/* Advance the state by ONE audio step at a GIVEN relative velocity v (no string): for the validation tests and the steady map.
   Returns the friction coefficient sigma0 z at the end of the step. */
double thermal_advance(Thermal *T, double F_B, double v);
#endif
