/* T32 the solved finite element body option (fe_body.h):
 *  a) loader round trip: a written file comes back value for value; the real file (data/fin5_cpl.bodymodes.txt) has 380 modes with the known first and last
 *     rows; malformed files (count mismatch, no end line, a bad number, a missing column) are refused;
 *  b) the BodyModes built from it: u flipped to our geometry (cw, f, Q, m, r_net not), per-string participations kept, no table modes, the statistical modes above
 *     the crossover identical to the shipped ones, nothing of the statistical set below it;
 *  c) per-string coupling: a unit force on one string moves every notch by its own participation (one-mode file: z_s = k^2 cu_s cu_F / m exactly; two-mode file:
 *     reciprocity z_C(F_A) = z_A(F_C) and z_C(F_C) / z_A(F_C) = the modal sum, which is not the ratio of one mode) and the real file's C and A notches differ;
 *  d) the crossover match: the radiated pressure of FE-A and of FE-B equals the shipped body's in 2.0-2.5 kHz (white force on every string) within 0.5 dB;
 *  e) the radiation weights: FE-A p_fe is the second difference of V = sum r_net q (a free lossless mode: p_fe = -gain rho / (4 pi) r_net w^2 q); FE-B weights are
 *     r_net / |r_net(A0)| for the compact modes (air_share > 0.5 or f < 250 Hz) and gain_b sign(r_net) for the others;
 *  f) T8 energy with the FE body (all four strings, FE-A and FE-B): lossless 1e-9, lossy never increases. */
#include "fe_body.h"
#include "sim.h"
#include "test_common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REAL "data/fin5_cpl.bodymodes.txt"
#define TMP "out/test_fe_body_tmp.txt"

static void write_file(const char *path, int n, const double (*row)[23], int count, int with_end, const char *badtok)
{
    FILE *fp = fopen(path, "w");
    fprintf(fp, "BODYMODES 1\n# test\nair c 343.215 rho 1.2041\nstrings C2 G2 D3 A3\ncount %d\n", count);
    fprintf(fp, "columns idx f_Hz eta Q m_kg cu_C cw_C cu_G cw_G cu_D cw_D cu_A cw_A foot_bass_w foot_treble_w top_centre_n back_centre_n r_plates r_fholes r_net air_share eta_struct eta_rad\n");
    for (int i = 0; i < n; i++) {
        fprintf(fp, "%d", i + 1);
        for (int c = 0; c < 22; c++) {
            if (badtok && i == 0 && c == 3) fprintf(fp, " %s", badtok);
            else fprintf(fp, " %.17g", row[i][c]);
        }
        fprintf(fp, "\n");
    }
    if (with_end) fprintf(fp, "end\n");
    fclose(fp);
}

/* row columns after idx: f eta Q m cuC cwC cuG cwG cuD cwD cuA cwA fb ft tc bc rp rf rnet air es er */
static void mkrow(double *r, double f, double Q, double m, double cuC, double cuA, double rnet, double air)
{
    memset(r, 0, 22 * sizeof(double));
    r[0] = f; r[1] = 1.0 / Q; r[2] = Q; r[3] = m;
    r[4] = cuC; r[5] = 0.3; r[6] = 0.5 * (cuC + cuA); r[7] = 0.2; r[8] = 0.5 * (cuC + cuA); r[9] = 0.1; r[10] = cuA; r[11] = -0.2;
    r[18] = rnet; r[19] = air;
}

