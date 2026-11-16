#include "fe_body.h"
#include "analysis.h"
#include "body.h"
#include "rng.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXCOL 40
#define FAIL(...) do { if (err) snprintf(err, (size_t)errn, __VA_ARGS__); fe_file_free(F); fclose(fp); return NULL; } while (0)

void fe_file_free(FeFile *F)
{
    if (!F) return;
    free(F->f); free(F->eta); free(F->Q); free(F->m); free(F->r_net); free(F->air);
    for (int s = 0; s < BODY_NS; s++) { free(F->cu[s]); free(F->cw[s]); }
    free(F);
}

static int col_of(char names[][24], int nc, const char *want)
{
    for (int i = 0; i < nc; i++) if (!strcmp(names[i], want)) return i;
    return -1;
}

FeFile *fe_file_load(const char *path, char *err, int errn)
{
    FILE *fp = fopen(path, "r");
    if (!fp) { if (err) snprintf(err, (size_t)errn, "cannot open %s", path); return NULL; }
    FeFile *F = calloc(1, sizeof *F);
    char line[1024];
    int have_head = 0, count = -1, nc = 0, got = 0, ended = 0;
    char cn[MAXCOL][24];
    int cf, ce, cq, cm, cr, ca, ccu[BODY_NS], ccw[BODY_NS];
    cf = ce = cq = cm = cr = ca = -1;
    while (fgets(line, sizeof line, fp)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        if (!have_head) {
            if (strncmp(p, "BODYMODES 1", 11)) FAIL("not a BODYMODES 1 file");
            have_head = 1;
            continue;
        }
        if (!strncmp(p, "air ", 4)) {
            char a[8], b[8];
            if (sscanf(p, "air %7s %lf %7s %lf", a, &F->c, b, &F->rho) != 4) FAIL("bad air line");
        } else if (!strncmp(p, "strings ", 8)) {
            if (sscanf(p, "strings %7s %7s %7s %7s", F->names[0], F->names[1], F->names[2], F->names[3]) != 4) FAIL("bad strings line (need %d names)", BODY_NS);
        } else if (!strncmp(p, "count ", 6)) {
            count = atoi(p + 6);
            if (count <= 0 || count > 100000) FAIL("bad count %d", count);
        } else if (!strncmp(p, "columns", 7)) {
            char *q = p + 7, *tok;
            char *sv = NULL;
            nc = 0;
            for (tok = strtok_r(q, " \t\r\n", &sv); tok; tok = strtok_r(NULL, " \t\r\n", &sv)) {
                if (nc >= MAXCOL) FAIL("too many columns");
                snprintf(cn[nc++], 24, "%s", tok);
            }
            cf = col_of(cn, nc, "f_Hz"); ce = col_of(cn, nc, "eta"); cq = col_of(cn, nc, "Q"); cm = col_of(cn, nc, "m_kg");
            cr = col_of(cn, nc, "r_net"); ca = col_of(cn, nc, "air_share");
            for (int s = 0; s < BODY_NS; s++) {
                char a[24], b[24];
                snprintf(a, sizeof a, "cu_%c", F->names[s][0]);
                snprintf(b, sizeof b, "cw_%c", F->names[s][0]);
                ccu[s] = col_of(cn, nc, a);
                ccw[s] = col_of(cn, nc, b);
                if (ccu[s] < 0 || ccw[s] < 0) FAIL("missing column %s / %s", a, b);
            }
            if (cf < 0 || ce < 0 || cq < 0 || cm < 0 || cr < 0 || ca < 0) FAIL("missing a required column");
            if (count <= 0) FAIL("columns before count");
            F->f = calloc((size_t)count, sizeof(double)); F->eta = calloc((size_t)count, sizeof(double));
            F->Q = calloc((size_t)count, sizeof(double)); F->m = calloc((size_t)count, sizeof(double));
            F->r_net = calloc((size_t)count, sizeof(double)); F->air = calloc((size_t)count, sizeof(double));
            for (int s = 0; s < BODY_NS; s++) { F->cu[s] = calloc((size_t)count, sizeof(double)); F->cw[s] = calloc((size_t)count, sizeof(double)); }
        } else if (!strncmp(p, "end", 3)) {
            ended = 1;
            break;
        } else {
            if (!F->f) FAIL("a data line before the columns line");
            if (got >= count) FAIL("more data lines than count %d", count);
            double v[MAXCOL];
            char *e = p;
            for (int i = 0; i < nc; i++) {
                char *nx;
                v[i] = strtod(e, &nx);
                if (nx == e || !isfinite(v[i])) FAIL("data line %d: bad number in column %d", got + 1, i + 1);
                e = nx;
            }
            while (isspace((unsigned char)*e)) e++;
            if (*e) FAIL("data line %d: trailing text", got + 1);
            F->f[got] = v[cf]; F->eta[got] = v[ce]; F->Q[got] = v[cq]; F->m[got] = v[cm]; F->r_net[got] = v[cr]; F->air[got] = v[ca];
            for (int s = 0; s < BODY_NS; s++) { F->cu[s][got] = v[ccu[s]]; F->cw[s][got] = v[ccw[s]]; }
            if (F->f[got] <= 0.0 || F->Q[got] <= 0.0 || F->m[got] <= 0.0) FAIL("data line %d: f, Q, m must be positive", got + 1);
            got++;
        }
    }
    if (!have_head) FAIL("empty file");
    if (!ended) FAIL("no end line");
    if (got != count) FAIL("count %d but %d data lines", count, got);
    fclose(fp);
    F->n = count;
    return F;
}

