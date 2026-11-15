#ifndef ALTCELLO_STRING_PARAMS_H
#define ALTCELLO_STRING_PARAMS_H

typedef struct {
    const char *name;
    double f0;     /* open string pitch, Hz */
    double T;      /* tension, N */
    double r;      /* core radius, m */
    double L;      /* vibrating length nut to bridge, m */
    double E;      /* Young's modulus of the core, Pa */
    double sigma0; /* frequency independent loss, 1/s */
    double sigma1; /* frequency dependent loss, m^2/s */
} StringSpec;

const StringSpec *string_spec(const char *name); /* "A3" "D3" "G2" "C2", NULL if unknown */
double string_rho(const StringSpec *sp);         /* T / (2 L f0)^2, kg/m */
double string_I(const StringSpec *sp);           /* pi r^4 / 4 */
double string_B(const StringSpec *sp);           /* pi^3 E r^4 / (4 T L^2) */

#endif
