#include "radiate_body.h"
#include <math.h>
#include <string.h>

double radiate_plate_sigma(const RadiationSpec *rs, double f, double fc)
{
    double s;
    if (f >= fc) return 1.0;
    double a = sqrt(f / fc), lam = RADIATE_C_AIR / fc;
    double pre = rs->perim * lam / (4.0 * M_PI * M_PI * rs->area);
    double u = 1.0 - a * a;
    s = pre * (u * log((1.0 + a) / (1.0 - a)) + 2.0 * a) / (u * sqrt(u));
    return s < 1.0 ? s : 1.0;
}

double radiate_compact_mag(const RadiationSpec *rs, double f)
{
    double x = 2.0 * M_PI * f * rs->a_eff / RADIATE_C_AIR;
    return x / sqrt(1.0 + x * x);
}

int radiator_init(Radiator *R, const RadiationSpec *rs, double fs, double fc, double g_plate)
{
    memset(R, 0, sizeof *R);
    R->fs = fs;
    R->g_plate = g_plate;
    double c = 2.0 * fs * rs->a_eff / RADIATE_C_AIR; /* 2 fs tau */
    R->c_b0 = c / (1.0 + c);
    R->c_b1 = -c / (1.0 + c);
    R->c_a1 = (1.0 - c) / (1.0 + c);
    const PlateFit *fit = NULL;
    for (int i = 0; i < RADIATE_NFC; i++)
        if (fabs(radiate_plate_fits[i].fc - fc) < 0.5) fit = &radiate_plate_fits[i];
    if (!fit) return -1;
    R->ns = RADIATE_NSEC;
    R->K = fit->K;
    double c2 = 2.0 * fs;
    for (int i = 0; i < RADIATE_NSEC; i++) {
        double d = 1.0 + c2 / fit->p[i];
        R->p_b0[i] = (1.0 + c2 / fit->z[i]) / d;
        R->p_b1[i] = (1.0 - c2 / fit->z[i]) / d;
        R->p_a1[i] = (1.0 - c2 / fit->p[i]) / d;
    }
    return 0;
}

double radiator_step_compact(Radiator *R, double vc)
{
    double y = R->c_b0 * vc + R->c_z;
    R->c_z = R->c_b1 * vc - R->c_a1 * y;
    return y;
}

double radiator_step_plate(Radiator *R, double vp)
{
    double x = vp;
    for (int i = 0; i < R->ns; i++) {
        double y = R->p_b0[i] * x + R->p_z[i];
        R->p_z[i] = R->p_b1[i] * x - R->p_a1[i] * y;
        x = y;
    }
    return R->K * x;
}

double radiator_step(Radiator *R, double vc, double vp)
{
    return radiator_step_compact(R, vc) + R->g_plate * radiator_step_plate(R, vp);
}

static void section(double b0, double b1, double a1, double w, double *re, double *im)
{
    /* (b0 + b1 z^-1) / (1 + a1 z^-1) at z = e^{jw} */
    double c = cos(w), s = -sin(w); /* z^-1 = c + j s */
    double nr = b0 + b1 * c, ni = b1 * s, dr = 1.0 + a1 * c, di = a1 * s, d2 = dr * dr + di * di;
    *re = (nr * dr + ni * di) / d2;
    *im = (ni * dr - nr * di) / d2;
}

double radiator_mag(const Radiator *R, int path, double f)
{
    double w = 2.0 * M_PI * f / R->fs, re, im;
    if (path == 0) {
        section(R->c_b0, R->c_b1, R->c_a1, w, &re, &im);
        return hypot(re, im);
    }
    double mr = R->K, mi = 0.0;
    for (int i = 0; i < R->ns; i++) {
        section(R->p_b0[i], R->p_b1[i], R->p_a1[i], w, &re, &im);
        double nr = mr * re - mi * im, ni = mr * im + mi * re;
        mr = nr;
        mi = ni;
    }
    return hypot(mr, mi);
}
