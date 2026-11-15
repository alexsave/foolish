/* Bow: friction law, contact weights, scalar friction solve, bow noise. */
#ifndef ALTCELLO_BOW_H
#define ALTCELLO_BOW_H
#include "rng.h"
#include "altstring.h"
#include "instrument.h"

#define BOW_NOISE_RATE 4000 /* noise draws per simulated second: one every 0.25 ms */
#define BOW_MAXPTS 64

double bow_phi(const Friction *fr, double v);   /* friction curve, odd (constants: the instrument's BowSpec) */
double bow_dphi(const Friction *fr, double v);  /* d phi / d |v| for v > 0, negative */

/* raised-cosine bow weights J (1/m, sum J h = 1) on points lo..hi */
void bow_weights(const String *s, double xB, double width, int *lo, int *hi, double *J);
/* the same on any grid of spacing h, points 0..N (the torsion grid) */
void bow_weights_grid(double h, int N, double xB, double width, int *lo, int *hi, double *J);

/* Solve  v = q - G phi(v)  (G >= 0) with the Friedlander construction.
 * stuck_prev: the previous step was sticking. Returns v; *Phi = phi value
 * (the friction coefficient force/F_B); *stuck = 1 if sticking now. See bow.c. */
double bow_solve(const Friction *fr, double q, double G, double vprev, int stuck_prev, double *Phi, int *stuck);

/* Compliant bow-hair contact (stage 2 iteration 3, an option, default off; idea source Pitteroff and Woodhouse 1998,
 * the numbers are a parameter study, ASSUMED): the hair contact point is a small mass m on a spring k and damper r to
 * the bow stick, which moves at vB. With d the hair displacement relative to the stick, the friction force F phi on the
 * hair gives  m d'' + r d' + k d = F phi  (the stick acceleration is ignored: the ramp of vB gives 1.7 mN on 0.5 g),
 * and the contact slip velocity is v_rel = v_string - vB - d'. The hair is one more admittance in series at the
 * contact (g/(2 dt) per unit F phi, bow.c), so the friction solve stays scalar. */
#define HAIR_MASS 5.0e-4   /* kg */
#define HAIR_ZETA 0.3      /* damping ratio of the contact resonance used by hair_init */
typedef struct {
    int on;
    double m, k, r;        /* kg, N/m, N s/m */
    double A, B, g;        /* d2 = A d1 + B d0 + g F phi */
    double d0, d1, d2;
    double dprev;          /* d(n-1) after the commit (diagnostics: hair velocity at n = (d1 - dprev)/2k) */
    double dfree;          /* free update of this step */
} Hair;
void hair_init(Hair *h, double kh, double dt);   /* on = 1, m = HAIR_MASS, r from HAIR_ZETA */
void hair_reset(Hair *h);

typedef struct {
    double level;       /* std of the raw draws (0 = off) */
    double lp_a;        /* one-pole coefficient */
    double lp_y;
    double x_prev, x_next;
    long idx_next;      /* index of x_next */
    int started;
    long nlog;
    double log_t[1000], log_x[1000]; /* first draws with their logical times */
    double log_now[1000];            /* simulated time at which each draw was made */
} BowNoise;

void bow_noise_init(BowNoise *b, double level, double fs);
/* noise value n(t) for sample n of rate fs (t = n/fs, integer arithmetic for
 * the draw schedule so it is identical at every sample rate) */
double bow_noise_next(BowNoise *b, Rng *r, long n, double fs);
#endif
