// Moscow choir test (v5): the chant of "Chant: Soldiers" sung by the VocalSet men, first alone, then over timpani, bass drum
// and field drum, for Alex to judge the choir by ear before the Moscow song depends on it. Render from this folder:
//   clang -O2 -w -o score score.c && SAMPLES=<samples dir> ENGINE=../../engine ./score full out
#define CHOIR_OM "../../samples/manifests/"
#include "../../engine/choir.h"
static int CH, TI, BD, FD;
static int t_map(const char *p, double *tp, double *vs, double *dp) {
    *tp = 0; *vs = 1;
    if (!strcmp(p, "CHOIR")) return CH;
    if (!strcmp(p, "TIMPANI")) return TI;
    if (!strcmp(p, "BASSDRUM")) { *dp = 60; return BD; }
    if (!strcmp(p, "FIELDDRUM")) { *dp = 60; return FD; }
    return -1;
}
void city_setup(void) {
    g_on_done = sections_out;
    static const char *men[] = {"male1", "male2", "male3", "male4", "male5", "male6", "male7", "male8", "male10", "male11"};   // male9 sings an octave above the rest
    CH = choir_setup(men, 10, "ah", 0, 0.5);
    TI = o_smp("timpani", "../../samples/manifests/timpani.txt", 1.2, 0, 0.0, 0, 0.22, 4);
    BD = o_smp("bassdrum", "../../samples/manifests/bassdrum.txt", 1.0, 0, 0.05, 0, 0.18, 3);
    FD = o_smp("fielddrum", "../../samples/manifests/fielddrum.txt", 0.4, 0, 0.15, 0, 0.2, 7);
    o_hall(2.8, 24);
    g_dyn_depth = 0.8;
}
double city_loop(void) { int n0 = g_nnote; g_nsec = 0; score_txt("score.txt", t_map, 0); choir_post(n0); return score_total_bars() * 4.0; }
void city_full(void) { city_loop(); }
