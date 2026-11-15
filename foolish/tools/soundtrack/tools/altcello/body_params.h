#ifndef ALTCELLO_BODY_PARAMS_H
#define ALTCELLO_BODY_PARAMS_H

typedef struct {
    int n;
    int nlead;                 /* the first nlead modes are the named table modes */
    double *f, *Q, *m, *cu, *cw, *r; /* Hz, -, kg (effective mass at the bridge), -, -, radiation weight r_k = R(f_k) (radiativity.h) */
} BodyModes;

BodyModes *body_modes_build(void); /* deterministic: statistical modes use ALTCELLO_BODY_SEED */
void body_modes_free(BodyModes *b);
const char *body_mode_name(int i); /* "A0" "CBR" "B1-" "B1+" ... */

#endif
