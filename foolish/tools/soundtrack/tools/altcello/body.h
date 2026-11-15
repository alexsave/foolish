/* Modal bridge body: m_k (q'' + (w_k/Q_k) q' + w_k^2 q) = c_ku F_u + c_kw F_w.
 * Leapfrog in time with the stiffness pre-warped so every mode has exactly its
 * table frequency (k^2 w_eff^2 = 4 sin^2(w k / 2)); the damping is centred and
 * implicit. With the strings' conservative scheme this is one conservative
 * leapfrog of the whole instrument (see sim.c). */
#ifndef ALTCELLO_BODY_H
#define ALTCELLO_BODY_H
#include "body_params.h"

typedef struct {
    int n;
    double fs, k;
    double *A, *B, *Gu, *Gw;     /* q2 = A q1 + B q0 + Gu Fu + Gw Fw */
    double *q0, *q1, *q2;
    double *m, *w2, *cu, *cw, *r; /* mass, prewarped w^2, participations, radiation weights */
    /* outputs of the last step */
    double zu, zw;   /* bridge displacement at n+1 */
    double vu, vw;   /* bridge velocity (q2 - q0)/2k at n */
    double p;        /* body pressure proxy sum r_k q_k'' at n */
} Body;

Body *body_new(const BodyModes *bm, double fs, int lossless);
void body_free(Body *b);
void body_step(Body *b, double Fu, double Fw); /* force on the bridge at time n */
double body_energy(const Body *b);             /* between the last two states, J */
#endif
