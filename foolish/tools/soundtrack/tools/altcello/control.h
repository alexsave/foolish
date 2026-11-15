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
#endif