#ifndef FE_DEFAULT_PATH
#define FE_DEFAULT_PATH "data/fin5_cpl.bodymodes.txt"
#endif

BodyModes *body_modes_build_fe(const Instrument *I, const BodyOpts *o)
{
    const char *path = I->body.fe_path ? I->body.fe_path : FE_DEFAULT_PATH;
    char err[200];
    FeFile *F = fe_file_load(path, err, sizeof err);
    if (!F) { fprintf(stderr, "FE body %s: %s\n", path, err); exit(2); }
    int map[BODY_NS]; /* instrument string index -> file column */
    for (int i = 0; i < BODY_NS; i++) {
        map[i] = -1;
        if (i < I->nstr)
            for (int s = 0; s < BODY_NS; s++) if (!strcmp(F->names[s], I->str[i].name)) map[i] = s;
        if (i < I->nstr && map[i] < 0) { fprintf(stderr, "FE body %s: no string %s\n", path, I->str[i].name); exit(2); }
    }
    BodyModes *d = body_modes_build_stat(I, o); /* the shipped modes: the statistical ones above fe_fx are kept as they are */
    double fx = I->body.fe_fx;
    int nfe = 0, nst = 0;
    for (int i = 0; i < F->n; i++) if (F->f[i] < fx) nfe++;
    for (int i = d->nlead; i < d->n; i++) if (d->f[i] >= fx) nst++;
    int n = nfe + nst;
    BodyModes *b = calloc(1, sizeof *b);
#define AL(x, c) b->x = calloc((size_t)(c), sizeof(double))
    AL(f, n); AL(Q, n); AL(m, n); AL(cu, n); AL(cw, n); AL(r, n);
    AL(cus, BODY_NS * nfe); AL(cws, BODY_NS * nfe); AL(rnet, nfe); AL(air, nfe);
    b->n = n;
    b->nfe = nfe;
    b->nlead = 0;
    b->ncompact = 0;
    int j = 0;
    for (int i = 0; i < F->n && F->f[i] < fx; i++, j++) {
        b->f[j] = F->f[i]; b->Q[j] = F->Q[i]; b->m[j] = F->m[i];
        double su = 0.0, sw = 0.0;
        int ns = 0;
        for (int s = 0; s < I->nstr && s < BODY_NS; s++) {
            double cu = -F->cu[map[s]][i]; /* the mesh has the bass side at +x, geom.c at -x: u flips */
            double cw = F->cw[map[s]][i];
            b->cus[s * nfe + j] = cu; b->cws[s * nfe + j] = cw;
            su += cu; sw += cw; ns++;
        }
        b->cu[j] = su / ns; b->cw[j] = sw / ns;
        b->rnet[j] = F->r_net[i]; b->air[j] = F->air[i];
    }
    for (int i = d->nlead; i < d->n; i++)
        if (d->f[i] >= fx) {
            b->f[j] = d->f[i]; b->Q[j] = d->Q[i]; b->m[j] = d->m[i]; b->cu[j] = d->cu[i]; b->cw[j] = d->cw[i]; b->r[j] = d->r[i];
            j++;
        }
    body_modes_free(d);
    fe_file_free(F);
    return b;
}

double fe_white_psd(const Instrument *I, double flo, double fhi, long n)
{
    const double fs = 96000.0;
    BodyModes *bm = body_modes_build(I);
    Body *b = body_new(I, bm, fs, 0, NULL);
    double *p = malloc(sizeof(double) * (size_t)n);
    Rng r;
    rng_seed(&r, ALTCELLO_SEED);
    for (long i = 0; i < n; i++) {
        double Fu[BODY_NS], Fw[BODY_NS], su = 0.0, sw = 0.0;
        for (int s = 0; s < BODY_NS; s++) { Fu[s] = rng_gauss(&r); Fw[s] = rng_gauss(&r); su += Fu[s]; sw += Fw[s]; }
        if (b->nfe > 0) body_step_str(b, Fu, Fw);
        else body_step(b, su, sw);
        p[i] = b->p;
    }
    Spectrum *sp = spec_make(p, n, fs, 1);
    double pw = 0.0;
    int m = 0;
    for (int i = 0; i < sp->n / 2; i++) {
        double f = i * sp->df;
        if (f >= flo && f < fhi) { pw += sp->mag[i] * sp->mag[i]; m++; }
    }
    spec_free(sp);
    free(p);
    body_free(b);
    body_modes_free(bm);
    return pw / m;
}