static void check_loader(void)
{
    double rows[3][23];
    mkrow(rows[0], 100.25, 80.5, 3.5, 0.9, 0.4, -0.01, 0.9);
    mkrow(rows[1], 210.125, 60.0, 0.75, -0.3, 0.8, 0.02, 0.1);
    mkrow(rows[2], 440.5, 45.5, 0.0625, 1.0, -1.0, 0.0, 0.0);
    char err[200];
    write_file(TMP, 3, (const double (*)[23])rows, 3, 1, NULL);
    FeFile *F = fe_file_load(TMP, err, sizeof err);
    CHECK(F != NULL, "round trip file refused: %s", F ? "" : err);
    if (F) {
        CHECK(F->n == 3, "count %d", F->n);
        CHECK(!strcmp(F->names[0], "C2") && !strcmp(F->names[3], "A3"), "names %s %s", F->names[0], F->names[3]);
        for (int i = 0; i < 3; i++) {
            CHECK(F->f[i] == rows[i][0] && F->Q[i] == rows[i][2] && F->m[i] == rows[i][3], "mode %d f %g Q %g m %g", i, F->f[i], F->Q[i], F->m[i]);
            CHECK(F->cu[0][i] == rows[i][4] && F->cu[3][i] == rows[i][10] && F->cw[3][i] == rows[i][11], "mode %d cu %g %g cw %g", i, F->cu[0][i], F->cu[3][i], F->cw[3][i]);
            CHECK(F->r_net[i] == rows[i][18] && F->air[i] == rows[i][19], "mode %d r_net %g air %g", i, F->r_net[i], F->air[i]);
        }
        fe_file_free(F);
    }
    write_file(TMP, 3, (const double (*)[23])rows, 4, 1, NULL);
    CHECK(fe_file_load(TMP, err, sizeof err) == NULL, "a count of 4 with 3 rows was accepted");
    write_file(TMP, 3, (const double (*)[23])rows, 3, 0, NULL);
    CHECK(fe_file_load(TMP, err, sizeof err) == NULL, "a file without the end line was accepted");
    write_file(TMP, 3, (const double (*)[23])rows, 3, 1, "abc");
    CHECK(fe_file_load(TMP, err, sizeof err) == NULL, "a bad number was accepted");
    CHECK(fe_file_load("out/does_not_exist.txt", err, sizeof err) == NULL, "a missing file was accepted");
    remove(TMP);
    /* the real file */
    F = fe_file_load(REAL, err, sizeof err);
    CHECK(F != NULL, "real file refused: %s", F ? "" : err);
    if (!F) return;
    printf("T32a real file: %d modes, first f %.3f Q %.2f m %.5e cu_C %+.5f r_net %+.4e, last f %.3f Q %.2f air %.4f\n", F->n, F->f[0], F->Q[0], F->m[0], F->cu[0][0], F->r_net[0], F->f[F->n - 1], F->Q[F->n - 1], F->air[F->n - 1]);
    CHECK(F->n == 380, "380 modes expected, %d", F->n);
    CHECK(fabs(F->f[0] - 52.140) < 1e-9 && fabs(F->Q[0] - 82.78) < 1e-9 && fabs(F->m[0] - 3.52615e+02) < 1e-9, "first row f %g Q %g m %g", F->f[0], F->Q[0], F->m[0]);
    CHECK(F->cu[0][0] == 0.11080 && F->cw[0][0] == -1.0 && F->cu[3][0] == 0.10866 && F->r_net[0] == -9.8862e-04, "first row c: %g %g %g %g", F->cu[0][0], F->cw[0][0], F->cu[3][0], F->r_net[0]);
    CHECK(fabs(F->f[379] - 2478.667) < 1e-9 && fabs(F->Q[379] - 1418.58) < 1e-9 && F->air[379] == 0.9421 && F->cu[0][379] == 0.37668, "last row f %g Q %g air %g cu_C %g", F->f[379], F->Q[379], F->air[379], F->cu[0][379]);
    for (int i = 1; i < F->n; i++) CHECK(F->f[i] > F->f[i - 1], "frequencies not ascending at %d", i);
    fe_file_free(F);
}

static Instrument fe_inst(int variant)
{
    Instrument c = *instrument_cello();
    c.body.fe_variant = variant;
    c.body.fe_path = REAL;
    return c;
}

