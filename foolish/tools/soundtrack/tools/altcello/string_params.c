/* The ONE table of string parameters. Edit only this table.
 * Sources: /Users/alex/Music/foolish_cities_v5/alt_cello/REFERENCES.md.
 *
 * Bending stiffness (stage 2 iteration 2): cello strings are wound, the winding
 * adds mass but almost no stiffness, so a solid steel core of the string's
 * diameter (stage 1) is far too stiff.
 *   G2: EI = 3.03e-4 N m^2, MEASURED (Thomastik Dominant G, REFERENCES.md 2.2,
 *       from a search snippet of an Acta Acustica 2024 table: B = 4.8e-5 here).
 *   A3, D3, C2: ASSUMED, chosen for B = 2.5e-5, 3.5e-5, 8.0e-5 (consistent with
 *       the measured G and with the order of the "B = 3e-4" cello D example of
 *       J. O. Smith, REFERENCES.md 2.3, being a plain-core upper bound).
 *
 * Torsion (ASSUMED from typical published ratios, Woodhouse and Galluzzo
 * 2004 / Galluzzo thesis 2003, REFERENCES.md 2.4: torsional damping about ten
 * times the transverse one, zeta about 0.01; torsional wave speed several
 * times the transverse one; torsional impedance of the same order as Z0):
 *   c_t = 5.0 c, zeta_t = 0.01, surface torsional impedance 3.0 Z0.
 * The outer radii r are ASSUMED typical wound-string radii (Dominant G is 1.15 mm
 * in diameter, measured). */
#include "string_params.h"
#include <math.h>
#include <string.h>

#define L_ALL 0.69
#define S0_ALL 1.2
#define S1_ALL 3e-4
#define CT_RATIO 5.0
#define ZETA_T 0.01
#define ZT_RATIO 3.0
#define EI_B(B, T) ((B) * (T) * L_ALL * L_ALL / (M_PI * M_PI))

static const StringSpec table[] = {
    /* name f0       T      r       L      EI                  sigma0  sigma1  ct     zeta_t  zt */
    {"A3", 220.00, 160.0, 0.50e-3, L_ALL, EI_B(2.5e-5, 160.0), S0_ALL, S1_ALL, CT_RATIO, ZETA_T, ZT_RATIO},
    {"D3", 146.83, 140.0, 0.57e-3, L_ALL, EI_B(3.5e-5, 140.0), S0_ALL, S1_ALL, CT_RATIO, ZETA_T, ZT_RATIO},
    {"G2", 98.00, 130.0, 0.575e-3, L_ALL, 3.03e-4, S0_ALL, S1_ALL, CT_RATIO, ZETA_T, ZT_RATIO},
    {"C2", 65.41, 135.0, 0.80e-3, L_ALL, EI_B(8.0e-5, 135.0), S0_ALL, S1_ALL, CT_RATIO, ZETA_T, ZT_RATIO},
};

const StringSpec *string_spec(const char *name)
{
    for (unsigned i = 0; i < sizeof table / sizeof table[0]; i++)
        if (!strcmp(table[i].name, name)) return &table[i];
    return NULL;
}

double string_rho(const StringSpec *sp)
{
    double c = 2.0 * sp->L * sp->f0;
    return sp->T / (c * c);
}

double string_B(const StringSpec *sp) { return M_PI * M_PI * sp->EI / (sp->T * sp->L * sp->L); }

double string_EI_for_B(double B, double T, double L) { return B * T * L * L / (M_PI * M_PI); }

double string_ct(const StringSpec *sp) { return sp->ct_ratio * 2.0 * sp->L * sp->f0; }

double string_Ip(const StringSpec *sp)
{
    double Z0 = sqrt(sp->T * string_rho(sp));
    return sp->zt_ratio * Z0 * sp->r * sp->r / string_ct(sp);
}
