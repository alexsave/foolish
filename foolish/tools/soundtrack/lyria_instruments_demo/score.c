// Lyria sampler instruments, proof render: each instrument plays alone in its own section at 90 BPM through the engine's
// sampler (smp_track), no reverb, no effects, no humanising. Per instrument: every sampled root as a 2-beat note, a slow
// one-octave scale in semitone steps from the lowest root (so the notes between roots are resampled), three 4-beat notes.
// Looping of sustained samples (sp[10]) is on for the cello, haegeum and trumpet; the legato-cut ones get a 25 ms attack
// fade (sp[2]). score.txt is written by make_score.py from the manifests. Render from this folder:
//   clang -O2 -w -o score score.c && SAMPLES=/Users/alex/Dev/soundtrack-samples ENGINE=../engine ./score full out
#include "../engine/orch.h"
#define LM "../samples/manifests/"
static int CELLO, HAEGEUM, TRUMPET;
static int t_map(const char *p, double *tp, double *vs, double *dp) {
    *tp = 0; *vs = 1;
    if (!strcmp(p, "CELLO")) return CELLO;
    if (!strcmp(p, "HAEGEUM")) return HAEGEUM;
    if (!strcmp(p, "TRUMPET")) return TRUMPET;
    return -1;
}
static int lyria_track(const char *name, const char *manifest, int loop) {
    int t = smp_track(name, manifest);
    g_tr[t].sp[0] = 0.25;                 // release
    g_tr[t].sp[10] = loop;                // loop sustained samples
    g_tr[t].sp[2] = 25;                   // attack fade for the legato-cut samples
    g_tr[t].human_ms = 0; g_tr[t].human_vel = 0; g_tr[t].send[0] = 0;
    return t;
}
void city_setup(void) {
    g_on_done = sections_out;
    CELLO = lyria_track("cello", LM "lyria_cello.txt", 1);
    HAEGEUM = lyria_track("haegeum", LM "lyria_haegeum.txt", 1);
    TRUMPET = lyria_track("trumpet", LM "lyria_trumpet.txt", 1);
}
double city_loop(void) { g_nsec = 0; score_txt("score.txt", t_map, 0); return score_total_bars() * 4.0; }
void city_full(void) { city_loop(); }