static void check_modes(void)
{
    Instrument I = fe_inst(1);
    FeFile *F = fe_file_load(REAL, NULL, 0);
    BodyOpts o = body_opts_default(&I);
    BodyModes *fe = body_modes_build_opts(&I, &o), *st = body_modes_build_stat(&I, &o);
    int nfe = fe->nfe;
    CHECK(nfe == 380 && fe->nlead == 0 && fe->ncompact == 0, "nfe %d nlead %d", nfe, fe->nlead);
    /* u flips, w does not, per string; f Q m as in the file */
    CHECK(fe->cus[0 * nfe + 0] == -0.11080 && fe->cws[0 * nfe + 0] == -1.0 && fe->cus[3 * nfe + 0] == -0.10866 && fe->cws[3 * nfe + 0] == -0.93922, "mode 0 notch participations: C %g %g, A %g %g", fe->cus[0], fe->cws[0], fe->cus[3 * nfe], fe->cws[3 * nfe]);
    CHECK(fe->f[0] == F->f[0] && fe->Q[0] == F->Q[0] && fe->m[0] == F->m[0] && fe->rnet[0] == F->r_net[0], "f Q m r_net not the file's");
    /* the statistical modes: none below the crossover; above it the shipped ones, value for value and in order */
    int j = nfe, nstat_above = 0;
    for (int i = st->nlead; i < st->n; i++)
        if (st->f[i] >= I.body.fe_fx) {
            nstat_above++;
            CHECK(j < fe->n && fe->f[j] == st->f[i] && fe->m[j] == st->m[i] && fe->Q[j] == st->Q[i] && fe->cu[j] == st->cu[i] && fe->r[j] == st->r[i], "statistical mode %d differs from the shipped one", i);
            j++;
        }
    CHECK(j == fe->n, "%d modes built, %d expected", fe->n, j);
    for (int i = nfe; i < fe->n; i++) CHECK(fe->f[i] >= I.body.fe_fx, "a statistical mode at %g Hz below the crossover", fe->f[i]);
    printf("T32b %d FE modes + %d statistical modes above %.0f Hz (the shipped body: %d modes, %d table + %d statistical)\n", nfe, nstat_above, I.body.fe_fx, st->n, st->nlead, st->n - st->nlead);
    body_modes_free(fe);
    body_modes_free(st);
    fe_file_free(F);
}

static double z_after_one_step(const BodyModes *bm, const Instrument *I, int force_string, double *zout)
{
    Body *b = body_new(I, bm, 96000.0, 1, NULL);
    double Fu[BODY_NS] = {0}, Fw[BODY_NS] = {0};
    Fu[force_string] = 1.0;
    body_step_str(b, Fu, Fw);
    for (int s = 0; s < BODY_NS; s++) zout[s] = b->zus[s];
    body_free(b);
    return 0.0;
}

static BodyModes *from_rows(const double (*rows)[23], int n, Instrument *I)
{
    write_file(TMP, n, rows, n, 1, NULL);
    I->body.fe_variant = 1;
    I->body.fe_path = TMP;
    I->body.fe_fx = 1e9; /* no statistical modes: only the file's, so the sums below are exact */
    BodyOpts o = body_opts_default(I);
    BodyModes *bm = body_modes_build_opts(I, &o);
    return bm;
}

static void check_coupling(void)
{
    double rows[2][23];
    Instrument I = fe_inst(1);
    /* one mode: z_s = k^2 cu_s cu_F F / m exactly (lossless, one step from rest); the u participations are flipped by the loader */
    mkrow(rows[0], 150.0, 50.0, 2.0, 0.8, 0.2, 0.0, 0.0);
    BodyModes *bm = from_rows((const double (*)[23])rows, 1, &I);
    double k = 1.0 / 96000.0, z[BODY_NS];
    double cu[BODY_NS] = {-0.8, -0.5, -0.5, -0.2}; /* C G D A, flipped */
    for (int fs_ = 0; fs_ < BODY_NS; fs_++) {
        z_after_one_step(bm, &I, fs_, z);
        for (int s = 0; s < BODY_NS; s++) {
            double want = k * k * cu[s] * cu[fs_] / 2.0;
            CHECK(fabs(z[s] - want) < 1e-12 * fabs(want) + 1e-30, "force on string %d: notch %d moved %.6e, want k^2 cu_s cu_F / m = %.6e", fs_, s, z[s], want);
        }
    }
    printf("T32c one mode, force on C: notch displacements C %.4e G %.4e D %.4e A %.4e (ratio A/C %.3f = cu_A / cu_C = 0.250)\n", z[0], z[1], z[2], z[3], z[3] / z[0]);
    body_modes_free(bm);
    /* two modes with opposite C/A balance: reciprocity, and the ratio is the modal sum (not one mode's cu_A / cu_C) */
    mkrow(rows[0], 150.0, 50.0, 2.0, 0.8, 0.2, 0.0, 0.0);
    mkrow(rows[1], 210.0, 50.0, 1.0, 0.1, 0.9, 0.0, 0.0);
    bm = from_rows((const double (*)[23])rows, 2, &I);
    double zc[BODY_NS], za[BODY_NS];
    z_after_one_step(bm, &I, 0, zc);
    z_after_one_step(bm, &I, 3, za);
    double c0[2] = {-0.8, -0.1}, a0[2] = {-0.2, -0.9}, m[2] = {2.0, 1.0}, wantcc = 0, wantac = 0, wantaa = 0;
    for (int i = 0; i < 2; i++) { wantcc += c0[i] * c0[i] / m[i]; wantac += a0[i] * c0[i] / m[i]; wantaa += a0[i] * a0[i] / m[i]; }
    wantcc *= k * k; wantac *= k * k; wantaa *= k * k;
    CHECK(fabs(zc[0] - wantcc) < 1e-12 * wantcc && fabs(zc[3] - wantac) < 1e-12 * fabs(wantac) && fabs(za[3] - wantaa) < 1e-12 * wantaa, "two-mode sums: %g %g %g want %g %g %g", zc[0], zc[3], za[3], wantcc, wantac, wantaa);
    CHECK(fabs(zc[3] - za[0]) < 1e-15 * fabs(zc[3]) + 1e-30, "reciprocity: z_A(F_C) %g, z_C(F_A) %g", zc[3], za[0]);
    body_modes_free(bm);
    remove(TMP);
    /* the real file: the C and A notches differ in u on the rocking modes (INTERFACE.md: up to about a factor of 2) */
    FeFile *F = fe_file_load(REAL, NULL, 0);
    double worst = 1.0;
    for (int i = 0; i < F->n; i++) {
        double a = fabs(F->cu[0][i]), b = fabs(F->cu[3][i]);
        if (a > 0.05 && b > 0.05 && a / b > worst) worst = a / b;
        if (a > 0.05 && b > 0.05 && b / a > worst) worst = b / a;
    }
    printf("T32c real file: largest |cu_C| / |cu_A| (or inverse) over the modes with both above 0.05: %.2f\n", worst);
    CHECK(worst > 1.3, "the C and A participations of the real file do not differ (%.2f)", worst);
    fe_file_free(F);
}

