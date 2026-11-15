/* The ONE table of string parameters. Values are provisional; the research
 * agent updates them from /Users/alex/Music/foolish_cities_v5/alt_cello/REFERENCES.md.
 * Edit only this table. */
#include "string_params.h"
#include <math.h>
#include <string.h>

#define L_ALL 0.69
#define E_ALL 2.0e11
#define S0_ALL 1.2
#define S1_ALL 3e-4

static const StringSpec table[] = {
    /* name f0       T      r        L      E      sigma0  sigma1 */
    {"A3", 220.00, 160.0, 0.20e-3, L_ALL, E_ALL, S0_ALL, S1_ALL},
    {"D3", 146.83, 140.0, 0.24e-3, L_ALL, E_ALL, S0_ALL, S1_ALL},
    {"G2", 98.00, 130.0, 0.30e-3, L_ALL, E_ALL, S0_ALL, S1_ALL},
    {"C2", 65.41, 135.0, 0.36e-3, L_ALL, E_ALL, S0_ALL, S1_ALL},
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

double string_I(const StringSpec *sp) { return M_PI * pow(sp->r, 4) / 4.0; }

double string_B(const StringSpec *sp)
{
    return M_PI * M_PI * M_PI * sp->E * pow(sp->r, 4) / (4.0 * sp->T * sp->L * sp->L);
}
