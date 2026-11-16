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
#define SIM_MAXSTR INSTR_MAXSTR
/* channels: bridge force u, bridge force w (of the bowed string, T y_x - E I y_xxx),
 * string velocity at the bow (u), v_rel, bow friction force (N),
 * bridge velocity u, bridge velocity w, radiated pressure (radiate_body.h; 0 without a body) */
extern const char *const sim_channel_names[SIM_NCH];
typedef struct {
    int body_on;      /* modal body on the bridge */
    int all_strings;  /* 1: all four strings on the bridge, 0: only the bowed one */
    int lossless;     /* strings sigma0 = sigma1 = zeta_t = 0 and body Q = infinity (energy test) */
    const BodyOpts *body_opts; /* NULL: the instrument's default body knobs (body_modes.h) */
} SimOpts;

typedef struct {
    const Instrument *inst;
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
    const PlayTracks *play;         /* NULL: the Env controls above; else dense contours of a played passage (control.h) that
                                       replace them all: bow, the bowed string and the fingers of every string (needs all_strings) */
    Env xFo[SIM_MAXSTR], aFo[SIM_MAXSTR]; /* fingers of the other strings (by str[] index) */
    double bow_width;
    int bow_on;             /* 0: no bow at all */
    Hair hair;              /* compliant bow-hair contact (bow.h), off by default; set with hair_init before sim_start */
    int torsion_on;         /* 1 (default): the bow contact includes the string's torsion (torsion.h); 0: transverse only */
    double bow_noise_level; /* std of raw draws; set before sim_start */
    /* playing gestures (diagnosis knobs, all off by default): sinusoidal finger vibrato of the bowed string and slow random
       walks of the bow force and speed, both in logical time */
    double vib_depth_c, vib_rate, vib_t0; /* peak cents, Hz, s; depth 0 = off */
    double walk_sd, walk_bw;              /* relative sd and bandwidth (Hz) of the F_B and v_B walks; sd 0 = off */
    double *walk_F, *walk_v;              /* tables at SIM_WALK_RATE per second */
    long walk_n;
    double vrel_prev;
    int stuck;
    double *fwbuf;          /* w load density, N+1, zero between steps */
    /* diagnostic decomposition (sim_enable_shadow): one extra body per string driven by that string's bridge force alone. The body is
       linear, so the shadow pressures add up to the radiated pressure out[7] (test_shadow): the share of each string. */
    Body *shadow[SIM_MAXSTR];
    double out_pstr[SIM_MAXSTR]; /* radiated pressure due to string i alone, at the step's time */
    double out_fstr[SIM_MAXSTR]; /* bridge force (u polarisation) of string i, N, at time n */
    double out[SIM_NCH];
} Sim;

typedef struct {
    long n;
    double fs;
    double *ch[SIM_NCH];
} Trace;

/* one string, rigid bridge, no body (stage 1) */
void sim_init(Sim *S, const Instrument *I, const StringSpec *sp, double fs);
/* the instrument; bowed_name is the name of one of its strings (cello: "C2" "G2" "D3" "A3") */
void sim_init_instrument(Sim *S, const Instrument *I, double fs, const SimOpts *o, const char *bowed_name);
void sim_free(Sim *S);
/* build the per-string shadow bodies (needs a body); opts NULL: the instrument's default body knobs; 0 on success */
int sim_enable_shadow(Sim *S, const BodyOpts *opts);
/* slow random walks of F_B and v_B: relative sd, bandwidth Hz (one-pole), table covering `seconds`; draws from ALTCELLO_WALK_SEED
   at SIM_WALK_RATE per second of logical seconds, independent of fs. Call before sim_start. */
#define SIM_WALK_RATE 100
/* finger position of a string of length L stopped at xF after a pitch shift of `cents` (vibrato) */
double sim_vib_finger(double L, double xF, double cents);
void sim_set_walk(Sim *S, double sd, double bw_hz, double seconds);
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
