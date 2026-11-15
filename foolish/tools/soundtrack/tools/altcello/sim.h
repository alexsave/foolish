/* The instrument: up to four strings on one bridge, an optional modal body,
 * one bow acting on one string. Stage 1 users build a single rigid-bridge
 * string with sim_init. */
#ifndef ALTCELLO_SIM_H
#define ALTCELLO_SIM_H
#include "body.h"
#include "bow.h"
#include "control.h"
#include "finger.h"
#include "rng.h"
#include "altstring.h"

#define SIM_NCH 8
#define SIM_MAXSTR 4
/* channels: bridge force u, bridge force w (of the bowed string, T y_x - E I y_xxx),
 * string velocity at the bow (u), v_rel, bow friction force (N),
 * bridge velocity u, bridge velocity w, body pressure proxy (0 without a body) */
extern const char *const sim_channel_names[SIM_NCH];
extern const char *const sim_string_names[SIM_MAXSTR]; /* C2 G2 D3 A3 */

typedef struct {
    int body_on;      /* modal body on the bridge */
    int all_strings;  /* 1: all four strings on the bridge, 0: only the bowed one */
    int lossless;     /* strings sigma0 = sigma1 = zeta_t = 0 and body Q = infinity (energy test) */
    int flat_radiation; /* test switch: every mode radiates with weight 1 (the radiativity curve is ignored) */
} SimOpts;

typedef struct {
    String *str[SIM_MAXSTR];
    int nstr, bowed;        /* index of the bowed string in str[] */
    String *s;              /* = str[bowed] */
    Body *body;
    Rng rng;
    Finger finger;
    BowNoise noise;
    double fs, k;
    long n;                 /* steps done; state y1 is at time n k */
    Env vB, FB, beta, xF, aF;       /* bow and the finger of the bowed string */
    Env xFo[SIM_MAXSTR], aFo[SIM_MAXSTR]; /* fingers of the other strings (by str[] index) */
    double bow_width;
    int bow_on;             /* 0: no bow at all */
    int torsion_on;         /* 1 (default): the bow contact includes the string's torsion (torsion.h); 0: transverse only */
    double bow_noise_level; /* std of raw draws; set before sim_start */
    double vrel_prev;
    int stuck;
    double *fwbuf;          /* w load density, N+1, zero between steps */
    double out[SIM_NCH];
} Sim;

typedef struct {
    long n;
    double fs;
    double *ch[SIM_NCH];
} Trace;

/* one string, rigid bridge, no body (stage 1) */
void sim_init(Sim *S, const StringSpec *sp, double fs);
/* the instrument; bowed_name is "C2" "G2" "D3" or "A3" */
void sim_init_instrument(Sim *S, double fs, const SimOpts *o, const char *bowed_name);
void sim_free(Sim *S);
void sim_start(Sim *S); /* call after setting controls and bow_noise_level */
void sim_step(Sim *S);
void sim_pluck(Sim *S, double x0, double halfwidth, double amp); /* bowed string, u, no velocity */
void sim_pluck_string(Sim *S, int idx, int pol, double x0, double halfwidth, double amp);
/* initial torsion bump on string idx (angle amp rad, no angular velocity) */
void sim_pluck_torsion(Sim *S, int idx, double x0, double halfwidth, double amp);
typedef void (*SimCb)(const Sim *S, long i, void *user);
Trace *sim_run(Sim *S, double seconds, SimCb cb, void *user);
void trace_free(Trace *t);
/* the bow weights of the bowed string at relative position beta: transverse grid (lo, hi, J) and torsion grid (tlo, thi, Jt) */
void sim_bow_weights(const Sim *S, double beta, int *lo, int *hi, double *J, int *tlo, int *thi, double *Jt);
/* convenient set-up: constant bow with a linear ramp-in of vB and FB */
void sim_set_bow(Sim *S, double beta, double vB, double FB, double ramp);
/* total energy (J) of the strings (transverse, both polarisations, and torsion) and the body between the last two states */
double sim_energy(const Sim *S);
#endif