static void check_crossover(void)
{
    const long n = 1L << 19;
    Instrument I0 = fe_inst(0), I1 = fe_inst(1), I2 = fe_inst(2);
    double p0 = fe_white_psd(&I0, 2000.0, 2500.0, n), p1 = fe_white_psd(&I1, 2000.0, 2500.0, n), p2 = fe_white_psd(&I2, 2000.0, 2500.0, n);
    double d1 = 10.0 * log10(p1 / p0), d2 = 10.0 * log10(p2 / p0);
    printf("T32d 2.0-2.5 kHz radiated PSD re the shipped body: FE-A %+.2f dB (gain_a %.6f), FE-B %+.2f dB (gain_b %.6f)\n", d1, I1.body.fe_gain_a, d2, I2.body.fe_gain_b);
    CHECK(fabs(d1) < 0.5, "FE-A is %+.2f dB re the shipped body at the crossover", d1);
    CHECK(fabs(d2) < 0.5, "FE-B is %+.2f dB re the shipped body at the crossover", d2);
}

static void check_radiation(void)
{
    double rows[1][23];
    Instrument I = fe_inst(1);
    mkrow(rows[0], 300.0, 1e9, 1.0, 0.5, 0.5, 0.01, 0.0);
    BodyModes *bm = from_rows((const double (*)[23])rows, 1, &I);
    Body *b = body_new(&I, bm, 96000.0, 1, NULL);
    double Fu[BODY_NS] = {0}, Fw[BODY_NS] = {0};
    Fu[0] = 1e6;
    double worst = 0.0;
    for (int i = 0; i < 400; i++) {
        body_step_str(b, Fu, Fw);
        Fu[0] = 0.0;
        if (i < 5) continue;
        double q = b->q0[0]; /* state at n, after the rotation */
        double want = -b->gain_a * 0.01 * b->w2[0] * q;
        double err = fabs(b->p_fe - want) / (fabs(want) + 1e-300);
        if (fabs(q) > 1e-9 && err > worst) worst = err;
    }
    printf("T32e FE-A free mode: p_fe = -gain rho/(4 pi) r_net w^2 q to %.2e relative\n", worst);
    CHECK(worst < 1e-9, "p_fe is not the second difference of r_net q (worst relative error %g)", worst);
    body_free(b);
    body_modes_free(bm);
    remove(TMP);
    /* FE-B weights on the real file */
    Instrument J = fe_inst(2);
    BodyOpts o = body_opts_default(&J);
    BodyModes *fe = body_modes_build_opts(&J, &o);
    Body *bb = body_new(&J, fe, 96000.0, 0, NULL);
    int a0 = -1, ncomp = 0, nplate = 0;
    for (int i = 0; i < fe->nfe; i++) if (fe->air[i] > 0.5) { a0 = i; break; }
    double rA0 = fabs(fe->rnet[a0]);
    for (int i = 0; i < fe->nfe; i++) {
        int compact = fe->air[i] > 0.5 || fe->f[i] < 250.0;
        if (compact) {
            ncomp++;
            CHECK(bb->wp[i] == 0.0 && fabs(bb->wc[i] - fe->rnet[i] / rA0) < 1e-12, "mode %d (%.1f Hz): compact weight %g want %g", i, fe->f[i], bb->wc[i], fe->rnet[i] / rA0);
        } else {
            nplate++;
            double sg = fe->rnet[i] > 0.0 ? 1.0 : -1.0;
            CHECK(bb->wc[i] == 0.0 && bb->wp[i] == sg * J.body.fe_gain_b, "mode %d (%.1f Hz): plate weight %g want %g (sign of r_net %+g)", i, fe->f[i], bb->wp[i], sg * J.body.fe_gain_b, fe->rnet[i]);
        }
    }
    CHECK(fabs(fabs(bb->wc[a0]) - 1.0) < 1e-12, "A0 weight %g, want magnitude 1", bb->wc[a0]);
    printf("T32e FE-B: A0 at %.2f Hz (mode %d), %d compact FE modes, %d plate FE modes\n", fe->f[a0], a0, ncomp, nplate);
    body_free(bb);
    body_modes_free(fe);
}

