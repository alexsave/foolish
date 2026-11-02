// Warsaw - a Chopin suite for solo piano (round 3). Score text by Fable (score.txt): impromptu storm, nocturne, heroic
// polonaise, funeral march, coda, and the storm again. The instrument is the Salamander Grand (Yamaha C5, 16 velocity layers).
// Performance rules from the research: the melody (right hand) leads the accompaniment by about 15 ms and sits above it in
// level; the left hand is near-strict; legato pedalling (the damper lifts just after each change and goes down again
// about 100 ms later), written by Fable as the PEDAL part and turned into cc 64 here.
#include "../engine/orch.h"

static int RH, LH, PED, VC, VC2, CB;
static void syn_none(Track *tr, const Note *n, float *L, float *R, long nf) { (void)tr; (void)n; (void)L; (void)R; (void)nf; }
static int w_map(const char *p, double *tp, double *vs, double *dp) {
    *tp = 0; *vs = 1; (void)0;
    if (!strcmp(p, "RH")) return RH;
    if (!strcmp(p, "LH")) { *vs = 0.9; return LH; }
    if (!strcmp(p, "PEDAL")) { *dp = -60; return PED; }   // a negative drum pitch keeps the token length
    return -1;
}

void city_setup(void) {
    g_on_done = sections_out;
    // the two hands are the same piano: same samples, same pedal, the melody hand a touch early and above the accompaniment
    RH = o_smp("rh", OM "salamander.txt", 0.35, 0, 0, -22, 0.3, 0);   // the melody hand 22 ms ahead
    LH = o_smp("lh", OM "salamander.txt", 0.35, 0, 0, 0, 0.3, -5);    // the accompaniment 5 dB under the melody
    for (int t = RH; t <= LH; t++) { g_tr[t].sp[9] = 1; g_tr[t].sp[5] = 0; g_tr[t].sp[1] = 0.35;   /* the Salamander layers already carry the dynamics in their recorded level */ g_tr[t].human_ms = 9; g_tr[t].human_vel = 0.06; }
    g_tr[LH].human_ms = 6; g_tr[RH].width = g_tr[LH].width = 0.55;   // the Salamander pair is spaced omnis (side above mid on single notes); narrowed for mono safety
    PED = syn_track("pedal", syn_none); g_tr[PED].mute = 1;
    eq(RH, 3, 2600, 1.0, 1.0); eq(RH, 5, 5000, 0, 2); eq(LH, 3, 220, 0.9, -1.0);
    // strings only where the music is a song or a march: a cello section sings the lament and the octave nocturne an octave
    // under the piano's top line (Chopin wrote for the cello), cellos and basses hold the funeral march's bass
    VC = o_smp("cellos", OM "vc_sus.txt", 0.45, 1, 0.3, -35, 0.4, 1); VC2 = o_double(VC, "cellos2", -0.15);
    CB = o_smp("basses", OM "cb_sus.txt", 0.5, 1, 0.45, -40, 0.35, 3);
    for (int t = VC; t <= VC2; t++) { eq(t, 3, 300, 1, -1.5); eq(t, 1, 6500, 0.7, 0); }   // warm: the bow noise above 7 kHz sat on top of the piano
    eq(CB, 3, 250, 1, -2);
    o_hall(2.3, 16);
    g_bus[0].gain_db = -6;
    g_master.cthr = -24; g_master.cratio = 1.6;
    g_dyn_depth = 0.6;
}
double city_loop(void) {
    int n0 = g_nnote; g_nsec = 0; score_txt("score.txt", w_map, 0);
    sec_rit(0.12, 2, 4, 0.05);   // breathe: 12% ritardando into every section end, 5% at every 4-bar phrase end
    // left-hand chords rolled upward, 7 ms a note (a pianist's spread, not a machine's block chord)
    for (int i = n0, n1 = g_nnote; i < n1; i++) if (g_note[i].tr == LH) { int below = 0;
        for (int j = n0; j < n1; j++) if (j != i && g_note[j].tr == LH && fabs(g_note[j].beat - g_note[i].beat) < 1e-6 && g_note[j].pitch < g_note[i].pitch) below++;
        g_note[i].beat += below * 0.007 * 54 / 60.0; }
    // PEDAL "x:LEN" -> damper down 0.1 beat after the change, up 0.03 beat after the next one (legato pedalling)
    for (int i = n0; i < g_nnote; i++) if (g_note[i].tr == PED) {
        double b = g_note[i].beat, e = b + g_note[i].len;
        for (int t = RH; t <= LH; t++) { cc(t, b + 0.1, 64, 127); cc(t, e + 0.03, 64, 0); }
    }
    // the sung lines: the top note of each right-hand chord, an octave down, legato into the next
    static const char *sing[] = {"II Nocturne: Song in octaves", "IV Funeral: Lament (the sung trio)", "II Nocturne: Elegy"};
    for (int k = 0; k < 3; k++) { int si = sec_find(sing[k]); if (si < 0) { fprintf(stderr, "no section %s\n", sing[k]); continue; }
        double b0 = g_sec[si].bar0 * 4.0, b1 = b0 + g_sec[si].bars * 4.0; int n1 = g_nnote;
        for (int i = n0; i < n1; i++) { Note a = g_note[i]; if (a.tr != RH || a.beat < b0 - 1e-6 || a.beat >= b1 - 1e-6) continue;
            int top = 1; for (int j = n0; j < n1; j++) if (j != i && g_note[j].tr == RH && fabs(g_note[j].beat - a.beat) < 1e-6 && g_note[j].pitch > a.pitch) top = 0;
            if (!top) continue; double p = a.pitch - 12; while (p < 48) p += 12;
            Note *c = note(VC, a.beat, a.len + 0.08, p, a.vel * 0.85); (void)c; Note *d = note(VC2, a.beat, a.len + 0.08, p, a.vel * 0.78); (void)d; } }
    // the march's bass: the lowest left-hand note of each chord, cellos an octave up, basses where they reach
    static const char *march[] = {"IV Funeral: March", "IV Funeral: Lament (the sung trio)", "V Coda: Collapse"};
    for (int k = 0; k < 3; k++) { int si = sec_find(march[k]); if (si < 0) { fprintf(stderr, "no section %s\n", march[k]); continue; }
        double b0 = g_sec[si].bar0 * 4.0, b1 = b0 + g_sec[si].bars * 4.0; int n1 = g_nnote;
        for (int i = n0; i < n1; i++) { Note a = g_note[i]; if (a.tr != LH || a.beat < b0 - 1e-6 || a.beat >= b1 - 1e-6) continue;
            int low = 1; for (int j = n0; j < n1; j++) if (j != i && g_note[j].tr == LH && fabs(g_note[j].beat - a.beat) < 1e-6 && g_note[j].pitch < a.pitch) low = 0;
            if (!low || a.len < 0.9) continue; double p = a.pitch; while (p < 28) p += 12;
            note(CB, a.beat, a.len + 0.06, p, a.vel * 0.8);
            if (k != 1) { double q = p; while (q < 36) q += 12; note(VC, a.beat, a.len + 0.06, q, a.vel * 0.7); } } }
    return score_total_bars() * 4.0;
}
void city_full(void) { city_loop(); }
