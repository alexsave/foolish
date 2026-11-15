/* T16 physical radiation (radiate_body.h):
 *  a) the baffled-plate efficiency sigma(f) matches the REFERENCES 7.4 table (f_c 3 kHz), is clamped to 1 and never falls with f;
 *  b) the realised digital plate filter matches sqrt(sigma) within 1 dB on 50 Hz - 20 kHz for every fitted f_c (96 kHz) except in a
 *     window 0.85-1.1 x the knee where the hard clamp sigma = 1 starts (there below 2 dB); the other f_c values (knob tries) 1.5 / 2.5 dB; and the
 *     compact filter is the first-order high pass with its corner at ka = 1 (-3 dB at c / (2 pi a_eff) = 218 Hz, +6 dB/oct below);
 *  c) the plate signs are deterministic, +-1, and the compact modes keep their stage 2 strengths (A0 1.0, the others 0.7);
 *  d) with a white bridge force the compact and the plate paths have equal power in the octave 212-424 Hz when scaled by
 *     G_PLATE (the one relative level of the model), within 1 dB;
 *  e) the old failure: the radiated spectrum no longer carries the mass-line response of the low modes flat to 16 kHz: with a white
 *     bridge force the 3-8 kHz power spectral density sits more than 8 dB below the 212-424 Hz one (flat-acceleration model: 0 dB). */
#include "analysis.h"
#include "body.h"
#include "rng.h"
#include "test_common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define FS 96000.0

static void check_sigma(void)
{
    struct { double f, s; } ref[] = {{250, 0.044}, {500, 0.069}, {1000, 0.127}, {2000, 0.424}};
    for (int i = 0; i < 4; i++) {
        double s = radiate_plate_sigma(ref[i].f, 3000.0);
        printf("T16a sigma(%.0f Hz) = %.4f (REFERENCES 7.4: %.3f)\n", ref[i].f, s, ref[i].s);
        CHECK(fabs(s / ref[i].s - 1.0) < 0.03, "sigma(%.0f) = %.4f, table %.3f", ref[i].f, s, ref[i].s);
    }
    double prev = 0.0;
    for (double f = 20.0; f < 20000.0; f *= 1.003) {
        double s = radiate_plate_sigma(f, 3000.0);
        CHECK(s > 0.0 && s <= 1.0, "sigma(%.1f) = %g outside (0, 1]", f, s);
        CHECK(s >= prev - 1e-12, "sigma falls at %.1f Hz (%g after %g)", f, s, prev);
        prev = s;
    }
    CHECK(radiate_plate_sigma(3000.0, 3000.0) == 1.0 && radiate_plate_sigma(9000.0, 3000.0) == 1.0, "sigma not clamped to 1 at and above f_c");
}

static void check_filters(void)
{
    for (int i = 0; i < RADIATE_NFC; i++) {
        double fc = radiate_plate_fits[i].fc;
        Radiator R;
        CHECK(radiator_init(&R, FS, fc, 1.0) == 0, "no fit for f_c %.0f", fc);
        /* the hard clamp of sigma at 1 is a kink that six real sections cannot follow to 1 dB: the error is measured everywhere and
           outside a window around the knee (the first frequency where sigma = 1) */
        double fk = fc;
        for (double f = 50.0; f < fc; f *= 1.001)
            if (radiate_plate_sigma(f, fc) >= 1.0) { fk = f; break; }
        double worst = 0.0, wf = 0.0, worst_out = 0.0, wfo = 0.0;
        for (double f = 50.0; f <= 20000.0; f *= 1.01) {
            double err = fabs(20.0 * log10(radiator_mag(&R, 1, f) / sqrt(radiate_plate_sigma(f, fc))));
            if (err > worst) { worst = err; wf = f; }
            if ((f < 0.85 * fk || f > 1.1 * fk) && err > worst_out) { worst_out = err; wfo = f; }
        }
        printf("T16b plate filter f_c %.0f Hz (knee %.0f Hz): worst |error| %.3f dB at %.0f Hz, outside the knee window %.3f dB at %.0f Hz\n", fc, fk, worst, wf, worst_out, wfo);
        /* the shipped f_c (3000) is held to 1 dB outside the knee window; the other values only serve the knob tries (REPORT: up to 1.4 dB) */
        double lim_out = fabs(fc - RADIATE_FC_DEFAULT) < 0.5 ? 1.0 : 1.5, lim_all = fabs(fc - RADIATE_FC_DEFAULT) < 0.5 ? 2.0 : 2.5;
        CHECK(worst_out < lim_out, "f_c %.0f: plate filter off by %.3f dB at %.0f Hz (outside the knee)", fc, worst_out, wfo);
        CHECK(worst < lim_all, "f_c %.0f: plate filter off by %.3f dB at %.0f Hz", fc, worst, wf);
    }
    Radiator R;
    radiator_init(&R, FS, 3000.0, 1.0);
    double fcorner = RADIATE_C_AIR / (2.0 * M_PI * RADIATE_A_EFF);
    double m = 20.0 * log10(radiator_mag(&R, 0, fcorner));
    double s1 = 20.0 * log10(radiator_mag(&R, 0, 20.0) / radiator_mag(&R, 0, 10.0));
    double hi = 20.0 * log10(radiator_mag(&R, 0, 10000.0));
    printf("T16b compact filter: %.2f dB at the ka = 1 corner %.0f Hz, %.2f dB per octave at 10-20 Hz, %.3f dB at 10 kHz\n", m, fcorner, s1, hi);
    CHECK(fabs(m + 3.0103) < 0.3, "corner gain %.2f dB, want -3.01", m);
    CHECK(fabs(s1 - 6.02) < 0.1, "low-frequency slope %.2f dB/oct, want 6.02", s1);
    CHECK(fabs(hi) < 0.1, "high-frequency gain %.3f dB, want 0 (velocity like)", hi);
    CHECK(radiator_init(&R, FS, 3100.0, 1.0) != 0, "an unfitted f_c must be refused");
}

