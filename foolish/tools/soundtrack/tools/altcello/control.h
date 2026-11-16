/* Piecewise linear breakpoint control curves, held at both ends. */
#ifndef ALTCELLO_CONTROL_H
#define ALTCELLO_CONTROL_H
#define ENV_MAX 16
typedef struct {
    int n;
    double t[ENV_MAX], v[ENV_MAX];
} Env;
void env_const(Env *e, double v);
void env_add(Env *e, double t, double v);
double env_eval(const Env *e, double t);

/* Dense control tracks of a played passage (player.h): every control is a sampled contour at `rate` per second of LOGICAL time,
 * linearly interpolated, so the controls never depend on the audio sample rate. Held at both ends. */
#define TRK_NSTR 4 /* = INSTR_MAXSTR */
typedef struct {
    double rate;
    long n;
    double *vB, *FB, *beta;     /* bow speed (signed, m/s), force (N), relative position */
    signed char *bowed;         /* index of the bowed string (step function) */
    double *xF[TRK_NSTR];       /* finger position of each string, m from the nut */
    double *aF[TRK_NSTR];       /* finger press 0..1 of each string */
} PlayTracks;
double trk_eval(const double *a, long n, double rate, double t);
void trk_free(PlayTracks *p);
void trk_alloc(PlayTracks *p, double rate, long n);
#endif
