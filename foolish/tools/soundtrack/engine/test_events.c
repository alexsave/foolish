// test_events: an "EVENTS PART: beat,pitch,len,vel ..." line places notes at exact positions: the beat is not moved to any
// grid, the length is taken as written (no gate), the velocity is scaled by the part and nothing else, the pitch may be a
// name, a MIDI number or x (the part's drum pitch), positions count from the section's start, and the time in seconds is the
// position through the tempo map.
// Build and run from engine/: clang -O2 -w -o <tmp>/te test_events.c && <tmp>/te <tmp>/score.txt   Exit 0 = pass.
// Mutation checked: with the beat rounded to a sixteenth the position assertion fails (1.3700 becomes 1.25); with the text
// gate applied to the length the length assertion fails (0.4100 becomes 0.3895); with the section offset dropped the
// second-section assertion fails; with the part's velocity scale dropped the velocity assertion fails; with x read as
// pitch 60 the drum assertion fails.
#define main engine_main   // render.h carries the renderer's main; the test has its own
#include "render.h"
#undef main
#include "scoretxt.h"
void city_setup(void) {}
double city_loop(void) { return 0; }
void city_full(void) {}
static int g_a, g_d;
static int map(const char *p, double *tp, double *vs, double *dp) {
    if (!strcmp(p, "A")) { *tp = 0; *vs = 0.5; return g_a; }
    if (!strcmp(p, "D")) { *dp = 38; *vs = 1; return g_d; }
    return -1;
}
static int near(double a, double b) { return fabs(a - b) < 1e-6; }
int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "test_events_score.txt";
    FILE *f = fopen(path, "w"); if (!f) { perror(path); return 2; }
    fputs("TEMPO 150\nSECTION ONE BARS 2 KEY C\n"
          "EVENTS A: 1.37,C4,0.41,0.8 2.003,61.5,3.2,0.6\n"
          "EVENTS D: 0.118,x,0.25,0.9\n"
          "EVENTS A: 7.9,G3,2.5,1\n"
          "SECTION TWO BARS 1 KEY C\n"
          "EVENTS A: 0.26,E4,0.5,0.4\n", f); fclose(f);
    g_a = syn_track("a", 0); g_d = syn_track("d", 0);
    score_txt(path, map, 0); qsort(g_tempo, g_ntempo, sizeof(TempoPt), tcmp); remove(path);
    int ok = 1;
    if (g_nnote != 5) { printf("FAIL: five events gave %d notes\n", g_nnote); puts("FAIL test_events"); return 1; }
    Note *n = g_note;
    if (!near(n[0].beat, 1.37)) { printf("FAIL: the event at beat 1.37 sits at %.4f (an event must not be moved to a grid)\n", n[0].beat); ok = 0; }
    if (!near(b2t_raw(n[0].beat), 1.37 * 60 / 150)) { printf("FAIL: beat 1.37 at 150 BPM is at %.5f s, expected %.5f\n", b2t_raw(n[0].beat), 1.37 * 60 / 150); ok = 0; }
    if (!near(n[0].len, 0.41)) { printf("FAIL: the event's length 0.41 became %.4f (no gate on events)\n", n[0].len); ok = 0; }
    if (!near(n[0].vel, 0.4) || !near(n[1].vel, 0.3)) { printf("FAIL: velocities 0.8 and 0.6 under a part scale of 0.5 became %.3f and %.3f\n", n[0].vel, n[1].vel); ok = 0; }
    if (!near(n[0].pitch, 60) || !near(n[1].pitch, 61.5)) { printf("FAIL: pitches C4 and 61.5 became %.2f and %.2f\n", n[0].pitch, n[1].pitch); ok = 0; }
    if (!near(n[1].beat, 2.003) || !near(n[1].len, 3.2)) { printf("FAIL: the second event of a line is at %.4f for %.3f, expected 2.003 for 3.2\n", n[1].beat, n[1].len); ok = 0; }
    if (n[2].tr != g_d || !near(n[2].pitch, 38) || !near(n[2].beat, 0.118)) { printf("FAIL: the drum event x is on track %d at pitch %.1f, beat %.3f; expected the drum part's pitch 38 at 0.118\n", n[2].tr, n[2].pitch, n[2].beat); ok = 0; }
    if (!near(n[3].beat, 7.9) || !near(n[3].len, 2.5)) { printf("FAIL: a second EVENTS line of the same part gave beat %.3f length %.3f\n", n[3].beat, n[3].len); ok = 0; }
    if (!near(n[4].beat, 8.26)) { printf("FAIL: an event at 0.26 of the second section sits at beat %.4f, expected 8.26\n", n[4].beat); ok = 0; }
    puts(ok ? "PASS test_events" : "FAIL test_events"); return ok ? 0 : 1;
}
