// test_vib: the sampler vibrato switch in the score text and in the sampler.
// Parsing half: "VIB PART: off|on|depth <scale>|rate <hz>" sets p[2] (depth scale code: 0 track default, negative off, positive scale)
// and p[3] (rate override, 0 = the track's) of every note of that part inside that section, wherever the line stands in the section
// (before or after the part's own line), for bar notes and EVENTS notes alike; later words override earlier ones; a line whose
// words are not all VIB words is a part (a part named "VIB X" still reads as a part); untouched notes keep 0.
// Build and run from engine/: clang -O2 -w -o <tmp>/tv test_vib.c && <tmp>/tv <tmp>/score.txt   Exit 0 = pass.
// Mutation checked (each applied to a copy of scoretxt.h, the test run against it, the original untouched):
//   VIB applied to every section        -> A section one: off: the note at beat 0 has p2 2.00 p3 5.20 (a later section's values)
//   VIB applied to every part           -> B section one is untouched: the note at beat 0 has p2 -1.00
//   "off" parsed as scale 0             -> A section one: off: the note at beat 0 has p2 0.00
//   rate not stored                     -> A section two: the note at beat 8 has p3 0.00, expected 5.20
//   unknown words accepted              -> the part "VIB X" is read as a VIB line: 11 notes instead of 12
//   sampler-only rule off               -> C is not a sampler part: the note at beat 16 has p2 3.00 p3 7.00
#define main engine_main   // render.h carries the renderer's main; the test has its own
#include "render.h"
#undef main
#include "scoretxt.h"
void city_setup(void) {}
double city_loop(void) { return 0; }
void city_full(void) {}
static int g_a, g_b, g_x, g_c;
static int map(const char *p, double *tp, double *vs, double *dp) {
    *tp = 0; *vs = 1;
    if (!strcmp(p, "A")) return g_a;
    if (!strcmp(p, "B")) return g_b;
    if (!strcmp(p, "VIB X")) return g_x;
    if (!strcmp(p, "C")) return g_c;
    return -1;
}
static int fails;
static void expect(int tr, double beat, double p2, double p3, const char *what) {
    for (int i = 0; i < g_nnote; i++) { Note *n = &g_note[i];
        if (n->tr == tr && fabs(n->beat - beat) < 1e-6) {
            if (fabs(n->p[2] - p2) > 1e-9 || fabs(n->p[3] - p3) > 1e-9) { printf("FAIL: %s: note at beat %.2f has p2 %.2f p3 %.2f, expected %.2f %.2f\n", what, beat, n->p[2], n->p[3], p2, p3); fails++; }
            return; } }
    printf("FAIL: %s: no note at beat %.2f of track %d\n", what, beat, tr); fails++;
}
int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "test_vib_score.txt";
    FILE *f = fopen(path, "w"); if (!f) { perror(path); return 2; }
    fputs("TEMPO 120\n"
          "SECTION ONE BARS 2 KEY C\n"
          "VIB A: off\n"                       // before the part's own line
          "A: C4:4 | D4:4\n"
          "B: E4:4 | F4:4\n"
          "EVENTS A: 1.5,G4,1,0.8\n"           // an EVENTS note is covered like a bar note
          "SECTION TWO BARS 2 KEY C\n"
          "A: C5:4 | D5:4\n"
          "B: E5:4 | F5:4\n"
          "VIB A: depth 0.5 rate 5.2\n"        // after the part's own lines
          "VIB B: on rate 6\n"
          "SECTION THREE BARS 1 KEY C\n"
          "A: C4:4\n"
          "VIB A: off depth 2\n"               // the later word wins
          "C: C4:4\n"
          "VIB C: depth 3 rate 7\n"            // C is not a sampler part: the line must not touch its notes
          "VIB X: C4:4\n", f);                  // a part named "VIB X", not a VIB line
    fclose(f);
    g_a = syn_track("a", syn_sampler); g_b = syn_track("b", syn_sampler); g_x = syn_track("x", syn_sampler); g_c = syn_track("c", 0);
    score_txt(path, map, 0); remove(path);
    if (g_nnote != 12) { printf("FAIL: expected 12 notes, got %d\n", g_nnote); puts("FAIL test_vib"); return 1; }
    expect(g_a, 0, -1, 0, "A section one: off"); expect(g_a, 4, -1, 0, "A section one: off"); expect(g_a, 1.5, -1, 0, "A section one: the EVENTS note is covered");
    expect(g_b, 0, 0, 0, "B section one is untouched"); expect(g_b, 4, 0, 0, "B section one is untouched");
    expect(g_a, 8, 0.5, 5.2, "A section two: depth 0.5 rate 5.2"); expect(g_a, 12, 0.5, 5.2, "A section two: depth 0.5 rate 5.2");
    expect(g_b, 8, 1, 6, "B section two: on rate 6"); expect(g_b, 12, 1, 6, "B section two: on rate 6");
    expect(g_a, 16, 2, 0, "A section three: depth 2 overrides off");
    expect(g_c, 16, 0, 0, "C is not a sampler part: its VIB line is ignored");
    expect(g_x, 16, 0, 0, "the part VIB X is a part, not a VIB line");
    puts(fails ? "FAIL test_vib" : "PASS test_vib"); return fails ? 1 : 0;
}
