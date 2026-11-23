// Warsaw bridge cello renderer (fix round 6): only the engine's own cello section (the two desks of 6_warsaw/score.c), read from score.txt.
// Part CELLO goes to the first desk and, at velocity 0.78/0.85 of it, to the second. TEMPO 60 in the score (a beat is a second).
// vc_sus.txt here is ../samples/manifests/vc_sus.txt with its sample paths moved to /Users/alex/Dev/soundtrack-samples (the original points into a deleted scratchpad).
// No sec_rit and no timing humanise: a note sounds at the written second (the sampler's own 35 ms early start stays). No random draws of ours.
#include "../engine/orch.h"

static int VC, VC2;
static int w_map(const char *p, double *tp, double *vs, double *dp) {
    *tp = 0; *vs = 1; *dp = 60;
    if (!strcmp(p, "CELLO")) { *vs = 0.85; return VC; }   // the suite doubles the sung line at 0.85 (desk 1) and 0.78 (desk 2)
    return -1;
}
void city_setup(void) {
    g_on_done = 0;
    VC = o_smp("cellos", "vc_sus.txt", 0.45, 1, 0.3, -35, 0.5, -2);
    VC2 = o_double(VC, "cellos2", -0.15);
    if (VC2 <= VC) { fprintf(stderr, "bridge score: the second cello track was not created (VC2 %d, VC %d)\n", VC2, VC); exit(1); }
    for (int t = VC; t <= VC2; t++) { eq(t, 3, 300, 1, -1.5); eq(t, 1, 6500, 0.7, 0); g_tr[t].human_ms = 0; g_tr[t].human_vel = 0; }
    o_hall(2.3, 16);
    g_bus[0].gain_db = -6;
    g_master.cthr = -24; g_master.cratio = 1.6;
    g_dyn_depth = 0.6;
    g_tail_s = 3.5;
}
double city_loop(void) {
    int n0 = g_nnote; g_nsec = 0; score_txt("score.txt", w_map, 0);
    // the section: every CELLO note doubled onto the second desk
    int n1 = g_nnote;
    for (int i = n0; i < n1; i++) { Note a = g_note[i]; if (a.tr != VC) continue; note(VC2, a.beat, a.len, a.pitch, a.vel * 0.78 / 0.85); }
    return score_total_bars() * 4.0;
}
void city_full(void) { city_loop(); }
