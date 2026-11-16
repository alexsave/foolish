/* Modal bridge body: m_k (q'' + (w_k/Q_k) q' + w_k^2 q) = c_ku F_u + c_kw F_w.
 * Leapfrog in time with the stiffness pre-warped so every mode has exactly its
 * table frequency (k^2 w_eff^2 = 4 sin^2(w k / 2)); the damping is centred and
 * implicit. With the strings' conservative scheme this is one conservative
 * leapfrog of the whole instrument (see sim.c). */
#ifndef ALTCELLO_BODY_H
#define ALTCELLO_BODY_H
#include "body_modes.h"
#include "radiate_body.h"

typedef struct {
    int n;
    double fs, k;
    double *A, *B, *Gu, *Gw;     /* q2 = A q1 + B q0 + Gu Fu + Gw Fw */
    double *q0, *q1, *q2;
    double *m, *w2, *cu, *cw, *r; /* mass, prewarped w^2, participations, radiation weights (compact strengths / plate signs, body_modes.h) */
    double *rp;      /* r for the plate modes, 0 for the compact ones */
    int nc;          /* number of compact modes (the first nc) */
    Radiator rad;    /* the physical radiation filters (radiate_body.h) */
    /* outputs of the last step */
    double zu, zw;   /* bridge displacement at n+1 */
    double vu, vw;   /* bridge velocity (q2 - q0)/2k at n */
    double p_compact, p_plate; /* the two paths separately (p_plate without G_PLATE), diagnostics */
    double p;        /* radiated pressure at n: compact path + plate path of the modal velocities (radiate_body.h) */
    /* the solved FE body (fe_body.h), nfe > 0: the first nfe modes have a per-string notch participation; body_step_str drives it */
    int nfe, variant;   /* variant: 1 FE-A, 2 FE-B (instrument.h) */
    double *Gus, *Gws, *cus, *cws; /* [BODY_NS][nfe] string-major */
    double *wc, *wp, *wv;          /* [nfe] FE-B compact weight, FE-B plate weight, FE-A monopole weight r_net */
    double *tq, *tdq;              /* [nfe] scratch: the new q and q - q0 */
    double gain_a;                 /* FE-A: p_fe = gain_a rho / (4 pi 1 m) V'' */
    double zus[BODY_NS], zws[BODY_NS], vus[BODY_NS], vws[BODY_NS]; /* per string: bridge displacement at n+1, velocity at n */
    double p_fe;                   /* FE-A monopole pressure of the last step (diagnostic) */
} Body;

/* opts: NULL for the instrument's default knobs (only opts->fc is used here) */
Body *body_new(const Instrument *I, const BodyModes *bm, double fs, int lossless, const BodyOpts *opts);
void body_free(Body *b);
void body_step(Body *b, double Fu, double Fw); /* force on the bridge at time n */
/* the FE body: force Fu[s], Fw[s] on the notch of instrument string s (BODY_NS arrays) */
void body_step_str(Body *b, const double *Fu, const double *Fw);
double body_energy(const Body *b);             /* between the last two states, J */
#endif
