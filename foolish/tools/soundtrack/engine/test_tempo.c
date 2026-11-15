// test_tempo: a bare TEMPO line is a step at its section boundary, and "TEMPO x RAMP n" changes over n beats.
// Build and run from engine/: clang -O2 -w -o <tmp>/tt test_tempo.c && <tmp>/tt <tmp>/score.txt Exit 0 = pass.
// Mutation checked: with the step's hold point removed (the old behaviour) the first assertion fails (4 bars took 10.0 s, not 8.0).
#define main engine_main   // render.h carries the renderer's main; the test has its own
#include "render.h"
#undef main
#include "scoretxt.h"
void city_setup(void) {}
double city_loop(void) { return 0; }
void city_full(void) {}
static int none(const char *p, double *tp, double *vs, double *dp) { (void)p; (void)tp; (void)vs; (void)dp; return -1; }
static int near(double a, double b) { return fabs(a - b) < 0.01; }
int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "test_tempo_score.txt";
    FILE *f = fopen(path, "w"); if (!f) { perror(path); return 2; }
    fputs("TEMPO 120\nSECTION A BARS 4 KEY C\nX: r:4 | r:4 | r:4 | r:4\nTEMPO 60\nSECTION B BARS 2 KEY C\nX: r:4 | r:4\nSECTION C BARS 2 KEY C\nTEMPO 120 RAMP 8\nX: r:4 | r:4\n", f); fclose(f);
    score_txt(path, none, 0); qsort(g_tempo, g_ntempo, sizeof(TempoPt), tcmp); remove(path);
    double a = b2t_raw(16), b = b2t_raw(24), c = b2t_raw(32); int ok = 1;
    if (!near(a, 8.0)) { printf("FAIL: 4 bars at 120 before a bare TEMPO 60 took %.3f s, expected 8.000 (a bare TEMPO must not ramp)\n", a); ok = 0; }
    if (!near(b - a, 8.0)) { printf("FAIL: 2 bars at 60 took %.3f s, expected 8.000\n", b - a); ok = 0; }
    double want = 8.0 * 60 * log(120.0 / 60.0) / (120.0 - 60.0);   // 8 beats on a straight line from 60 to 120 BPM
    if (!near(c - b, want)) { printf("FAIL: the 8-beat ramp 60 -> 120 took %.3f s, expected %.3f\n", c - b, want); ok = 0; }
    puts(ok ? "PASS test_tempo" : "FAIL test_tempo"); return ok ? 0 : 1;
}
