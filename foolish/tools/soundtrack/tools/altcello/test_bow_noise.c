/* T26 bow noise on the sliding friction coefficient (iteration 2) and the playability table.
 * a) the noise acts only while slipping: a sticking junction gives exactly the deterministic force and velocity whatever the
 *    noise, and a slipping junction gives phi_slip = (1 + n) phi at the solved velocity;
 * b) the draw schedule is in logical time: the same draws (values, logical times) at 48 and 96 kHz, and the noise value
 *    n(t) at a given logical time is the same function of t;
 * c) the playability table of the instrument: its centre force at beta 0.1, v_B 0.1 m/s gives Helmholtz motion in a 3 s run
 *    on every string (body on, all strings), and every table row is consistent (fmin < centre < fmax, centre = geometric). */
#include "analysis.h"
#include "sim.h"
#include "test_common.h"
#include <math.h>
#include <stdio.h>

static void a_sticking_only_deterministic(void)
{
    const Friction *fr = &instrument_cello()->bow.friction;
    double G = 2.0;
    double nz[] = {0.0, 0.3, -0.3, 0.9};
    /* inside the static cone, previous step sticking: v = 0, Phi = q / G for every noise */
    for (int i = 0; i < 4; i++) {
        double Phi, v;
        int st;
        v = bow_solve(fr, 0.7 * fr->mu_static * G, G, 0.0, 1, nz[i], &Phi, &st);
        CHECK(st == 1 && v == 0.0 && Phi == 0.7 * fr->mu_static, "sticking with noise %g: v %g Phi %.12g stuck %d", nz[i], v, Phi, st);
    }
    /* slipping (outside the cone): the force is (1 + n) phi(v) at the solved v, and v solves v = q - G (1 + n) phi(v) */
    for (int i = 0; i < 4; i++) {
        double Phi, q = 4.0; /* outside the static cone (mu_static G = 2.4) */
        int st;
        double v = bow_solve(fr, q, G, 0.0, 0, nz[i], &Phi, &st);
        CHECK(st == 0, "noise %g: slipping junction reported sticking", nz[i]);
        CHECK(fabs(Phi - (1.0 + nz[i]) * bow_phi(fr, v)) < 1e-12, "noise %g: Phi %.12g not (1+n) phi(v) = %.12g", nz[i], Phi,
              (1.0 + nz[i]) * bow_phi(fr, v));
        CHECK(fabs(v - (q - G * Phi)) < 1e-10, "noise %g: v %.12g vs q - G Phi %.12g", nz[i], v, q - G * Phi);
    }
    double P0, P1;
    int st;
    bow_solve(fr, 4.0, G, 0.0, 0, 0.0, &P0, &st);
    bow_solve(fr, 4.0, G, 0.0, 0, 0.2, &P1, &st);
    CHECK(fabs(P1 - P0) > 1e-3, "noise 0.2 does not change the slipping force (%.6f vs %.6f)", P1, P0);
    printf("T26a sticking exact for noise 0 / 0.3 / -0.3 / 0.9; slipping Phi = (1+n) phi(v): noise 0.2 moves Phi %.4f -> %.4f\n", P0, P1);
}

static void b_schedule_logical_time(void)
{
    BowNoise a, b;
    Rng ra, rb;
    rng_seed(&ra, ALTCELLO_SEED);
    rng_seed(&rb, ALTCELLO_SEED);
    bow_noise_init(&a, 0.02, 48000.0);
    bow_noise_init(&b, 0.02, 96000.0);
    double fa = 48000.0, fb = 96000.0;
    for (long n = 0; n < (long)(1.0 * fb); n++) {
        bow_noise_next(&b, &rb, n, fb);
        if (n % 2 == 0) { /* the same logical time n/fb = (n/2)/fa */
            bow_noise_next(&a, &ra, n / 2, fa);
        }
    }
    CHECK(a.idx_next == b.idx_next && a.idx_next == 4000, "draw index after 1 s: %ld (48k) %ld (96k), want 4000", a.idx_next, b.idx_next);
    int ident = 1;
    for (long i = 0; i < a.nlog && i < 1000; i++)
        if (a.log_x[i] != b.log_x[i] || a.log_t[i] != b.log_t[i]) { ident = 0; break; }
    CHECK(ident, "first 1000 draws differ between 48 and 96 kHz");
    printf("T26b %ld draws at 48 kHz and 96 kHz over 1 s, first 1000 identical (values and logical times): %s\n", a.idx_next, ident ? "yes" : "NO");
}

/* T26d the faster, higher corner noise of richness F1: the draw schedule is still in logical time at 64000 draws per second (identical draws at 48 and 96 kHz, 64000 per
 * second), and the level is a density: the draw at rate r is the draw at the reference rate times sqrt(r / 4000), so the noise density does not depend on the draw rate. */