static void check_signs(void)
{
    BodyModes *a = body_modes_build(), *b = body_modes_build();
    int plus = 0, nplate = a->n - a->ncompact;
    CHECK(a->ncompact == RADIATE_NCOMPACT, "ncompact %d", a->ncompact);
    CHECK(a->r[0] == 1.0 && a->r[1] == 0.7 && a->r[2] == 0.7 && a->r[3] == 0.7, "compact strengths %g %g %g %g", a->r[0], a->r[1], a->r[2], a->r[3]);
    for (int i = 0; i < a->n; i++) {
        CHECK(a->r[i] == b->r[i], "mode %d: weight differs between two builds", i);
        if (i >= a->ncompact) {
            CHECK(a->r[i] == 1.0 || a->r[i] == -1.0, "plate mode %d weight %g is not +-1", i, a->r[i]);
            if (a->r[i] > 0) plus++;
        }
    }
    printf("T16c %d plate modes, %d positive signs\n", nplate, plus);
    CHECK(fabs(plus - 0.5 * nplate) < 5.0 * sqrt(0.25 * nplate), "signs unbalanced: %d of %d positive", plus, nplate);
    body_modes_free(a);
    body_modes_free(b);
}

static double band_power(const Spectrum *s, double flo, double fhi)
{
    double p = 0.0;
    for (int i = 0; i < s->n / 2; i++) {
        double f = i * s->df;
        if (f >= flo && f < fhi) p += s->mag[i] * s->mag[i];
    }
    return p;
}

static void check_paths(void)
{
    BodyModes *bm = body_modes_build();
    Body *b = body_new(bm, FS, 0, NULL);
    long n = 1L << 19; /* 5.5 s */
    double *pc = malloc(sizeof(double) * (size_t)n), *pp = malloc(sizeof(double) * (size_t)n);
    Rng r;
    rng_seed(&r, ALTCELLO_SEED);
    for (long i = 0; i < n; i++) {
        body_step(b, rng_gauss(&r), rng_gauss(&r));
        pc[i] = b->p_compact;
        pp[i] = b->p_plate;
    }
    Spectrum *sc = spec_make(pc, n, FS, 1), *sp = spec_make(pp, n, FS, 1);
    double c1 = band_power(sc, 212.0, 424.0), p1 = band_power(sp, 212.0, 424.0);
    double ratio_db = 10.0 * log10(c1 / p1);
    double g_db = 20.0 * log10(RADIATE_G_PLATE);
    printf("T16d white bridge force, octave 212-424 Hz: compact / plate power %.2f dB, so G_PLATE for equality = %.4f (compiled %.4f)\n", ratio_db, pow(10.0, ratio_db / 20.0), RADIATE_G_PLATE);
    CHECK(fabs(ratio_db - g_db) < 1.0, "paths differ by %.2f dB in 212-424 Hz after G_PLATE (%.2f dB)", ratio_db - g_db, g_db);
    /* e) total */
    for (long i = 0; i < n; i++) pc[i] += RADIATE_G_PLATE * pp[i];
    Spectrum *st = spec_make(pc, n, FS, 1);
    double lo = band_power(st, 212.0, 424.0), hi = band_power(st, 3000.0, 8000.0);
    double hi_db = 10.0 * log10((hi / 5000.0) / (lo / 212.0)); /* power per Hz */
    printf("T16e total pressure, white bridge force: 3-8 kHz power density %.1f dB re the 212-424 Hz band (flat-acceleration model: 0 dB)\n", hi_db);
    CHECK(hi_db < -8.0, "3-8 kHz density is only %.1f dB below the 212-424 Hz band", hi_db);
    spec_free(sc);
    spec_free(sp);
    spec_free(st);
    free(pc);
    free(pp);
    body_free(b);
    body_modes_free(bm);
}

int main(void)
{
    check_sigma();
    check_filters();
    check_signs();
    check_paths();
    TEST_DONE("test_radiate");
}
