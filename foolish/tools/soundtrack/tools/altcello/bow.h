/* Bow: friction law, contact weights, scalar friction solve, bow noise. */
#ifndef ALTCELLO_BOW_H
#define ALTCELLO_BOW_H
#include "rng.h"
#include "string.h"

#define BOW_NOISE_RATE 4000 /* draws per second of simulated time (every 0.25 ms) */
#define BOW_MAXPTS 64
#define BOW_PHI_STATIC 1.2  /* phi(0+) = 0.4 + 0.45 + 0.35 */

double bow_phi(double v);   /* friction curve, odd */
double bow_dphi(double v);  /* d phi / d |v| for v > 0, negative */

/* raised-cosine bow weights J (1/m, sum J h = 1) on points lo..hi */
void bow_weights(const String *s, double xB, double width, int *lo, int *hi, double *J);

/* Solve  v = q - G phi(v)  (G >= 0) with the Friedlander construction.
 * stuck_prev: the previous step was sticking. Returns v; *Phi = phi value
 * (the friction coefficient force/F_B); *stuck = 1 if sticking now. See bow.c. */
double bow_solve(double q, double G, double vprev, int stuck_prev, double *Phi, int *stuck);

typedef struct {
    double level;       /* std of the raw draws (0 = off) */
    double lp_a;        /* one-pole coefficient */
    double lp_y;
    double x_prev, x_next;
    long idx_next;      /* index of x_next */
    int started;
    long nlog;
    double log_t[1000], log_x[1000]; /* first draws with their logical times */
} BowNoise;

void bow_noise_init(BowNoise *b, double level, double fs);
/* noise value n(t) for sample n of rate fs (t = n/fs, integer arithmetic for
 * the draw schedule so it is identical at every sample rate) */
double bow_noise_next(BowNoise *b, Rng *r, long n, double fs);
#endif
