/* One bowed, fingered string: the stage 1 simulation. */
#ifndef ALTCELLO_SIM_H
#define ALTCELLO_SIM_H
#include "bow.h"
#include "control.h"
#include "finger.h"
#include "rng.h"
#include "altstring.h"

#define SIM_NCH 5
/* channels: bridge force u, bridge force w, string velocity at the bow (u),
 * v_rel, bow friction force (N) */
extern const char *const sim_channel_names[SIM_NCH];

typedef struct {
    String *s;
    Rng rng;
    Finger finger;
    BowNoise noise;
    double fs, k;
    long n;                 /* steps done; state y1 is at time n k */
    Env vB, FB, beta, xF, aF;
    double bow_width;
    int bow_on;             /* 0: no bow at all */
    double bow_noise_level; /* std of raw draws; set before sim_start */
    double vrel_prev;
    int stuck;
    double *fwbuf;          /* w load density, N+1, zero between steps */
    /* last outputs */
    double out[SIM_NCH];
} Sim;

typedef struct {
    long n;
    double fs;
    double *ch[SIM_NCH];
} Trace;

void sim_init(Sim *S, const StringSpec *sp, double fs);
void sim_free(Sim *S);
void sim_start(Sim *S); /* call after setting controls and bow_noise_level */
void sim_step(Sim *S);
void sim_pluck(Sim *S, double x0, double halfwidth, double amp); /* initial u shape, no velocity */
typedef void (*SimCb)(const Sim *S, long i, void *user);
Trace *sim_run(Sim *S, double seconds, SimCb cb, void *user);
void trace_free(Trace *t);
/* convenient set-up: constant bow with a linear ramp-in of vB and FB */
void sim_set_bow(Sim *S, double beta, double vB, double FB, double ramp);
#endif
