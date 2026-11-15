#include "instrument.h"
#include <math.h>
#include <string.h>

int instrument_string_index(const Instrument *I, const char *name)
{
    for (int i = 0; i < I->nstr; i++)
        if (!strcmp(I->str[i].name, name)) return i;
    return -1;
}

const StringSpec *instrument_string(const Instrument *I, const char *name)
{
    int i = instrument_string_index(I, name);
    return i < 0 ? NULL : &I->str[i];
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
