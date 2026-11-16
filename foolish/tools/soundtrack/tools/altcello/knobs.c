#include "knobs.h"
#include "analysis.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* amplitude decay rate of a partial at 2 kHz is w / (2 Q) = sigma0 + sigma1 (w / c)^2 with c = 2 L f0 (instrument_cello.c Q2K_S1) */
static double q2k_of(const StringSpec *s)
{
    double c = 2.0 * s->L * s->f0, w = 2.0 * M_PI * 2000.0;
    return w / (2.0 * (s->sigma0 + s->sigma1 * (w / c) * (w / c)));
}
static void set_q2k(StringSpec *s, double Q)
{
    double c = 2.0 * s->L * s->f0, w = 2.0 * M_PI * 2000.0;
    s->sigma1 = (w / (2.0 * Q) - s->sigma0) / ((w / c) * (w / c));
}

int knobs_apply(Instrument *I, const char *spec)
{
    char *buf = strdup(spec), *save = NULL;
    int rc = 0;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) { fprintf(stderr, "knob %s: no =\n", tok); rc = 1; break; }
        *eq = 0;
        double v = atof(eq + 1);
        char *dot = strchr(tok, '.');
        const char *names = NULL;
        if (dot) { *dot = 0; names = dot + 1; }
        if (!strcmp(tok, "s0") && !names) {
            double q[INSTR_MAXSTR];
            for (int i = 0; i < I->nstr; i++) q[i] = q2k_of(&I->str[i]);
            for (int i = 0; i < I->nstr; i++) { I->str[i].sigma0 = v; set_q2k(&I->str[i], q[i]); }
        } else if (!strcmp(tok, "fifths") && !names) {
            /* the open strings tuned in pure fifths from A3 = 220 Hz (A 220, D 146.667, G 97.778, C 65.185); the finger position of a stopped
               note does not depend on f0, so the equal-tempered stopped notes land 2-4 cents off the open string in unison */
            static const double pf[4] = {65.1852, 97.7778, 146.6667, 220.0}; /* C G D A */
            if (v != 0.0)
                for (int i = 0; i < I->nstr && i < 4; i++) I->str[i].f0 = pf[i];
        } else if (!strcmp(tok, "bow") && !names) {
            /* bow=1 selects the thermal elasto-plastic friction, bow=0 the friction curve */
            I->bow.model = v != 0.0 ? BOW_THERMAL : BOW_CURVE;
        } else if (!strcmp(tok, "th_xi") && !names) {
            I->bow.thermal.xi = v;
        } else if (!strcmp(tok, "th_b") && !names) {
            I->bow.thermal.b_tau = v;
        } else if (!strcmp(tok, "th_s0") && !names) {
            I->bow.thermal.sigma0 = v;
        } else if (!strcmp(tok, "th_ns") && !names) {
            I->bow.thermal.nsub = (int)v;
        } else if (!strcmp(tok, "lead_mass") && !names) {
            for (int m = 0; m < I->body.nlead; m++) I->body.lead[m].m *= v;
        } else if (!strcmp(tok, "stat_mass") && !names) {
            I->body.stat_m500 *= v;
        } else if (names && (!strcmp(tok, "q2k") || !strcmp(tok, "zeta") || !strcmp(tok, "zt"))) {
            int hit = 0;
            for (int i = 0; i < I->nstr; i++) {
                if (!strchr(names, I->str[i].name[0])) continue;
                hit = 1;
                if (!strcmp(tok, "q2k")) set_q2k(&I->str[i], v);
                else if (!strcmp(tok, "zeta")) I->str[i].zeta_t = v;
                else I->str[i].zt_ratio = v;
            }
            if (!hit) { fprintf(stderr, "knob %s.%s: no such string\n", tok, names); rc = 1; break; }
        } else { fprintf(stderr, "unknown knob %s\n", tok); rc = 1; break; }
    }
    free(buf);
    return rc;
}

void knobs_install(void)
{
    const char *s = getenv("ALTCELLO_KNOBS");
    if (!s || !*s) return;
    static Instrument copy;
    copy = *instrument_cello();
    if (knobs_apply(&copy, s)) exit(2);
    instrument_cello_override(&copy);
    if (copy.bow.model == BOW_THERMAL) analysis_slip_thr = ANALYSIS_SLIP_THR_THERMAL;
}
