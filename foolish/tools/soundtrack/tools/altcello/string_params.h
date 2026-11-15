#ifndef ALTCELLO_STRING_PARAMS_H
#define ALTCELLO_STRING_PARAMS_H

typedef struct {
    const char *name;
    double f0;     /* open string pitch, Hz */
    double T;      /* tension, N */
    double r;      /* outer (surface) radius of the wound string, m: where the bow touches (torsion lever arm) */
    double L;      /* vibrating length nut to bridge, m */
    double EI;     /* bending stiffness, N m^2 (a wound string: the winding adds mass, not stiffness) */
    double sigma0; /* frequency independent loss, 1/s */
    double sigma1; /* frequency dependent loss, m^2/s */
    double ct_ratio; /* torsional wave speed / transverse wave speed */
    double zeta_t;   /* torsional damping ratio at the torsional fundamental (sigma_t = zeta_t w_t1); 0 = lossless */
    double zt_ratio; /* torsional characteristic impedance at the surface / Z0 = sqrt(T rho) */
} StringSpec;

const StringSpec *string_spec(const char *name); /* "A3" "D3" "G2" "C2", NULL if unknown */
double string_rho(const StringSpec *sp);         /* T / (2 L f0)^2, kg/m */
double string_B(const StringSpec *sp);           /* pi^2 EI / (T L^2) */
double string_EI_for_B(double B, double T, double L); /* EI = B T L^2 / pi^2 */
double string_ct(const StringSpec *sp);          /* torsional wave speed, m/s */
double string_Ip(const StringSpec *sp);          /* torsional inertia per length, kg m: Z_t,surface = Ip c_t / r^2 = zt_ratio Z0 */

#endif