static void d_fast_noise(void)
{
    BowNoise a, b, c;
    Rng ra, rb, rc;
    rng_seed(&ra, ALTCELLO_SEED);
    rng_seed(&rb, ALTCELLO_SEED);
    rng_seed(&rc, ALTCELLO_SEED);
    bow_noise_init_ex(&a, 0.02, 48000.0, 16000.0, 64000);
    bow_noise_init_ex(&b, 0.02, 96000.0, 16000.0, 64000);
    bow_noise_init(&c, 0.02, 96000.0);
    double fa = 48000.0, fb = 96000.0;
    for (long n = 0; n < (long)fb; n++) {
        bow_noise_next(&b, &rb, n, fb);
        bow_noise_next(&c, &rc, n, fb);
        if (n % 2 == 0) bow_noise_next(&a, &ra, n / 2, fa);
    }
    CHECK(a.idx_next >= 63999 && b.idx_next == 64000, "draws after 1 s: %ld (48k, whose last sample is n = 47999) %ld (96k), want 63999+ / 64000", a.idx_next, b.idx_next);
    int ident = 1;
    for (long i = 0; i < 1000; i++) if (a.log_x[i] != b.log_x[i] || a.log_t[i] != b.log_t[i]) ident = 0;
    CHECK(ident, "the first 1000 fast draws differ between 48 and 96 kHz");
    double ratio = b.log_x[5] / c.log_x[5];
    CHECK(fabs(ratio - 4.0) < 1e-12, "draw scale %g, want sqrt(64000 / 4000) = 4", ratio);
    printf("T26d 64000 draws per second at 48 and 96 kHz identical: %s; draw scale against the 4000 per second noise %.6f\n", ident ? "yes" : "NO", ratio);
}

static void c_playability_table(void)
{
    const Instrument *I = instrument_cello();
    int ib = -1, iv = -1;
    for (int i = 0; i < PLAY_NBETA; i++) if (fabs(I->play_beta[i] - 0.1) < 1e-9) ib = i;
    for (int i = 0; i < PLAY_NVB; i++) if (fabs(I->play_vb[i] - 0.1) < 1e-9) iv = i;
    CHECK(ib >= 0 && iv >= 0, "table has no beta 0.1 / v_B 0.1 entry");
    if (ib < 0 || iv < 0) return;
    for (int s = 0; s < I->nstr; s++) {
        for (int b = 0; b < PLAY_NBETA; b++)
            for (int v = 0; v < PLAY_NVB; v++) {
                const PlayBand *p = &I->playability[s][b][v];
                if (p->fmax <= 0.0) continue; /* no band found at this point */
                CHECK(p->fmin > 0.0 && p->fmin <= p->fmax, "%s row %d %d: fmin %g fmax %g", I->str[s].name, b, v, p->fmin, p->fmax);
                CHECK(fabs(p->fcentre - sqrt(p->fmin * p->fmax)) < 1e-4 * p->fcentre, "%s row %d %d: centre %g is not the geometric centre", I->str[s].name, b, v, p->fcentre);
            }
        double F = I->playability[s][ib][iv].fcentre;
        CHECK(F > 0.0, "%s has no band at beta 0.1, v_B 0.1", I->str[s].name);
        Sim S;
        SimOpts so = {1, 1, 0, NULL};
        sim_init_instrument(&S, I, 96000.0, &so, I->str[s].name);
        sim_set_bow(&S, 0.1, 0.1, F, 0.03);
        sim_start(&S);
        Trace *tr = sim_run(&S, 3.0, NULL, NULL);
        Helm h = analyse_helmholtz(tr->ch[0], tr->ch[3], tr->n, tr->fs, I->str[s].f0, 1.5);
        SimpleHelm sh = analyse_simple(tr->ch[3], tr->n, tr->fs, h.f_est, 1.5, 0.2);
        printf("T26c %s: centre force %.4f N (band %.4f - %.4f): one slip per period %d, simple %d, slip fraction %.3f\n", I->str[s].name, F,
               I->playability[s][ib][iv].fmin, I->playability[s][ib][iv].fmax, h.ok_one_per_period, sh.ok, h.slip_frac);
        CHECK(h.ok_one_per_period && sh.ok, "%s: no Helmholtz motion at the table centre %.4f N", I->str[s].name, F);
        trace_free(tr);
        sim_free(&S);
    }
}

int main(void)
{
    a_sticking_only_deterministic();
    b_schedule_logical_time();
    d_fast_noise();
    c_playability_table();
    TEST_DONE("test_bow_noise");
}