typedef struct { double *E; } Log;
static void cb(const Sim *S, long i, void *u) { ((Log *)u)->E[i] = sim_energy(S); }

static void run_energy(const Instrument *I, int lossless, int variant)
{
    SimOpts o = {1, 1, lossless, 0};
    Sim S;
    sim_init_instrument(&S, I, 96000.0, &o, "D3");
    S.bow_on = 0;
    sim_start(&S);
    for (int i = 0; i < S.nstr; i++) {
        sim_pluck_string(&S, i, 0, (0.13 + 0.07 * i) * 0.69, 0.02 * 0.69, 1e-3);
        sim_pluck_string(&S, i, 1, (0.21 + 0.05 * i) * 0.69, 0.02 * 0.69, 5e-4);
        sim_pluck_torsion(&S, i, (0.31 + 0.06 * i) * 0.69, 0.04 * 0.69, 0.5);
    }
    long n = 96000;
    Log lg = {calloc((size_t)n, sizeof(double))};
    double E0 = sim_energy(&S);
    Trace *tr = sim_run(&S, 1.0, cb, &lg);
    double mx = 0, mn = 1e300, worst_up = 0;
    for (long i = 0; i < n; i++) {
        if (lg.E[i] > mx) mx = lg.E[i];
        if (lg.E[i] < mn) mn = lg.E[i];
        if (i > 0) { double up = (lg.E[i] - lg.E[i - 1]) / E0; if (up > worst_up) worst_up = up; }
    }
    double Ebody = body_energy(S.body);
    if (lossless) {
        double rel = (mx - mn) / E0;
        printf("T32f FE-%c lossless: E0 %.6e J, relative spread %.3e (limit 1e-9), body energy at 1 s %.2f%% of total\n", 'A' + variant - 1, E0, rel, 100.0 * Ebody / lg.E[n - 1]);
        CHECK(rel < 1e-9, "FE-%c coupled energy not conserved: relative spread %g", 'A' + variant - 1, rel);
        CHECK(Ebody > 1e-3 * lg.E[n - 1], "FE-%c: the body holds %g of the energy: coupling is vacuous", 'A' + variant - 1, Ebody / lg.E[n - 1]);
    } else {
        printf("T32f FE-%c lossy: E0 %.6e -> %.6e J, largest relative step increase %.3e\n", 'A' + variant - 1, E0, lg.E[n - 1], worst_up);
        CHECK(worst_up < 1e-12, "FE-%c energy increased by relative %g in one step", 'A' + variant - 1, worst_up);
        CHECK(lg.E[n - 1] < 0.9 * E0, "FE-%c energy did not decay", 'A' + variant - 1);
    }
    free(lg.E);
    trace_free(tr);
    sim_free(&S);
}

int main(void)
{
    check_loader();
    check_modes();
    check_coupling();
    check_radiation();
    check_crossover();
    for (int v = 1; v <= 2; v++) {
        Instrument I = fe_inst(v);
        run_energy(&I, 1, v);
        run_energy(&I, 0, v);
    }
    TEST_DONE("test_fe_body");
}
