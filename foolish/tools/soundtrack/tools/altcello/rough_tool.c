/* rough_tool: render one note list like `altcello render` (expert/novice player, all strings, body) and write the contact trace of the bowed string.
 *   rough_tool NOTES.txt OUTPREFIX [--seed N] [--vib on|off] [--vb MIN SPAN] [--frac MIN SPAN] [--beta0 B]
 * OUTPREFIX.f32: float32 interleaved at 96 kHz, columns vbow (string surface velocity at the bow, m/s), vrel (m/s), fric (N), tau (K above ambient),
 * zbristle (m), bridge force u (N); OUTPREFIX.txt: header line. Diagnosis tool of the roughness work (charts/rough.py); not part of the shipped path. */
#include "knobs.h"
#include "render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NC 6
typedef struct { float *buf; long n, cap; } Rec;

static void cb(const Sim *S, long i, void *user)
{
    Rec *r = user;
    (void)i;
    if (r->n >= r->cap) { r->cap = r->cap ? 2 * r->cap : 1 << 20; r->buf = realloc(r->buf, sizeof(float) * NC * (size_t)r->cap); }
    float *p = r->buf + NC * r->n++;
    p[0] = (float)S->out[2]; p[1] = (float)S->out[3]; p[2] = (float)S->out[4];
    p[3] = (float)S->th.tau; p[4] = (float)S->th.z; p[5] = (float)S->out[0];
}

int main(int argc, char **argv)
{
    knobs_install();
    if (argc < 3) { fprintf(stderr, "usage: rough_tool NOTES.txt OUTPREFIX [--seed N] [--vib on|off] [--vb MIN SPAN] [--frac MIN SPAN] [--beta0 B]\n"); return 2; }
    RenderOpts o;
    render_opts_default(&o);
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc) o.seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--vib") && i + 1 < argc) o.vib_off = !strcmp(argv[++i], "off");
        else if (!strcmp(argv[i], "--vb") && i + 2 < argc) { o.vb[0] = atof(argv[i + 1]); o.vb[1] = atof(argv[i + 2]); i += 2; }
        else if (!strcmp(argv[i], "--frac") && i + 2 < argc) { o.frac[0] = atof(argv[i + 1]); o.frac[1] = atof(argv[i + 2]); i += 2; }
        else if (!strcmp(argv[i], "--solo")) o.solo = 1;
        else if (!strcmp(argv[i], "--beta0") && i + 1 < argc) o.beta0 = atof(argv[++i]);
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    Note *nt;
    int n;
    if (notes_read(argv[1], &nt, &n) || n < 1) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    Rec rec = {0};
    o.step_cb = cb;
    o.step_user = &rec;
    Played pl;
    float *y;
    long m;
    if (render_notes(nt, n, &o, &y, &m, &pl, NULL)) return 1;
    char path[1024];
    snprintf(path, sizeof path, "%s.f32", argv[2]);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    fwrite(rec.buf, sizeof(float) * NC, (size_t)rec.n, f);
    fclose(f);
    snprintf(path, sizeof path, "%s.txt", argv[2]);
    f = fopen(path, "w");
    fprintf(f, "fs=96000 n=%ld channels=vbow,vrel,fric,tau,z,bridgeu\n", rec.n);
    fclose(f);
    return 0;
}
