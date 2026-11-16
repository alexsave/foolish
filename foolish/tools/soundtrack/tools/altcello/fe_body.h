/* The solved finite element body as an option behind a switch (experiment, ITERATION_LOG.md "FE body"): a loader for the BODYMODES 1 file of
 * body_fem (its INTERFACE.md is the contract) and the mode set built from it.
 *
 * Switch: Instrument.body.fe_variant (knob fe=0|1|2, knobs.h), the file Instrument.body.fe_path (knob fe_path=..., else FE_DEFAULT_PATH).
 *   0 STAT  the statistical body (the shipped default; nothing here is used)
 *   1 FE-A  FE modes below fe_fx, radiation = the monopole of the file: p = gain_a rho V'' / (4 pi d), V = sum_k r_net,k q_k
 *   2 FE-B  FE modes below fe_fx, radiation through our own filters: compact modes (air_share > 0.5 or f < 250 Hz) through the ka filter with the
 *           weight r_net,k / |r_net(A0)| (A0 = the first mode with air_share > 0.5, strength 1 like the shipped A0), every other mode through the
 *           plate efficiency filter with the weight gain_b sign(r_net,k) instead of a random sign
 * Both: the statistical modes above fe_fx are the shipped ones (same draws, same bridge-hill division, same random plate signs, same G_PLATE path); the
 * lead table modes and the statistical modes below fe_fx are dropped; the FE modes carry no bridge-hill division (the bridge is in the FE model).
 * Each string has its own notch participation (cu_s, cw_s) in the FE modes. u is flipped to our geometry (geom.c puts the bass side at -x, the mesh at +x);
 * w and r_net are unchanged.
 * gain_a and gain_b are one scale each, set so that the radiated pressure of the FE modes equals that of the shipped body in 2.0-2.5 kHz (white
 * bridge force on every string): measured by fe_tool and asserted by test_fe_body. */
#ifndef ALTCELLO_FE_BODY_H
#define ALTCELLO_FE_BODY_H
#include "body_modes.h"

typedef struct {
    int n;
    char names[BODY_NS][8];
    double rho, c;
    double *f, *eta, *Q, *m, *cu[BODY_NS], *cw[BODY_NS], *r_net, *air; /* as in the file (u not flipped) */
} FeFile;

/* strtod loader, no JSON. NULL (message in err, if not NULL, up to errn bytes) on any malformed file; free with fe_file_free */
FeFile *fe_file_load(const char *path, char *err, int errn);
void fe_file_free(FeFile *F);

/* radiated pressure under an independent white bridge force of unit variance on every string (the shipped body gets the sum): the mean power spectral
 * density in flo..fhi, arbitrary units, n steps at 96 kHz from ALTCELLO_SEED. Used to match the two FE variants to the shipped body (fe_tool, test_fe_body). */
double fe_white_psd(const Instrument *I, double flo, double fhi, long n);
#endif
