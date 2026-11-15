#include "control.h"

void env_const(Env *e, double v)
{
    e->n = 1;
    e->t[0] = 0.0;
    e->v[0] = v;
}

void env_add(Env *e, double t, double v)
{
    if (e->n < ENV_MAX) {
        e->t[e->n] = t;
        e->v[e->n] = v;
        e->n++;
    }
}

double env_eval(const Env *e, double t)
{
    if (e->n == 0) return 0.0;
    if (t <= e->t[0]) return e->v[0];
    for (int i = 1; i < e->n; i++)
        if (t < e->t[i]) {
            double u = (t - e->t[i - 1]) / (e->t[i] - e->t[i - 1]);
            return e->v[i - 1] + u * (e->v[i] - e->v[i - 1]);
        }
    return e->v[e->n - 1];
}
